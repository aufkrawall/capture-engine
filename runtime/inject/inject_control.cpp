#include "inject_control.h"

#include <windows.h>

#include "common/logging/logging.h"

namespace ce::runtime {

namespace {

// The wording the hotkey path has always logged for each toggle, kept verbatim so existing log
// searches and analyzers keep working.
struct ToggleText {
    const char* notConnected;
    const char* rejected;
    const char* handled;
    const char* blockLabel;
};

const ToggleText& TextFor(ProcessCommand command) {
    static const ToggleText overlay{"Overlay toggle hotkey pressed, but no inject process is connected",
                                    "Inject process did not accept the overlay toggle",
                                    "Overlay toggle hotkey handled", "overlay toggle IPC"};
    static const ToggleText benchmark{"Benchmark hotkey pressed, but no inject process is connected",
                                      "Inject process did not accept the benchmark toggle",
                                      "Benchmark toggle hotkey handled", "benchmark toggle IPC"};
    return command == ProcessCommand::ToggleBenchmark ? benchmark : overlay;
}

// Reports a caller that sat in a blocking IPC round trip for InjectControl::kBlockWarnThresholdMs or more.
// Same message the controller's MainThreadBlockTimer wrote, so the diagnostic survives the move; it lives
// here because this component owns the blocking call.
class BlockedCallerTimer {
public:
    explicit BlockedCallerTimer(const char* label) : label_(label), startMs_(GetTickCount64()) {}
    ~BlockedCallerTimer() {
        const ULONGLONG elapsedMs = GetTickCount64() - startMs_;
        if (InjectControl::ShouldWarnBlocked(elapsedMs)) {
            LogWarn("[Controller] Main-thread blocked %llums in %s — tray + global hotkeys were unresponsive this long",
                    static_cast<unsigned long long>(elapsedMs), label_);
        }
    }
    BlockedCallerTimer(const BlockedCallerTimer&) = delete;
    BlockedCallerTimer& operator=(const BlockedCallerTimer&) = delete;

private:
    const char* label_;
    ULONGLONG startMs_;
};

}  // namespace

ToggleOutcome InjectControl::ToggleOverlay() {
    return Toggle(ProcessCommand::ToggleOverlay);
}

ToggleOutcome InjectControl::ToggleBenchmark() {
    return Toggle(ProcessCommand::ToggleBenchmark);
}

ToggleOutcome InjectControl::Toggle(ProcessCommand command) {
    const ToggleText& text = TextFor(command);
    if (!link_.Ready()) {
        LogWarn("[Controller] %s", text.notConnected);
        return ToggleOutcome::NotConnected;
    }

    BlockedCallerTimer blocked(text.blockLabel);
    ProcessResponse response = ProcessResponse::Error;
    if (!link_.Send(command, response, kToggleTimeoutMs) || response == ProcessResponse::Error) {
        LogError("[Controller] %s", text.rejected);
        return ToggleOutcome::Rejected;
    }
    LogInfo("[Controller] %s", text.handled);
    return ToggleOutcome::Accepted;
}

ce::ipc::ControlResult InjectControl::Notify(OverlayNotificationType type, uint64_t expiryTick) {
    return link_.Present() ? link_.Channel().PublishNotification(type, expiryTick) : ce::ipc::ControlResult{};
}

ce::ipc::ControlResult InjectControl::PublishRecordingIntent(RecordingStartIntent intent) {
    return link_.Present() ? link_.Channel().PublishRecordingIntent(intent) : ce::ipc::ControlResult{};
}

ce::ipc::ControlResult InjectControl::ReadHealth(ce::ipc::RecordingHealthObservation& observation) {
    observation = {};
    return link_.Present() ? link_.Channel().ReadRecordingHealth(observation) : ce::ipc::ControlResult{};
}

void InjectControl::ClearMediaFailure(uint32_t failure, bool mediaGone) {
    if (!link_.Present())
        return;
    const ce::ipc::InjectControlChannel channel = link_.Channel();
    if (failure)
        channel.ConsumeRecordingFailure(failure);
    if (mediaGone)
        channel.ClearDeadMediaState();
}

}  // namespace ce::runtime
