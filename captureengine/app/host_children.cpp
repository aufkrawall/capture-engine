#include "host_children.h"
#include "child_process_lifecycle.h"
#include "child_recording_stop.h"
#include "common/logging/logging.h"

#include <string>

namespace ce::runtime::detail {
namespace {
constexpr std::array<ProcessMode, 4> kModes{ProcessMode::Inject, ProcessMode::Media, ProcessMode::Logger,
                                            ProcessMode::Sensors};
constexpr std::array<const char*, 4> kNames{"inject", "media", "logger", "sensor"};
}  // namespace

class NativeHostChildren : public ChildProcessEffects {
public:
    NativeHostChildren(const char* configPath, void (*pump)(), bool (*accept)())
        : configPath_(configPath ? configPath : ""),
          pump_(pump),
          accept_(accept),
          lifecycle_(*this) {}

    bool Ensure(ChildRole role, uint32_t timeoutMs) {
        if (pendingServices_ && (role == ChildRole::Logger || role == ChildRole::Sensors))
            return false;
        const uint64_t generation = lifecycle_.Generation(role);
        const bool ready = lifecycle_.Ensure(role, timeoutMs);
        if (ready && generation != lifecycle_.Generation(role)) {
            LogInfo("[HostChildren] %s ready (generation=%llu)", kNames[Index(role)],
                    static_cast<unsigned long long>(lifecycle_.Generation(role)));
        }
        return ready;
    }
    bool Ready(ChildRole role) const {
        return lifecycle_.Ready(role);
    }
    bool Present(ChildRole role) const {
        return lifecycle_.Present(role);
    }
    bool Running(ChildRole role) const {
        return lifecycle_.Running(role);
    }
    bool Send(ChildRole role, ProcessCommand command, const char* payload, ProcessResponse* response,
              uint32_t timeout) {
        auto* client = Client(role);
        return lifecycle_.Ready(role) && client && client->SendCommand(command, payload, response, timeout);
    }
    void RetireMedia() {
        lifecycle_.Retire(ChildRole::Media);
    }
    ce::controller::CommandOutcome StopRecording(ChildRole role, uint32_t timeout) {
        return ce::controller::detail::RequestChildRecordingStop(Client(role), timeout);
    }

    void Reconfigure(AuxiliaryServices wanted) {
        wanted_ = wanted;
        const bool logger = lifecycle_.Running(ChildRole::Logger);
        const bool sensor = lifecycle_.Running(ChildRole::Sensors);
        if (!pendingServices_ && logger == wanted.logger && sensor == wanted.sensors &&
            !(sensor && wanted.sensors && wanted.restartSensors)) {
            lifecycle_.CollectRetired();
            return;
        }
        if (!pendingServices_) {
            LogInfo("[HostChildren] reconfigure services (logger=%d sensors=%d)", wanted.logger, wanted.sensors);
            SignalAuxiliaryShutdown();
            lifecycle_.Retire(ChildRole::Logger);
            lifecycle_.Retire(ChildRole::Sensors);
            pendingServices_ = true;
        }
        CompleteServiceRetirement();
    }
    void Service(AuxiliaryServices wanted, void (*beforeRecovery)()) {
        lifecycle_.CollectRetired();
        if (pendingServices_) {
            wanted_ = wanted;
            CompleteServiceRetirement();
        }
        const uint64_t now = NowMs();
        if (now - lastHealthMs_ < 1000)
            return;
        lastHealthMs_ = now;
        if (beforeRecovery)
            beforeRecovery();
        const std::array<bool, 4> expected{true, lifecycle_.Present(ChildRole::Media), wanted.logger, wanted.sensors};
        for (size_t i = 0; i < expected.size(); ++i) {
            if (!expected[i])
                continue;
            const auto role = static_cast<ChildRole>(i);
            if (Ensure(role, 2000)) {
                recoveryFailure_[i] = false;
            } else if (!recoveryFailure_[i] && AcceptingWork()) {
                LogWarn("[HostChildren] %s recovery pending; retaining existing process identity", kNames[i]);
                recoveryFailure_[i] = true;
            }
        }
    }
    bool StopSensors() {
        const auto process = lifecycle_.Process(ChildRole::Sensors);
        if (!process)
            return true;
        if (!Terminate(process))
            return false;
        Wait(&process, 1, UINT32_MAX);
        lifecycle_.Retire(ChildRole::Sensors);
        return !lifecycle_.HasRetired(ChildRole::Sensors);
    }
    bool Shutdown() {
        lifecycle_.BeginShutdown();
        const bool drained = lifecycle_.Drain(10000, true);
        if (drained && auxiliaryShutdown_) {
            CloseHandle(auxiliaryShutdown_);
            auxiliaryShutdown_ = nullptr;
        }
        if (!drained)
            LogError("[HostChildren] shutdown incomplete; a helper process has not exited");
        return drained;
    }
    ce::ipc::InjectControlChannel InjectControl() const {
        return ce::ipc::InjectControlChannel(reinterpret_cast<HANDLE>(lifecycle_.Process(ChildRole::Inject)));
    }

private:
    static size_t Index(ChildRole role) {
        return static_cast<size_t>(role);
    }
    ProcessIPCClient* Client(ChildRole role) {
        if (role == ChildRole::Inject)
            return &inject_;
        if (role == ChildRole::Media)
            return &media_;
        return nullptr;
    }
    const ProcessIPCClient* Client(ChildRole role) const {
        return const_cast<NativeHostChildren*>(this)->Client(role);
    }
    void SignalAuxiliaryShutdown() {
        if (!auxiliaryShutdown_) {
            wchar_t name[64]{};
            GenerateShutdownEventName(name, 64, GetCurrentProcessId());
            auxiliaryShutdown_ = CreateEventW(nullptr, TRUE, FALSE, name);
        }
        if (auxiliaryShutdown_)
            SetEvent(auxiliaryShutdown_);
    }
    void CompleteServiceRetirement() {
        lifecycle_.CollectRetired();
        if (lifecycle_.HasRetired(ChildRole::Logger) || lifecycle_.HasRetired(ChildRole::Sensors))
            return;
        if (auxiliaryShutdown_) {
            ResetEvent(auxiliaryShutdown_);
            CloseHandle(auxiliaryShutdown_);
            auxiliaryShutdown_ = nullptr;
        }
        pendingServices_ = false;
        if (wanted_.logger)
            Ensure(ChildRole::Logger, 0);
        if (wanted_.sensors)
            Ensure(ChildRole::Sensors, 0);
    }

