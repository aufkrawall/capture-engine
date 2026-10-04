#include "inject_control_channel.h"

#include "shared_defs.h"
#include "common/logging/logging.h"
#include "common/logging/log_meter.h"

namespace ce::ipc {
namespace {
struct Mapping {
    HANDLE handle = nullptr;
    void* view = nullptr;
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() {
        if (view)
            UnmapViewOfFile(view);
        if (handle)
            CloseHandle(handle);
    }
    bool Open(const wchar_t* name, DWORD access, size_t size) {
        handle = OpenFileMappingW(access, FALSE, name);
        if (handle)
            view = MapViewOfFile(handle, access, 0, 0, size);
        return view != nullptr;
    }
};

class ControlView {
public:
    ControlView(HANDLE expectedProcess, const wchar_t* discoveryName) : expectedProcess_(expectedProcess) {
        if (!discovery_.Open(discoveryName ? discoveryName : SHARED_MEM_DISCOVERY, FILE_MAP_READ, sizeof(DiscoveryInfo))) {
            if (discovery_.handle) result.status = ControlStatus::MappingFailed;
            return;
        }
        const auto* discovery = static_cast<const DiscoveryInfo*>(discovery_.view);
        if (!ValidateDiscoveryInfo(discovery)) {
            result.status = ControlStatus::InvalidDiscovery;
            return;
        }
        result.injectPid = discovery->GetInjectPid();
        if (!result.injectPid)
            return;
        if (expectedProcess_ && (GetProcessId(expectedProcess_) != result.injectPid ||
                                 WaitForSingleObject(expectedProcess_, 0) != WAIT_TIMEOUT)) {
            result.status = ControlStatus::StaleTarget;
            return;
        }
        wchar_t name[64]{};
        GenerateSharedMemName(name, 64, result.injectPid);
        if (!memory_.Open(name, FILE_MAP_READ | FILE_MAP_WRITE, sizeof(SharedMemoryLayout))) {
            if (memory_.handle) result.status = ControlStatus::MappingFailed;
            return;
        }
        auto* memory = static_cast<SharedMemoryLayout*>(memory_.view);
        if (!ValidateSharedMemory(memory)) {
            result.status = ControlStatus::InvalidSharedMemory;
            return;
        }
        // Keep discovery mapped and recheck after opening the payload; no mapping is cached.
        if (!ValidateDiscoveryInfo(discovery) || discovery->GetInjectPid() != result.injectPid ||
            (expectedProcess_ && WaitForSingleObject(expectedProcess_, 0) != WAIT_TIMEOUT)) {
            result.status = ControlStatus::StaleTarget;
            return;
        }
        state = &memory->runtimeState;
        result.status = ControlStatus::Ok;
    }
    ~ControlView() {
        static ce::log_meter::ChangeGate gate;
        const auto verdict = gate.Observe(ce::log_meter::FieldKey(result.status, result.injectPid));
        if (verdict && result.status != ControlStatus::Ok && result.status != ControlStatus::Unavailable)
            LogWarn("[InjectControl] transaction rejected status=%u pid=%u (+%llu unchanged)",
                    static_cast<unsigned>(result.status), result.injectPid,
                    static_cast<unsigned long long>(verdict.suppressed));
    }
    ControlResult result;
    CaptureState* state = nullptr;

private:
    HANDLE expectedProcess_;
    Mapping discovery_;
    Mapping memory_;
};
}  // namespace

ControlResult InjectControlChannel::PublishRecordingIntent(RecordingStartIntent intent) const {
    ControlView view(expectedProcess_, discoveryName_);
    if (view.state) {
        view.state->SetRecordingStartIntent(intent);
        view.state->audioOnly.store(intent == RecordingStartIntent::AudioOnly, std::memory_order_release);
    }
    return view.result;
}

ControlResult InjectControlChannel::PublishNotification(OverlayNotificationType type, uint64_t expiry) const {
    ControlView view(expectedProcess_, discoveryName_);
    if (view.state) {
        if (type == OverlayNotificationType::None) {
            view.state->notificationExpiry.store(0, std::memory_order_release);
            view.state->notificationType.store(0, std::memory_order_release);
        } else {
            view.state->notificationType.store(static_cast<uint32_t>(type), std::memory_order_release);
            view.state->notificationExpiry.store(expiry, std::memory_order_release);
        }
    }
    return view.result;
}

ControlResult InjectControlChannel::ReadRecordingHealth(RecordingHealthObservation& observation) const {
    observation = {};
    ControlView view(expectedProcess_, discoveryName_);
    if (view.state) {
        observation.failure = view.state->recordingFailureCode.load(std::memory_order_acquire);
        observation.live = view.state->isRecording.load(std::memory_order_acquire);
        observation.liveSince = view.state->recordingStartTime.load(std::memory_order_acquire);
        observation.audioOnly = view.state->audioOnly.load(std::memory_order_acquire);
    }
    return view.result;
}

ControlResult InjectControlChannel::ConsumeRecordingFailure(uint32_t expectedFailure) const {
    ControlView view(expectedProcess_, discoveryName_);
    if (view.state)
        view.state->recordingFailureCode.compare_exchange_strong(
            expectedFailure, static_cast<uint32_t>(RecordingFailureCode::None), std::memory_order_acq_rel);
    return view.result;
}

ControlResult InjectControlChannel::ClearDeadMediaState() const {
    ControlView view(expectedProcess_, discoveryName_);
    if (view.state) {
        view.state->SetRecordingStartIntent(RecordingStartIntent::Idle);
        view.state->captureRequested.store(false, std::memory_order_release);
        view.state->isRecording.store(false, std::memory_order_release);
        view.state->recordingStartTime.store(0, std::memory_order_release);
    }
    return view.result;
}

}  // namespace ce::ipc
