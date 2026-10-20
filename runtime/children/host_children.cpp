#include "host_children.h"
#include "child_process_lifecycle.h"
#include "media_finalizations.h"
#include "common/ipc/recording_stop_command.h"
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
    NativeHostChildren(const char* configPath, void (*pump)(), bool (*accept)(), const wchar_t* executable)
        : configPath_(configPath ? configPath : ""),
          executable_(executable ? executable : L""),
          pump_(pump),
          accept_(accept),
          lifecycle_(*this) {}

    bool Ensure(ChildRole role, uint32_t timeoutMs) {
        if (pendingServices_ && (role == ChildRole::Logger || role == ChildRole::Sensors))
            return false;
        if (sensorsHeld_ && role == ChildRole::Sensors)
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
    void RetireMedia(uint64_t recordingRequest) {
        // Stamped before the retirement, so an exit that completes during it is already attributed.
        if (const ChildToken process = lifecycle_.Process(ChildRole::Media))
            finalizations_.Retire(process, recordingRequest);
        lifecycle_.Retire(ChildRole::Media);
    }
    std::vector<FinalizedMedia> CollectFinalizedMedia() {
        return finalizations_.Take();
    }
    uint32_t FinalizingCount() const {
        // Every tracked media child except the active one is retired and still running.
        const size_t active = lifecycle_.Present(ChildRole::Media) ? 1 : 0;
        const size_t tracked = finalizations_.Tracked();
        return static_cast<uint32_t>(tracked > active ? tracked - active : 0);
    }
    ce::ipc::CommandOutcome StopRecording(ChildRole role, uint32_t timeout) {
        return ce::ipc::RequestChildRecordingStop(Client(role), timeout);
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
    void Service(AuxiliaryServices wanted, const std::function<void()>& beforeRecovery) {
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
        // A sensor helper held for a driver setup is not recovered; the setup's end resumes it on the next tick.
        const std::array<bool, 4> expected{true, lifecycle_.Present(ChildRole::Media), wanted.logger,
                                           wanted.sensors && !sensorsHeld_};
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
    // Holds before the stop, so nothing can restart the helper while it exits.
    bool StopSensorsForSetup() {
        if (!sensorsHeld_)
            LogInfo("[HostChildren] sensor helper held stopped for driver setup");
        sensorsHeld_ = true;
        const auto process = lifecycle_.Process(ChildRole::Sensors);
        if (!process)
            return true;
        if (!Terminate(process))
            return false;
        Wait(&process, 1, UINT32_MAX);
        lifecycle_.Retire(ChildRole::Sensors);
        return !lifecycle_.HasRetired(ChildRole::Sensors);
    }
    void ResumeSensorsAfterSetup() {
        if (sensorsHeld_)
            LogInfo("[HostChildren] sensor helper released after driver setup");
        sensorsHeld_ = false;
    }
    // Never terminates a helper (DR-17): a retired media child may still be finalizing a recording, for as
    // long as its writer keeps advancing. A client that stops waiting keeps the runtime and its ownership.
    bool Shutdown() {
        lifecycle_.BeginShutdown();
        const bool drained = lifecycle_.DrainUntilExited(10000, [this](uint64_t elapsedMs) {
            LogWarn("[HostChildren] shutdown still waiting for helper exit after %llu ms (inject=%d media=%zu "
                    "logger=%d sensor=%d); helpers are never terminated",
                    static_cast<unsigned long long>(elapsedMs), lifecycle_.HasRetired(ChildRole::Inject),
                    finalizations_.Tracked(), lifecycle_.HasRetired(ChildRole::Logger),
                    lifecycle_.HasRetired(ChildRole::Sensors));
        });
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
        // A media child inherits a result slot that the spawner keeps until the child has been closed.
        std::unique_ptr<ce::media_result::ResultSlot> slot;
        if (role == ChildRole::Media)
            slot = ce::media_result::ResultSlot::Create();
        const ChildToken process = reinterpret_cast<ChildToken>(
            SpawnChildProcess(kModes[Index(role)], configPath_.c_str(), Client(role),
                              executable_.empty() ? nullptr : executable_.c_str(), slot.get()));
        if (process && role == ChildRole::Media)
            finalizations_.Adopt(process, std::move(slot));
        return process;
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
        // The exit code and the result slot are only readable until the handle is closed.
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(process), &exitCode))
            LogWarn("[HostChildren] helper exit code unavailable (error=%lu)", GetLastError());
        finalizations_.Closing(process, exitCode);
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
    std::wstring executable_;
    void (*pump_)();
    bool (*accept_)();
    ProcessIPCClient inject_{ProcessMode::Inject};
    ProcessIPCClient media_{ProcessMode::Media};
    ChildProcessLifecycle lifecycle_;
    MediaFinalizations finalizations_;
    HANDLE auxiliaryShutdown_ = nullptr;
    AuxiliaryServices wanted_;
    uint64_t lastHealthMs_ = 0;
    std::array<bool, 4> recoveryFailure_{};
    bool pendingServices_ = false;
    bool sensorsHeld_ = false;  // a driver setup holds the sensor helper stopped (StopSensorsForSetup)
};
}  // namespace ce::runtime::detail

