#pragma once

// A recording the media helper ends on its own. The media process stops a live recording itself when its output
// cannot continue (a failed output write, a capture colour contract that changed after the file opened, an encoder
// that stopped emitting frames). It finalizes what it committed and exits, like after a requested stop. The
// controller, which still held the recording as requested, used to read that exit as a crash: "Recording failed"
// replaced the saved/degraded completion the media had just published, automatic recording was disabled and the
// helper was respawned (session 20261103_004120, a network share that dropped the open file).
//
// The media now publishes a notice before it finalizes. The controller ends its request on it and retires the media
// child, so the child's exit reports the recording's finalization. Reason and live stamp share one 64-bit value: a
// reader never pairs a reason with another recording's stamp, and a notice is accepted only by the request that
// started before the recording it ended went live (the rule that already rejects stale live stamps).

#include "common/ipc/shared_defs.h"

#include <atomic>
#include <cstdint>

namespace ce::media_end {

enum class Reason : uint8_t {
    None = 0,
    OutputFailed = 1,           // the recording output refused a write or its trailer
    SourceContractChanged = 2,  // the capture colour contract changed after the output opened
    VideoOutputFailed = 3,      // the encoder or its device stopped emitting frames
    OutputSpoolExhausted = 4,   // the output target stayed unreachable until the local spool was full
};

inline const char* ReasonName(Reason reason) {
    switch (reason) {
        case Reason::None:
            return "none";
        case Reason::OutputFailed:
            return "output_failed";
        case Reason::SourceContractChanged:
            return "source_contract_changed";
        case Reason::VideoOutputFailed:
            return "video_output_failed";
        case Reason::OutputSpoolExhausted:
            return "output_spool_exhausted";
    }
    return "unknown";
}

struct Notice {
    Reason reason = Reason::None;
    uint64_t liveSinceMs = 0;  // the ended recording's live stamp (CaptureState::recordingStartTime)
};

inline constexpr uint64_t kLiveStampMask = (uint64_t{1} << 56) - 1;

inline constexpr uint64_t Encode(Reason reason, uint64_t liveSinceMs) {
    return ((liveSinceMs & kLiveStampMask) << 8) | static_cast<uint8_t>(reason);
}

inline constexpr Notice Decode(uint64_t value) {
    return Notice{static_cast<Reason>(value & 0xFF), value >> 8};
}

// Whether `notice` ends the request that started at `requestStartedMs`: a recording that went live before the request
// started belongs to an earlier request.
inline constexpr bool EndsRequest(const Notice& notice, uint64_t requestStartedMs) {
    return notice.reason != Reason::None && notice.liveSinceMs != 0 && notice.liveSinceMs >= requestStartedMs;
}

// Ends the live recording from inside the media helper: publishes the notice (the first reason of a recording wins),
// shows "Finalizing recording..." in both overlays until the media's completion replaces it, and asks the media loop
// to stop. A recording that never went live has no stamp to scope a notice by; it only gets the stop request.
inline void RequestStop(CaptureState& state, Reason reason, uint64_t nowMs) {
    const int64_t liveSince = state.recordingStartTime.load(std::memory_order_acquire);
    if (liveSince > 0) {
        const uint64_t notice = Encode(reason, static_cast<uint64_t>(liveSince));
        uint64_t current = state.mediaEndedRecording.load(std::memory_order_acquire);
        while (Decode(current).liveSinceMs != Decode(notice).liveSinceMs &&
               !state.mediaEndedRecording.compare_exchange_weak(current, notice, std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
        }
        state.notificationType.store(static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing),
                                     std::memory_order_release);
        state.notificationExpiry.store(nowMs + 60000ULL, std::memory_order_release);
    }
    state.cmdStopRecording.store(true, std::memory_order_release);
}

}  // namespace ce::media_end
