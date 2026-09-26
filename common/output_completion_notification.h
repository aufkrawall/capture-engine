#pragma once

// User-facing text for the end of a recording or stream. The in-game overlay (hook) and
// the controller's pseudo overlay both render these, so the wording lives in one place.
// A degraded completion names the track that lost content: an audio-only loss used to be
// reported as "video degraded" because only one degraded bit existed.

#include <cstdint>

#include "shared_defs.h"

namespace ce::output_completion {

struct CompletionText {
    const char* text = nullptr;  // nullptr: not a recording/stream completion notification
    bool warning = false;        // degraded or failed: drawn in the warning color
};

inline CompletionText DescribeOutputCompletion(OverlayNotificationType type) {
    switch (type) {
        case OverlayNotificationType::RecordingSaved:
            return {"Recording saved", false};
        case OverlayNotificationType::RecordingSavedDegraded:
            return {"Recording saved - video degraded", true};
        case OverlayNotificationType::RecordingSavedAudioDegraded:
            return {"Recording saved - audio degraded", true};
        case OverlayNotificationType::RecordingSavedAudioVideoDegraded:
            return {"Recording saved - audio and video degraded", true};
        case OverlayNotificationType::RecordingCanceled:
            return {"Recording canceled", false};
        case OverlayNotificationType::RecordingFailed:
            return {"Recording failed", true};
        case OverlayNotificationType::StreamingEnded:
            return {"Stream ended", false};
        case OverlayNotificationType::StreamingEndedDegraded:
            return {"Stream ended - video degraded", true};
        case OverlayNotificationType::StreamingEndedAudioDegraded:
            return {"Stream ended - audio degraded", true};
        case OverlayNotificationType::StreamingEndedAudioVideoDegraded:
            return {"Stream ended - audio and video degraded", true};
        case OverlayNotificationType::StreamingFailed:
            return {"Stream failed", true};
        default:
            return {};
    }
}

// Finalization feedback (in progress or completed) is idle-only: it must not cover a
// newer recording. A numeric range check here missed the audio-degraded values (11-14).
inline bool IsRecordingFinalizationNotification(uint32_t type) {
    const auto notification = static_cast<OverlayNotificationType>(type);
    return notification == OverlayNotificationType::RecordingFinalizing ||
           DescribeOutputCompletion(notification).text != nullptr;
}

// Every completion type, for overlay panels that size themselves to the widest text
// they may ever show.
inline constexpr OverlayNotificationType kOutputCompletionNotificationTypes[] = {
    OverlayNotificationType::RecordingSaved,
    OverlayNotificationType::RecordingSavedDegraded,
    OverlayNotificationType::RecordingSavedAudioDegraded,
    OverlayNotificationType::RecordingSavedAudioVideoDegraded,
    OverlayNotificationType::RecordingCanceled,
    OverlayNotificationType::RecordingFailed,
    OverlayNotificationType::StreamingEnded,
    OverlayNotificationType::StreamingEndedDegraded,
    OverlayNotificationType::StreamingEndedAudioDegraded,
    OverlayNotificationType::StreamingEndedAudioVideoDegraded,
    OverlayNotificationType::StreamingFailed,
};

}  // namespace ce::output_completion
