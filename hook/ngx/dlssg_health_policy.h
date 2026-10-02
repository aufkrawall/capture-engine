#pragma once

// DLSS-G health and state-reporting policy.
//
// Split out of streamline_runtime_policy.h (2026-09-19) when that file reached
// the 800-line ceiling. Both halves answer the same question - "what is the
// runtime actually telling us, and can we trust it?" - and both were written
// from the same two hardware sessions, so they belong together.
//
// Stays inside `ce::streamline_runtime_policy` so every existing call site is
// unchanged; streamline_runtime_policy.h includes this.

#include <cstddef>
#include <cstdint>

namespace ce::streamline_runtime_policy {

// `slDLSSGState` grows by struct version and the APPLICATION owns the
// allocation: CE only ever sees a pointer to whatever the game declared. A
// field introduced above the game's `structVersion` is therefore memory the
// game never reserved for it, and reading it yields whatever happened to be
// next on the game's stack - not a runtime answer.
//
// This is not hypothetical. `bIsDynamicMFGSupported` arrived in version 4;
// Talos Principle 2 publishes version 3 (its fence fields read back correctly,
// the byte after them does not). Session `20260919_223111` read that byte as 0
// and reported "dynamic MFG NOT supported" while the runtime was demonstrably
// varying the cadence between 2x and 4x to hold ~139.6 fps. Unknown has to be
// spelled unknown.
inline constexpr size_t kDLSSGStateVsyncSupportMinVersion = 2;
inline constexpr size_t kDLSSGStateFenceMinVersion = 3;
inline constexpr size_t kDLSSGStateDynamicMFGMinVersion = 4;

inline constexpr bool DLSSGStateCarriesDynamicMFGSupport(size_t structVersion) {
    return structVersion >= kDLSSGStateDynamicMFGMinVersion;
}

inline constexpr bool DLSSGStateCarriesVsyncSupport(size_t structVersion) {
    return structVersion >= kDLSSGStateVsyncSupportMinVersion;
}

// Tri-state for a `char` boolean the runtime may not have written: -1 unknown
// (the game's struct is too old to carry it), otherwise 0/1.
inline constexpr int ResolveDLSSGStateOptionalBool(size_t structVersion, size_t minVersion, char value,
                                                   char invalidSentinel) {
    return structVersion < minVersion || value == invalidSentinel ? -1 : (value != 0 ? 1 : 0);
}

// --- DLSSG activation-health monitor (session 20260702_094955: GTA cold-start DLSS FG reported ON with
// updateActive=1, but presents stayed at base rate all session and the user saw no fps gain) -------------
// Track health only for successful GetState queries where the game actually REQUESTS frame generation
// (options mode != off). OFF-mode samples must not extend a not-interpolating streak.
//
// `sl::DLSSGMode::eAuto` (2) and `eDynamic` (3) hand the cadence to the runtime, and **choosing not to
// generate a frame is a legitimate operating point there**, not a failed activation: `dlss_fg_dynamic_max`
// is an "up to", and when the rendered rate already meets the target the correct choice is 1x. The
// monitor's whole premise - the game asked for frame generation, so frames must be appearing - only holds
// for a fixed request.
inline bool IsDLSSGCadenceChosenByRuntime(uint32_t mode) {
    return mode == 2 || mode == 3;
}

// The application's options are NOT the whole answer, and assuming they were is why the first attempt at
// this gate did nothing. `dlss_fg_mode=dynamic` is delivered over the driver settings and applied inside
// sl.dlss_g, so the options CE sees keep reporting the game's own fixed request: session
// `20260919_231939` still warned four times with `optionsMode=on(1)` while the cadence varied 2x..4x.
// CE's own configured mode therefore has to suppress the monitor too.
//
// Deliberately keyed on the *configured* intent rather than on evidence that the runtime accepted it:
// whether it did is exactly what CE cannot observe here (the game's DLSSGState predates
// `bIsDynamicMFGSupported`), and having asked for a runtime-chosen cadence is already enough to make
// "nothing was generated" unusable as a fault signal.
inline bool ShouldTrackDLSSGActivationHealthSample(bool getStateSucceeded, bool optionsRequestFrameGenerationOn,
                                                   uint32_t optionsMode, bool configuredCadenceChosenByRuntime) {
    return getStateSucceeded && optionsRequestFrameGenerationOn && !IsDLSSGCadenceChosenByRuntime(optionsMode) &&
           !configuredCadenceChosenByRuntime;
}

// DLSSGState.numFramesActuallyPresented >= 2 proves generated frames reached presentation. ==1 means only
// the real frame was presented (no interpolation) — though on hardware flip-metering MFG paths the API-side
// value can stay 1 while the display shows generated frames, so this is EVIDENCE for a log-side monitor,
// never an enforcement signal.
inline bool IsDLSSGInterpolationPresentEvidence(uint32_t numFramesActuallyPresented) {
    return numFramesActuallyPresented >= 2;
}

// Deterministic streak warning: first warn after warnAtStreak consecutive non-interpolating ON samples
// (GTA polls GetState ~per frame, so this lands within a handful of frames of a failed activation), then
// repeat every repeatEvery samples so a long session stays readable.
inline bool ShouldWarnDLSSGActiveButNotInterpolating(uint64_t consecutiveNonInterpolatingSamples, uint64_t warnAtStreak,
                                                     uint64_t repeatEvery) {
    if (warnAtStreak == 0 || consecutiveNonInterpolatingSamples < warnAtStreak) {
        return false;
    }
    if (consecutiveNonInterpolatingSamples == warnAtStreak) {
        return true;
    }
    return repeatEvery != 0 && ((consecutiveNonInterpolatingSamples - warnAtStreak) % repeatEvery) == 0;
}

}  // namespace ce::streamline_runtime_policy
