#include "pseudo_overlay_focus_grace.h"

#include <algorithm>

namespace ce::pseudo_overlay {

namespace {

// Treat `lastAcquireTickMs == 0` as the "no acquire recorded yet" sentinel. The
// controller-side timer only has millisecond resolution, so the grace window's edge
// cases (e.g. focus lost and regained within the same tick) still resolve cleanly.
bool IsUnsetTick(uint64_t tickMs) {
    return tickMs == 0;
}

}  // namespace

FocusGraceDecision ComputeFocusGraceDecision(uint64_t nowMs, uint64_t lastAcquireTickMs,
                                             uint32_t lastAcquirePid, uint32_t currentPid,
                                             bool hadForegroundTarget, bool currentHadTarget,
                                             bool prevGraceActive, uint32_t graceMs,
                                             bool recordingStateChanged) {
    FocusGraceDecision out;
    // Clamp the configured grace into a sane range. The config layer already clamps to
    // [0, 10000], but this helper must be safe to call with arbitrary inputs from tests.
    const uint32_t effectiveGraceMs = (graceMs > 10000u) ? 10000u : graceMs;
    out.graceMs = effectiveGraceMs;

    // Grace disabled -> never suppress for focus reasons.
    if (effectiveGraceMs == 0) {
        out.suppressVisibleOverlay = false;
        out.justStartedGrace = false;
        out.justEndedGrace = false;
        out.graceActive = false;
        return out;
    }

    // Whitelisted target lost -> not in grace. (Caller should destroy the windows and
    // reset its tracking via lastAcquireTickMs / lastAcquirePid.)
    if (!currentHadTarget) {
        out.suppressVisibleOverlay = false;
        out.justStartedGrace = false;
        out.justEndedGrace = false;
        out.graceActive = false;
        return out;
    }

    // Detect focus acquire transition:
    //   - first time we ever see a whitelisted PID (lastAcquireTickMs == 0)
    //   - PID changed since last acquire (different process, even if still whitelisted)
    //   - we didn't have a target last tick, but we do this tick
    const bool pidChanged = (lastAcquirePid != 0) && (lastAcquirePid != currentPid);
    const bool isTransition =
        IsUnsetTick(lastAcquireTickMs) || pidChanged || (!hadForegroundTarget && currentHadTarget);

    if (isTransition) {
        // The grace starts on the transition tick. We do NOT suppress the very first
        // tick's render because the caller has just detected a new focus and will
        // call UpdateOverlay() to render. The grace effectively begins *after* this
        // tick's first render, which is the same behavior as before this change for
        // tick 0 and gives a soft 0ms delay on a focus transition when the timer
        // immediately fires again.
        //
        // To preserve the user-visible "wait 2s before touching MPO" guarantee, we
        // DO suppress on the transition tick if the caller has not yet rendered at
        // least one frame for this focus (lastAcquireTickMs == 0 means truly first
        // detection). For a real PID swap mid-session we still mark the transition
        // so the caller can reset its state, but we let the very first detection
        // render at t=0 and start measuring from there.
        out.justStartedGrace = true;
        out.suppressVisibleOverlay = IsUnsetTick(lastAcquireTickMs);
        out.justEndedGrace = false;
        out.graceActive = !IsUnsetTick(lastAcquireTickMs);  // we are entering grace for
                                                            // a PID change mid-session
        return out;
    }

    // External abort signal: a recording state change during grace commits immediately.
    // This is the rare case where the user pressed the hotkey during the Alt+Tab
    // debounce window and we want the indicator to appear at once.
    if (recordingStateChanged) {
        out.suppressVisibleOverlay = false;
        out.justStartedGrace = false;
        out.justEndedGrace = prevGraceActive;  // only true if we were actually in grace
        out.graceActive = false;
        return out;
    }

    // No transition, still focused on the same whitelisted PID. Are we inside grace?
    if (IsUnsetTick(lastAcquireTickMs) || nowMs < lastAcquireTickMs) {
        // Defensive: no acquire recorded, treat as "no grace, render now".
        out.suppressVisibleOverlay = false;
        out.justStartedGrace = false;
        out.justEndedGrace = false;
        out.graceActive = false;
        return out;
    }

    const uint64_t elapsed = nowMs - lastAcquireTickMs;
    if (elapsed < effectiveGraceMs) {
        out.suppressVisibleOverlay = true;
        out.justStartedGrace = false;
        out.justEndedGrace = false;
        out.graceActive = true;
        return out;
    }

    // Grace has elapsed; render normally. Mark justEndedGrace only on the single
    // tick where the boundary is crossed (prevGraceActive && !currentActive).
    out.suppressVisibleOverlay = false;
    out.justStartedGrace = false;
    out.justEndedGrace = prevGraceActive;
    out.graceActive = false;
    return out;
}

bool ShouldSuppressPseudoOverlayForFocusGrace(uint64_t nowMs, uint64_t lastAcquireTickMs,
                                              uint32_t lastAcquirePid, uint32_t currentPid,
                                              bool hadForegroundTarget, bool currentHadTarget,
                                              bool prevGraceActive, uint32_t graceMs,
                                              bool recordingStateChanged) {
    return ComputeFocusGraceDecision(nowMs, lastAcquireTickMs, lastAcquirePid, currentPid,
                                     hadForegroundTarget, currentHadTarget, prevGraceActive,
                                     graceMs, recordingStateChanged)
        .suppressVisibleOverlay;
}

}  // namespace ce::pseudo_overlay