    ChildToken Spawn(ChildRole role) override {
        return reinterpret_cast<ChildToken>(SpawnChildProcess(kModes[Index(role)], configPath_.c_str(), Client(role)));
    }
    bool Running(ChildToken process) const override {
        // An observation error is not evidence of exit.
        return WaitForSingleObject(reinterpret_cast<HANDLE>(process), 0) != WAIT_OBJECT_0;
    }
    bool Connected(ChildRole role) const override {
        const auto* client = Client(role);
        return !client || client->IsConnected();
    }
    void Disconnect(ChildRole role) override {
        if (auto* client = Client(role))
            client->Disconnect();
    }
    void RequestShutdown(ChildRole role) override {
        if (auto* client = Client(role)) {
            if (client->IsConnected())
                client->SendCommand(ProcessCommand::Shutdown);
        } else {
            SignalAuxiliaryShutdown();
        }
    }
    void Close(ChildToken process) override {
        CloseHandle(reinterpret_cast<HANDLE>(process));
    }
    bool Terminate(ChildToken process) override {
        if (!Running(process))
            return true;
        const bool terminated = TerminateProcess(reinterpret_cast<HANDLE>(process), 1) != FALSE;
        if (!terminated)
            LogError("[HostChildren] cannot terminate helper (error=%lu)", GetLastError());
        return terminated;
    }
    uint64_t NowMs() const override {
        return GetTickCount64();
    }
    ChildWait Wait(const ChildToken* processes, size_t count, uint32_t timeoutMs) override {
        std::array<HANDLE, MAXIMUM_WAIT_OBJECTS - 1> handles{};
        const size_t watched = (std::min)(count, handles.size());
        for (size_t i = 0; i < watched; ++i)
            handles[i] = reinterpret_cast<HANDLE>(processes[i]);
        const DWORD n = static_cast<DWORD>(watched);
        const DWORD result =
            pump_ ? MsgWaitForMultipleObjectsEx(n, handles.data(), timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                  : WaitForMultipleObjects(n, handles.data(), FALSE, timeoutMs);
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + n)
            return ChildWait::Exited;
        if (pump_ && result == WAIT_OBJECT_0 + n)
            return ChildWait::Messages;
        return result == WAIT_TIMEOUT ? ChildWait::Timeout : ChildWait::Failed;
    }
    void PumpMessages() override {
        if (pump_)
            pump_();
    }
    bool AcceptingWork() const override {
        return !accept_ || accept_();
    }

    std::string configPath_;
    void (*pump_)();
    bool (*accept_)();
    ProcessIPCClient inject_{ProcessMode::Inject};
    ProcessIPCClient media_{ProcessMode::Media};
    ChildProcessLifecycle lifecycle_;
    HANDLE auxiliaryShutdown_ = nullptr;
    AuxiliaryServices wanted_;
    uint64_t lastHealthMs_ = 0;
    std::array<bool, 4> recoveryFailure_{};
    bool pendingServices_ = false;
};
}  // namespace ce::runtime::detail

