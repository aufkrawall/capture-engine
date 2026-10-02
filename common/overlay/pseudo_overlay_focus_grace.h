#pragma once

#include <cstdint>

namespace ce::pseudo_overlay {

// Result of a foreground-acquire grace check for the controller-side pseudo-overlay.
//
// The controller-side pseudo-overlay runs entirely inside captureengine.exe and uses
// layered topmost popup windows. On Alt+Tab back into a whitelisted game, the OS has to
// re-attach Windows MPO planes and rebind fullscreen buffer surfaces. The pseudo-overlay
// previously inserted its ShowWindow/SetWindowPos/UpdateLayeredWindow calls immediately,
// which on some drivers / games destabilized that rebind and could freeze the game window
// (most likely a Windows MPO / DXGI fullscreen bug, not our code).
//
// The grace period defers the first visible render until N ms after the whitelisted PID
// (re)acquires foreground focus. Internal state (sticky anchor, warning blink phase) is
// still advanced so the first visible frame after grace is in-position and in-phase.
struct FocusGraceDecision {
    bool suppressVisibleOverlay = false;  // True: skip EnsureOverlayWindows + ShowWindow +
                                          // SetWindowPos + UpdateLayeredWindow this tick.
    bool justStartedGrace = false;        // True: this tick is the first tick of a new grace window.
    bool justEndedGrace = false;          // True: this tick is the first tick after the grace expired.
    bool graceActive = false;             // True: grace window is currently suppressing the visible overlay.
    uint32_t graceMs = 0;                 // Configured grace length (echoed for diagnostics).
};

// Decide whether the pseudo-overlay's visible render should be deferred this tick.
//
// Inputs are plain integers / bools so the helper is fully unit-testable without Windows
// APIs or shared memory mocks.
//
//   nowMs:                current GetTickCount64() snapshot (ms)
//   lastAcquireTickMs:    last time the whitelisted PID became foreground (ms), or 0 if never
//   lastAcquirePid:       PID that became foreground at lastAcquireTickMs, or 0
//   currentPid:           PID that IsForegroundTarget() is reporting this tick (0 = none)
//   hadForegroundTarget:  whether last tick already had a whitelisted target focused
//   currentHadTarget:     whether this tick has a whitelisted target focused
//   prevGraceActive:      caller's last-tick `FocusGraceDecision::graceActive` (false on
//                         first call). The helper uses this to detect the grace-end edge
//                         exactly once instead of every tick past the boundary.
//   graceMs:              configured grace period (0 disables; values > 10000 are clamped)
//   recordingStateChanged: optional external abort signal (e.g. user toggles recording
//                          during grace, which is a clean state-change to commit to immediately)
//
// `lastAcquireTickMs == 0` is the sentinel for "no acquire recorded yet" and behaves
// like a transition on the first focus detection.
FocusGraceDecision ComputeFocusGraceDecision(uint64_t nowMs, uint64_t lastAcquireTickMs, uint32_t lastAcquirePid,
                                             uint32_t currentPid, bool hadForegroundTarget, bool currentHadTarget,
                                             bool prevGraceActive, uint32_t graceMs, bool recordingStateChanged);

// Convenience wrapper for callers that just want the suppression bool. Returns the same
// value as FocusGraceDecision::suppressVisibleOverlay.
bool ShouldSuppressPseudoOverlayForFocusGrace(uint64_t nowMs, uint64_t lastAcquireTickMs, uint32_t lastAcquirePid,
                                              uint32_t currentPid, bool hadForegroundTarget, bool currentHadTarget,
                                              bool prevGraceActive, uint32_t graceMs, bool recordingStateChanged);

}  // namespace ce::pseudo_overlay