namespace ce::runtime {
namespace {
detail::ChildRole Role(HostChild child) {
    return static_cast<detail::ChildRole>(child);
}
}  // namespace

class ChildSupervisor::Impl final : public detail::NativeHostChildren {
public:
    using detail::NativeHostChildren::NativeHostChildren;
};

ChildSupervisor::ChildSupervisor(const char* configPath, void (*pump)(), bool (*accept)(), const wchar_t* executable)
    : impl_(std::make_unique<Impl>(configPath, pump, accept, executable)) {}
ChildSupervisor::~ChildSupervisor() {
    bool stopped = false;
    try {
        stopped = impl_->Shutdown();
    } catch (...) {
        OutputDebugStringA("[HostChildren] shutdown threw; retaining helper ownership\n");
    }
    if (!stopped) {
        // A live helper must keep an owner: dropping the state would orphan the process and its
        // endpoint. The retained state is intentionally never freed.
        static_cast<void>(impl_.release());
    }
}
bool ChildSupervisor::Ensure(HostChild child, uint32_t timeout) {
    return impl_->Ensure(Role(child), timeout);
}
bool ChildSupervisor::Ready(HostChild child) const {
    return impl_->Ready(Role(child));
}
bool ChildSupervisor::Present(HostChild child) const {
    return impl_->Present(Role(child));
}
bool ChildSupervisor::Running(HostChild child) const {
    return impl_->Running(Role(child));
}
bool ChildSupervisor::Send(HostChild child, ProcessCommand command, const char* payload, ProcessResponse* response,
                           uint32_t timeout) {
    return impl_->Send(Role(child), command, payload, response, timeout);
}
void ChildSupervisor::SendToAll(ProcessCommand command) {
    Send(HostChild::Inject, command);
    Send(HostChild::Media, command);
}
void ChildSupervisor::RetireMedia(uint64_t recordingRequest) {
    impl_->RetireMedia(recordingRequest);
}
std::vector<FinalizedMedia> ChildSupervisor::CollectFinalizedMedia() {
    return impl_->CollectFinalizedMedia();
}
uint32_t ChildSupervisor::FinalizingCount() const {
    return impl_->FinalizingCount();
}
ce::ipc::CommandOutcome ChildSupervisor::StopRecording(HostChild child, uint32_t timeout) {
    return impl_->StopRecording(Role(child), timeout);
}
void ChildSupervisor::Service(AuxiliaryServices services, const std::function<void()>& beforeRecovery) {
    impl_->Service(services, beforeRecovery);
}
void ChildSupervisor::ReconfigureServices(AuxiliaryServices services) {
    impl_->Reconfigure(services);
}
bool ChildSupervisor::StopSensorsForSetup() {
    return impl_->StopSensorsForSetup();
}
void ChildSupervisor::ResumeSensorsAfterSetup() {
    impl_->ResumeSensorsAfterSetup();
}
bool ChildSupervisor::Shutdown() {
    return impl_->Shutdown();
}
ce::ipc::ControlResult ChildSupervisor::PublishRecordingIntent(RecordingStartIntent intent) {
    return impl_->Present(detail::ChildRole::Inject) ? impl_->InjectControl().PublishRecordingIntent(intent)
                                                     : ce::ipc::ControlResult{};
}
ce::ipc::ControlResult ChildSupervisor::PublishNotification(OverlayNotificationType type, uint64_t expiry) {
    return impl_->Present(detail::ChildRole::Inject) ? impl_->InjectControl().PublishNotification(type, expiry)
                                                     : ce::ipc::ControlResult{};
}
ce::ipc::ControlResult ChildSupervisor::ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) {
    observation = {};
    return impl_->Present(detail::ChildRole::Inject) ? impl_->InjectControl().ReadRecordingHealth(observation)
                                                     : ce::ipc::ControlResult{};
}
ce::ipc::InjectControlChannel ChildSupervisor::InjectChannel() const {
    return impl_->InjectControl();
}
void ChildSupervisor::ClearMediaFailure(uint32_t failure, bool mediaGone) {
    if (!impl_->Present(detail::ChildRole::Inject))
        return;
    const auto channel = impl_->InjectControl();
    if (failure)
        channel.ConsumeRecordingFailure(failure);
    if (mediaGone)
        channel.ClearDeadMediaState();
}
}  // namespace ce::runtime
