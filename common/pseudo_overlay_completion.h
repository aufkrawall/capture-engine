#pragma once

// Maps the shared-memory completion notification onto the controller pseudo overlay's
// text selection and back, so the pseudo overlay draws the same wording and warning
// color as the in-game overlay (common/output_completion_notification.h). Kept out of
// captureengine/pseudo_overlay_internal.h so the round trip is unit-testable.

#include <cstdint>

#include "output_completion_notification.h"
#include "pseudo_overlay_visibility.h"
#include "shared_defs.h"

inline ce::pseudo_overlay::RecordingNotificationKind ToPseudoRecordingNotification(uint32_t notificationType) {
    switch (static_cast<OverlayNotificationType>(notificationType)) {
        case OverlayNotificationType::RecordingFinalizing:
            return ce::pseudo_overlay::RecordingNotificationKind::Finalizing;
        case OverlayNotificationType::RecordingSaved:
            return ce::pseudo_overlay::RecordingNotificationKind::Saved;
        case OverlayNotificationType::RecordingSavedDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::SavedDegraded;
        case OverlayNotificationType::RecordingCanceled:
            return ce::pseudo_overlay::RecordingNotificationKind::Canceled;
        case OverlayNotificationType::RecordingFailed:
            return ce::pseudo_overlay::RecordingNotificationKind::Failed;
        case OverlayNotificationType::StreamingEnded:
            return ce::pseudo_overlay::RecordingNotificationKind::StreamEnded;
        case OverlayNotificationType::StreamingEndedDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::StreamEndedDegraded;
        case OverlayNotificationType::StreamingFailed:
            return ce::pseudo_overlay::RecordingNotificationKind::StreamFailed;
        case OverlayNotificationType::RecordingSavedAudioDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::SavedAudioDegraded;
        case OverlayNotificationType::RecordingSavedAudioVideoDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::SavedAudioVideoDegraded;
        case OverlayNotificationType::StreamingEndedAudioDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::StreamEndedAudioDegraded;
        case OverlayNotificationType::StreamingEndedAudioVideoDegraded:
            return ce::pseudo_overlay::RecordingNotificationKind::StreamEndedAudioVideoDegraded;
        default:
            return ce::pseudo_overlay::RecordingNotificationKind::None;
    }
}

// The completion a pseudo-overlay text kind shows, so its wording and warning color come
// from the same table as the in-game overlay (common/output_completion_notification.h).
inline OverlayNotificationType ToOutputCompletionNotification(ce::pseudo_overlay::OverlayTextKind kind) {
    using Kind = ce::pseudo_overlay::OverlayTextKind;
    switch (kind) {
        case Kind::RecordingSaved:
            return OverlayNotificationType::RecordingSaved;
        case Kind::RecordingSavedDegraded:
            return OverlayNotificationType::RecordingSavedDegraded;
        case Kind::RecordingSavedAudioDegraded:
            return OverlayNotificationType::RecordingSavedAudioDegraded;
        case Kind::RecordingSavedAudioVideoDegraded:
            return OverlayNotificationType::RecordingSavedAudioVideoDegraded;
        case Kind::RecordingCanceled:
            return OverlayNotificationType::RecordingCanceled;
        case Kind::RecordingFailed:
            return OverlayNotificationType::RecordingFailed;
        case Kind::StreamingEnded:
            return OverlayNotificationType::StreamingEnded;
        case Kind::StreamingEndedDegraded:
            return OverlayNotificationType::StreamingEndedDegraded;
        case Kind::StreamingEndedAudioDegraded:
            return OverlayNotificationType::StreamingEndedAudioDegraded;
        case Kind::StreamingEndedAudioVideoDegraded:
            return OverlayNotificationType::StreamingEndedAudioVideoDegraded;
        case Kind::StreamingFailed:
            return OverlayNotificationType::StreamingFailed;
        default:
            return OverlayNotificationType::None;
    }
}
