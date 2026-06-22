#pragma once

#include <cstdint>

namespace ce::pseudo_overlay {

// Pure, Windows-free decision for whether the controller-side pseudo-overlay should have
// ANY visible window this update. Extracted from the live overlay so the visibility policy
// can be unit-tested without GDI / shared memory, mirroring the focus-grace helper pattern.
//
// Invariant enforced here (and the reason this helper exists): the "NOT RECORDING" warning
// is mutually exclusive with an active recording. It is gated on `!isRecording` so a stale
// `warnVisible` flag can never keep a layered topmost window alive once recording has
// started. Previously the flag was only cleared by the next 500 ms timer tick, so starting
// a recording in mode 2 while the warning was in its visible phase left the warning window
// up (and re-rendered "NOT RECORDING") for up to ~500 ms into the recording. The only
// sanctioned overlay activity during a mode-2 recording is the encoder-overload warning and
// the screenshot-saved notification, both represented below.
struct OverlayVisibilityInputs {
    int mode = 0;                       // 0=indicator, 1=warn+indicator, 2=warn-only
    bool isRecording = false;
    bool warnVisible = false;           // NOT-RECORDING blink phase currently "on"
    bool showEncoderOverloadWarn = true;
    bool ghostActive = false;           // alwaysRender keepalive (1px alpha ghost)
    uint64_t nowMs = 0;
    uint64_t overloadWarnUntilMs = 0;   // 0 = no overload warning pending
    uint64_t screenshotNotifyUntilMs = 0;  // 0 = no screenshot notification pending
};

inline bool ShouldPseudoOverlayBeVisible(const OverlayVisibilityInputs& in) {
    const bool showIndicator = in.isRecording && in.mode != 2;
    const bool showNotRecordingWarn = in.warnVisible && !in.isRecording;
    const bool showOverloadWarn =
        in.showEncoderOverloadWarn && in.overloadWarnUntilMs != 0 && in.nowMs < in.overloadWarnUntilMs;
    const bool showScreenshot = in.screenshotNotifyUntilMs != 0 && in.nowMs < in.screenshotNotifyUntilMs;
    return showIndicator || showNotRecordingWarn || showOverloadWarn || showScreenshot || in.ghostActive;
}

}  // namespace ce::pseudo_overlay
