#pragma once

#include <cstdint>
#include "recording_indicator_policy.h"

namespace ce::pseudo_overlay {

enum class RecordingNotificationKind : uint8_t {
    None = 0,
    Finalizing,
    Saved,
    SavedDegraded,
    Canceled,
    Failed,
};

// Pure, Windows-free decision for whether the controller-side pseudo-overlay should have
// ANY visible window this update. Extracted from the live overlay so the visibility policy
// can be unit-tested without GDI / shared memory, mirroring the focus-grace helper pattern.
//
// Invariant enforced here (and the reason this helper exists): the "NOT RECORDING" warning
// is mutually exclusive with pending and active recording states. A stale `warnVisible`
// flag can therefore never keep a layered topmost window alive once recording startup has
// begun. During an established mode-2 recording, the sanctioned overlay activity is
// recording-health warning and screenshot feedback; finalization feedback is idle-only,
// while pending startup text is shown before that established-recording contract begins.
struct OverlayVisibilityInputs {
    int mode = 0;  // 0=indicator, 1=warn+indicator, 2=warn-only
    ce::recording_indicator::State recordingState = ce::recording_indicator::State::Idle;
    bool warnVisible = false;  // NOT-RECORDING blink phase currently "on"
    bool showEncoderOverloadWarn = true;
    bool ghostActive = false;  // alwaysRender keepalive (1px alpha ghost)
    uint64_t nowMs = 0;
    uint64_t overloadWarnUntilMs = 0;      // 0 = no overload warning pending
    uint64_t screenshotNotifyUntilMs = 0;  // 0 = no screenshot notification pending
    uint64_t recordingNotifyUntilMs = 0;  // 0 = no recording finalization notification pending
    RecordingNotificationKind recordingNotification = RecordingNotificationKind::None;
};

enum class OverlayTextKind : uint8_t {
    None = 0,
    Starting,
    NotRecording,
    EncoderOverload,
    Screenshot,
    RecordingFinalizing,
    RecordingSaved,
    RecordingSavedDegraded,
    RecordingCanceled,
    RecordingFailed,
};

inline OverlayTextKind SelectPseudoOverlayText(const OverlayVisibilityInputs& in) {
    if (ce::recording_indicator::IsStarting(in.recordingState)) {
        return in.mode != 0 ? OverlayTextKind::Starting : OverlayTextKind::None;
    }
    if (in.screenshotNotifyUntilMs != 0 && in.nowMs < in.screenshotNotifyUntilMs) {
        return OverlayTextKind::Screenshot;
    }
    if (in.recordingState == ce::recording_indicator::State::Idle &&
        in.recordingNotifyUntilMs != 0 && in.nowMs < in.recordingNotifyUntilMs) {
        switch (in.recordingNotification) {
            case RecordingNotificationKind::Finalizing:
                return OverlayTextKind::RecordingFinalizing;
            case RecordingNotificationKind::Saved:
                return OverlayTextKind::RecordingSaved;
            case RecordingNotificationKind::SavedDegraded:
                return OverlayTextKind::RecordingSavedDegraded;
            case RecordingNotificationKind::Canceled:
                return OverlayTextKind::RecordingCanceled;
            case RecordingNotificationKind::Failed:
                return OverlayTextKind::RecordingFailed;
            default:
                break;
        }
    }
    if (in.showEncoderOverloadWarn && in.overloadWarnUntilMs != 0 && in.nowMs < in.overloadWarnUntilMs) {
        return OverlayTextKind::EncoderOverload;
    }
    if (in.warnVisible && in.recordingState == ce::recording_indicator::State::Idle) {
        return OverlayTextKind::NotRecording;
    }
    return OverlayTextKind::None;
}

inline bool ShouldPseudoOverlayBeVisible(const OverlayVisibilityInputs& in) {
    const bool showIndicator = ce::recording_indicator::IsVisible(in.recordingState) && in.mode != 2;
    return showIndicator || SelectPseudoOverlayText(in) != OverlayTextKind::None || in.ghostActive;
}

}  // namespace ce::pseudo_overlay
