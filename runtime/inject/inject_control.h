#pragma once

#include <cstdint>

#include "common/ipc/inject_control_channel.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/shared_defs.h"

namespace ce::runtime {

// What InjectControl needs from whoever owns the helper processes. The runtime core adapts its child
// supervisor to this; the component itself reaches no other runtime subsystem (DR-21).
class InjectLink {
public:
    virtual ~InjectLink() = default;
    // An inject process identity exists (running or not). Shared-memory operations need only this.
    virtual bool Present() const = 0;
    // The identity is running and its authenticated command channel is connected.
    virtual bool Ready() const = 0;
    // The shared-memory control channel bound to the CURRENT inject identity; only called while
    // Present(). A dead or replaced child is rejected by the channel itself.
    virtual ce::ipc::InjectControlChannel Channel() const = 0;
    // One authenticated command round trip. False when it was not delivered or not answered in time.
    virtual bool Send(ProcessCommand command, ProcessResponse& response, uint32_t timeoutMs) = 0;
};

// How a toggle request ended. Both failures map to the public CE_E_IPC; they stay distinct because
// the first is "nothing to talk to" (a normal state without a game) and the second a refusal.
enum class ToggleOutcome : uint8_t { Accepted, NotConnected, Rejected };

// The controller's whole conversation with the inject helper: the overlay/benchmark toggles
// (authenticated command channel) and the low-frequency shared-memory transactions (recording
// intent, in-game notifications, recording health). One copy of the toggle policy; the hotkey path and
// the public API both call it. Engine-thread object; every call is synchronous.
class InjectControl {
public:
    // Same bound the hotkey path always used for these commands.
    static constexpr uint32_t kToggleTimeoutMs = 1000;
    // A toggle that blocks its caller this long is reported: tray, hotkeys and (later) the engine loop
    // are unresponsive meanwhile.
    static constexpr uint64_t kBlockWarnThresholdMs = 250;

    explicit InjectControl(InjectLink& link) : link_(link) {}

    // Forward the intent to the inject process, which owns the overlay config (keeps the overlay-config
    // seqlock single-writer). Logs the outcome; text is the controller path's.
    ToggleOutcome ToggleOverlay();
    ToggleOutcome ToggleBenchmark();

    // Shared-memory transactions. Without an inject identity they return an Unavailable result and touch
    // nothing (ReadHealth reports an all-default observation), as the free functions they replace did.
    ce::ipc::ControlResult Notify(OverlayNotificationType type, uint64_t expiryTick);
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent intent);
    ce::ipc::ControlResult ReadHealth(ce::ipc::RecordingHealthObservation& observation);
    // Consumes the observed failure code (when non-zero) and, when the media process is gone, the
    // recording state it left behind. No-op without an inject identity.
    void ClearMediaFailure(uint32_t failure, bool mediaGone);

    // The timer rule behind the blocked-caller warning, exposed so it is testable without waiting.
    static constexpr bool ShouldWarnBlocked(uint64_t elapsedMs) {
        return elapsedMs >= kBlockWarnThresholdMs;
    }

private:
    ToggleOutcome Toggle(ProcessCommand command);

    InjectLink& link_;
};

}  // namespace ce::runtime