namespace ce::runtime {
namespace {
detail::NativeHostChildren* activeChildren = nullptr;
bool retainedAfterFailedShutdown = false;
detail::ChildRole Role(HostChild child) {
    return static_cast<detail::ChildRole>(child);
}
}  // namespace

class HostChildrenSession::Impl final : public detail::NativeHostChildren {
public:
    using detail::NativeHostChildren::NativeHostChildren;
};

HostChildrenSession::HostChildrenSession(const char* configPath, void (*pump)(), bool (*accept)()) {
    if (activeChildren) {
        LogError("[HostChildren] refusing a second host child owner");
        return;
    }
    impl_ = std::make_unique<Impl>(configPath, pump, accept);
    activeChildren = impl_.get();
}
HostChildrenSession::~HostChildrenSession() {
    if (impl_) {
        bool stopped = false;
        try {
            stopped = impl_->Shutdown();
        } catch (...) {
            OutputDebugStringA("[HostChildren] shutdown threw; retaining helper ownership for retry\n");
        }
        if (stopped) {
            activeChildren = nullptr;
        } else {
            // Shutdown failed: preserve a retryable owner, not an untracked
            // live process. A second host cannot replace this context.
            retainedAfterFailedShutdown = true;
            activeChildren = impl_.release();
        }
    }
}
bool HostChildrenSession::IsReady() const {
    return impl_ != nullptr;
}
bool EnsureHostChild(HostChild child, uint32_t timeout) {
    return activeChildren && activeChildren->Ensure(Role(child), timeout);
}
bool HostChildReady(HostChild child) {
    return activeChildren && activeChildren->Ready(Role(child));
}
bool HostChildPresent(HostChild child) {
    return activeChildren && activeChildren->Present(Role(child));
}
bool HostChildRunning(HostChild child) {
    return activeChildren && activeChildren->Running(Role(child));
}
bool SendHostChildCommand(HostChild child, ProcessCommand command, const char* payload, ProcessResponse* response,
                          uint32_t timeout) {
    return activeChildren && activeChildren->Send(Role(child), command, payload, response, timeout);
}
void SendHostCommandToAll(ProcessCommand command) {
    SendHostChildCommand(HostChild::Inject, command);
    SendHostChildCommand(HostChild::Media, command);
}
void RetireHostMedia() {
    if (activeChildren)
        activeChildren->RetireMedia();
}
ce::controller::CommandOutcome StopHostChildRecording(HostChild child, uint32_t timeout) {
    return activeChildren ? activeChildren->StopRecording(Role(child), timeout)
                          : ce::controller::CommandOutcome::AcknowledgementUnknown;
}
void ServiceHostChildren(AuxiliaryServices services, void (*beforeRecovery)()) {
    if (activeChildren)
        activeChildren->Service(services, beforeRecovery);
}
void ReconfigureHostServices(AuxiliaryServices services) {
    if (activeChildren)
        activeChildren->Reconfigure(services);
}
bool StopHostSensorsForSetup() {
    return !activeChildren || activeChildren->StopSensors();
}
bool ShutdownHostChildren() {
    if (!activeChildren)
        return true;
    const bool stopped = activeChildren->Shutdown();
    if (stopped && retainedAfterFailedShutdown) {
        delete activeChildren;
        activeChildren = nullptr;
        retainedAfterFailedShutdown = false;
    }
    return stopped;
}
ce::ipc::ControlResult PublishHostRecordingIntent(RecordingStartIntent intent) {
    return activeChildren && activeChildren->Present(detail::ChildRole::Inject)
               ? activeChildren->InjectControl().PublishRecordingIntent(intent)
               : ce::ipc::ControlResult{};
}
ce::ipc::ControlResult PublishHostNotification(OverlayNotificationType type, uint64_t expiry) {
    return activeChildren && activeChildren->Present(detail::ChildRole::Inject)
               ? activeChildren->InjectControl().PublishNotification(type, expiry)
               : ce::ipc::ControlResult{};
}
ce::ipc::ControlResult ReadHostRecordingHealth(ce::ipc::RecordingHealthObservation& observation) {
    observation = {};
    return activeChildren && activeChildren->Present(detail::ChildRole::Inject)
               ? activeChildren->InjectControl().ReadRecordingHealth(observation)
               : ce::ipc::ControlResult{};
}
void ClearHostMediaFailure(uint32_t failure, bool mediaGone) {
    if (!activeChildren || !activeChildren->Present(detail::ChildRole::Inject))
        return;
    const auto channel = activeChildren->InjectControl();
    if (failure)
        channel.ConsumeRecordingFailure(failure);
    if (mediaGone)
        channel.ClearDeadMediaState();
}
}  // namespace ce::runtime
