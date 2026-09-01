#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>

#include "fg_runtime_state.h"

namespace ce::fps_limiter_policy {

struct ReflexPacingDecision {
    bool useGameSleepHandoff = false;
    bool useGameSleepWarmup = false;
    bool useExplicitLocalCadence = false;
};

inline uint32_t RebaseGameSleepBaselineForCounterEpoch(uint32_t baselineCount,
                                                       uint32_t currentSleepCount,
                                                       uint32_t previouslyEvaluatedSleepCount) {
    // Reflex activation epochs reset their observation counter. Preserve a
    // disruption baseline only within the same epoch; otherwise a high old
    // baseline can prevent a fresh 1/2/3-call streak from ever handing off.
    return currentSleepCount < previouslyEvaluatedSleepCount ? 0 : baselineCount;
}

inline ReflexPacingDecision ResolveReflexPacingDecision(bool explicitReflexMode, bool gameActivated,
                                                        bool gameSleepObserved, bool gameSleepRecent,
                                                        bool gameSleepAdvanced, uint32_t freshSleepCount,
                                                        bool recentPresentGap) {
    ReflexPacingDecision decision;
    (void)recentPresentGap;
    // The caller re-bases freshSleepCount at the edge of a Present gap. Once
    // three successful Sleep calls have occurred after that edge, extending
    // the local fallback for the whole gap-grace window only overlays a second
    // cadence on the newly healthy native one. That showed up as temporal
    // jitter around cutscene/FG transitions. Fresh post-gap Sleep evidence is
    // the recovery proof; a merely old observed Sleep is still insufficient.
    decision.useGameSleepHandoff =
        gameActivated && gameSleepObserved && gameSleepRecent && freshSleepCount >= 3;
    // Do not overlay CE's fallback cadence on the exact recovery frames where
    // a newly successful game Sleep already owns pacing. This is deliberately
    // progress-qualified rather than merely recency-qualified: if Sleep stops
    // again before the stable streak is complete, fallback resumes on the
    // very next evaluated frame instead of waiting out the recency grace.
    decision.useGameSleepWarmup = gameActivated && gameSleepObserved && gameSleepRecent &&
                                  gameSleepAdvanced && freshSleepCount < 3;
    decision.useExplicitLocalCadence =
        explicitReflexMode && !decision.useGameSleepHandoff && !decision.useGameSleepWarmup;
    return decision;
}

inline bool ShouldRunExplicitReflexCadencePostPresent(const ReflexPacingDecision& decision,
                                                      bool callSiteSupportsPostPresentCadence) {
    return decision.useExplicitLocalCadence && callSiteSupportsPostPresentCadence;
}

inline bool ShouldReturnNvApiReflexWrapper(bool manualReflexLimiterConfiguredOrActive, bool callerIsStreamlineRuntime,
                                           bool callerIsThirdPartyOverlay, bool callerIsSystemModule,
                                           bool callerIsCaptureHookModule) {
    return manualReflexLimiterConfiguredOrActive && !callerIsStreamlineRuntime && !callerIsThirdPartyOverlay &&
           !callerIsSystemModule && !callerIsCaptureHookModule;
}

inline bool IsManualReflexLimiterConfigured(bool generalEnabled, int generalFps, uint32_t generalMode,
                                            bool captureSyncEnabled, uint32_t captureSyncMode,
                                            uint32_t nativeModeValue) {
    return (generalEnabled && generalFps > 0 && generalMode == nativeModeValue) ||
           (captureSyncEnabled && captureSyncMode == nativeModeValue);
}

inline bool ShouldScaleTargetForFrameGeneration(bool usingCaptureSync, bool injectVideoCaptureRequested,
                                                bool injectFinalOutputAvailable) {
    // Ordinary inject capture publishes application-rendered frames, while
    // WGC/DXGI and the explicit DX12/Vulkan final-output routes observe the
    // presented stream including generated frames.
    return !usingCaptureSync || !injectVideoCaptureRequested || injectFinalOutputAvailable;
}

// Streamline's requested/available state can report DLSS-G ON before the game
// starts executing the frame-generation pipeline. Dividing the limiter target
// from that nominal state caps an ordinary one-Present render loop at
// target/multiplier (Gothic Remake sat at 60 fps under a 120 fps cap while its
// pre-menu DLSS-G state still produced no generated frames). A successful,
// recent game-owned low-latency sleep is a protocol-level production signal:
// DLSS-G requires Reflex, while Vulkan publishes the equivalent native pacing
// ownership through its backend. FSR FG and Smooth Motion runtime detection is
// already based on an active presentation path and does not need this extra
// Streamline qualification.
inline bool IsFrameGenerationProducingForPacing(ce::fg_runtime::RuntimeMode runtimeMode,
                                                bool runtimeFrameGenerationActive,
                                                bool recentActivatedD3DGameSleep,
                                                bool externalNativeGameActive) {
    if (!runtimeFrameGenerationActive) {
        return false;
    }
    if (runtimeMode != ce::fg_runtime::RuntimeMode::kDLSSFG) {
        return true;
    }
    return recentActivatedD3DGameSleep || externalNativeGameActive;
}

enum class LimiterConstraintSource : uint8_t {
    kNone,
    kCaptureSync,
    kGeneral,
};

struct LimiterTargetSelection {
    LimiterConstraintSource source = LimiterConstraintSource::kNone;
    int targetFps = 0;
    int captureTargetFps = 0;
    int captureOutputEquivalentFps = 0;
    int generalTargetFps = 0;
    bool captureSourceIsFinalOutput = false;

    bool IsActive() const {
        return source != LimiterConstraintSource::kNone;
    }

    bool UsesCaptureSync() const {
        return source == LimiterConstraintSource::kCaptureSync;
    }
};

inline int SaturatingPositiveProduct(int value, int multiplier) {
    if (value <= 0 || multiplier <= 0) {
        return 0;
    }
    if (value > INT_MAX / multiplier) {
        return INT_MAX;
    }
    return value * multiplier;
}

// Capture sync and the general limiter are simultaneous constraints, not a
// priority list. Compare them in the final-output domain so starting a base-
// frame inject capture cannot silently replace a stricter displayed-rate cap.
// Equal constraints prefer capture sync to retain its stable CFR grid phase.
inline LimiterTargetSelection ResolveLimiterTargetSelection(
    bool captureRequested, bool captureSyncEnabled, int captureFps, int captureSyncMultiplier,
    bool useVfr, bool generalEnabled, int generalFps, bool frameGenerationActive,
    int frameGenerationMultiplier, bool injectVideoCaptureRequested,
    bool injectFinalOutputAvailable) {
    LimiterTargetSelection selection;
    const bool captureAvailable = captureRequested && captureSyncEnabled && !useVfr &&
                                  captureFps > 0 && captureSyncMultiplier >= 1 &&
                                  captureSyncMultiplier <= 8;
    const bool generalAvailable = generalEnabled && generalFps > 0;

    selection.generalTargetFps = generalAvailable ? generalFps : 0;
    if (captureAvailable) {
        selection.captureTargetFps = SaturatingPositiveProduct(captureFps, captureSyncMultiplier);
        selection.captureSourceIsFinalOutput =
            !injectVideoCaptureRequested || injectFinalOutputAvailable;
        selection.captureOutputEquivalentFps = selection.captureTargetFps;
        if (frameGenerationActive && frameGenerationMultiplier >= 2 &&
            !selection.captureSourceIsFinalOutput) {
            selection.captureOutputEquivalentFps = SaturatingPositiveProduct(
                selection.captureTargetFps, std::min(frameGenerationMultiplier, 4));
        }
    }

    if (captureAvailable &&
        (!generalAvailable || selection.captureOutputEquivalentFps <= generalFps)) {
        selection.source = LimiterConstraintSource::kCaptureSync;
        selection.targetFps = selection.captureTargetFps;
    } else if (generalAvailable) {
        selection.source = LimiterConstraintSource::kGeneral;
        selection.targetFps = generalFps;
    }
    return selection;
}

inline int ResolveFrameGenerationBaseTarget(int outputTargetFps, bool frameGenerationActive, int multiplier,
                                            bool scaleForFrameGeneration) {
    if (outputTargetFps <= 0)
        return outputTargetFps;
    if (!frameGenerationActive || multiplier < 2 || !scaleForFrameGeneration)
        return outputTargetFps;
    return std::max(1, outputTargetFps / std::min(multiplier, 4));
}

// Size of one output group at a real final presentation/acquire boundary: the
// FG multiplier while frame generation is active, otherwise every callback is
// its own group owner. Deliberately ordinal-based, never time-based.
inline int ResolveOutputGroupAdmissionMultiplier(bool frameGenerationActive, int frameGenerationMultiplier) {
    if (!frameGenerationActive || frameGenerationMultiplier < 2) {
        return 1;
    }
    return std::min(frameGenerationMultiplier, 4);
}

// Scale of the local rational cadence interval: the group period is
// frequency * cadenceScale / configuredTarget. Final-output observers scale by
// the FG multiplier so target/multiplier groups per second yield exactly the
// configured output rate (130/3 = 43.333... groups/s under a 130 cap), while
// ordinary/base inject capture-sync keeps scale 1 because that source contains
// only application-rendered frames and its target already IS the base rate.
inline int ResolveCadenceScaleMultiplier(bool frameGenerationActive, int frameGenerationMultiplier,
                                         bool scaleForFrameGeneration) {
    if (!frameGenerationActive || !scaleForFrameGeneration || frameGenerationMultiplier < 2) {
        return 1;
    }
    return std::min(frameGenerationMultiplier, 4);
}

// NVIDIA's driver-owned low-latency interval - `minimumIntervalUs` in
// NvAPI_D3D_SetSleepMode / NvAPI_Vulkan_SetSleepMode / vkSetLatencySleepModeNV -
// is frame-generation aware for NVIDIA's OWN generated frames: the driver
// stretches the application's render loop by the active DLSS-G/MFG factor so
// the interval constrains the FINAL presented rate. Third-party generated
// frames (FSR FG) are invisible to it; there the interval throttles the game's
// Reflex sleep, i.e. the render loop itself, and the base target is correct.
inline bool DriverLowLatencyIntervalCoversGeneratedFrames(ce::fg_runtime::RuntimeMode runtimeMode) {
    return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG ||
           runtimeMode == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}

// Target handed to a driver-owned low-latency interval. CE's own cadence paces
// base frames, but a frame-generation-aware driver interval must receive the
// OUTPUT rate: giving it the FG-divided base target applies the divisor twice.
// Portal RTX with 3x MFG under a 130 cap received 43, the driver then paced the
// render loop at 43/3 = 14.3 fps, and the game displayed 43 fps instead of 130.
inline int ResolveNativeDriverPacingTargetFps(int configuredTargetFps, int baseTargetFps,
                                              bool frameGenerationActive, int multiplier,
                                              bool scaleForFrameGeneration,
                                              bool driverIntervalCoversGeneratedFrames) {
    if (configuredTargetFps <= 0 || baseTargetFps <= 0) {
        return baseTargetFps;
    }
    if (!frameGenerationActive || multiplier < 2 || !driverIntervalCoversGeneratedFrames) {
        return baseTargetFps;
    }
    if (scaleForFrameGeneration) {
        // The configured cap already denotes the final output rate.
        return configuredTargetFps;
    }
    // Ordinary/base inject capture sync configures the application-rendered
    // rate; the equivalent output rate that the driver interval expects is
    // base * multiplier. Explicit final-output inject routes take the
    // scaleForFrameGeneration branch above instead.
    const int clampedMultiplier = std::min(multiplier, 4);
    if (configuredTargetFps > INT_MAX / clampedMultiplier) {
        return configuredTargetFps;
    }
    return configuredTargetFps * clampedMultiplier;
}

// Deterministic multiplier-sized output-group admission for real final
// presentation boundaries (native-Vulkan vkQueuePresentKHR /
// vkAcquireNextImageKHR). Exactly one callback per group of `multiplier`
// consecutive real-boundary callbacks owns a cadence slot; the remaining
// multiplier-1 callbacks are the generated outputs of that already admitted
// group. Classification is a pure ordinal and never reads a clock, so a next
// real group arriving inside a time window cannot be confused with generated
// spillover - the escape that let Portal RTX run ~146 fps against a 130 cap.
class OutputGroupAdmission {
public:
    enum class Decision {
        kPaceGroup,          // This callback owns the next base-group cadence slot.
        kPassGeneratedSlot,  // One of the remaining multiplier-1 generated output slots.
    };

    Decision Classify(int multiplier) {
        if (multiplier < 2) {
            // Every real-boundary callback is its own paced group owner.
            ordinal_ = 0;
            return Decision::kPaceGroup;
        }
        const Decision decision = (ordinal_ == 0) ? Decision::kPaceGroup : Decision::kPassGeneratedSlot;
        if (++ordinal_ >= static_cast<uint32_t>(multiplier)) {
            ordinal_ = 0;
        }
        return decision;
    }

    // Discards any partial group. Returns true when generated slots had
    // already been handed out, so the caller can surface a group reset in the
    // limiter diagnostics instead of silently re-basing the admission.
    bool Reset() {
        const bool hadPartialGroup = ordinal_ != 0;
        ordinal_ = 0;
        return hadPartialGroup;
    }

    uint32_t PendingGeneratedSlots() const {
        return ordinal_;
    }

private:
    uint32_t ordinal_ = 0;
};

inline int64_t NextRationalIntervalTicks(int64_t frequency, int fps, int64_t& remainder) {
    if (frequency <= 0 || fps <= 0) {
        remainder = 0;
        return 1;
    }
    int64_t ticks = frequency / fps;
    remainder += frequency % fps;
    if (remainder >= fps) {
        remainder -= fps;
        ++ticks;
    }
    return ticks > 0 ? ticks : 1;
}

// Exact rational group-cadence variant of NextRationalIntervalTicks: the
// interval is frequency * cadenceScale / fps, so e.g. a 130 fps output cap
// with a 3x multiplier paces 130/3 = 43.333... groups/s with zero long-term
// drift instead of flooring to 43 groups/s (129 output fps). The Bresenham
// remainder distributes the sub-tick fraction across intervals.
inline int64_t NextRationalGroupIntervalTicks(int64_t frequency, int fps, int cadenceScale, int64_t& remainder) {
    if (frequency <= 0 || fps <= 0 || cadenceScale <= 0) {
        remainder = 0;
        return 1;
    }
    const int64_t scale = static_cast<int64_t>(cadenceScale);
    if (frequency > INT64_MAX / scale) {
        // Guard the multiplication even though real QPC frequencies are small;
        // degrade to the unscaled interval rather than overflow.
        remainder = 0;
        return std::max<int64_t>(1, frequency / fps);
    }
    const int64_t scaledFrequency = frequency * scale;
    int64_t ticks = scaledFrequency / fps;
    remainder += scaledFrequency % fps;
    if (remainder >= fps) {
        remainder -= fps;
        ++ticks;
    }
    return ticks > 0 ? ticks : 1;
}

struct PhasePreservingLateAdvance {
    int64_t nextTargetQpc = 0;
    uint32_t skippedGridSlots = 0;
};

inline PhasePreservingLateAdvance AdvanceCaptureSyncDeadlineAfterLateFrame(int64_t currentTargetQpc,
                                                                           int64_t nowQpc, int64_t frequency,
                                                                           int fps, int cadenceScale,
                                                                           int64_t& remainder) {
    PhasePreservingLateAdvance result{currentTargetQpc, 0};
    if (currentTargetQpc <= 0 || nowQpc <= 0 || frequency <= 0 || fps <= 0) {
        result.nextTargetQpc = nowQpc + NextRationalGroupIntervalTicks(frequency, fps, cadenceScale, remainder);
        result.skippedGridSlots = 1;
        return result;
    }

    // Keep the original rational-grid phase after a hitch. A half-interval guard prevents an
    // immediate short catch-up Present, while advancing by whole grid slots avoids the permanent
    // phase rebase that otherwise makes a matched capture/output cadence straddle the CFR
    // selector's half-frame boundary for the rest of the recording. With frame generation the
    // grid is the scaled output-group grid, so whole GROUP slots are skipped.
    const int64_t halfInterval = std::max<int64_t>(1, (frequency / fps) * std::max<int64_t>(1, cadenceScale) / 2);
    const int64_t minimumNextQpc = nowQpc <= INT64_MAX - halfInterval ? nowQpc + halfInterval : INT64_MAX;
    do {
        const int64_t step = NextRationalGroupIntervalTicks(frequency, fps, cadenceScale, remainder);
        if (result.nextTargetQpc > INT64_MAX - step) {
            result.nextTargetQpc = minimumNextQpc;
            ++result.skippedGridSlots;
            break;
        }
        result.nextTargetQpc += step;
        ++result.skippedGridSlots;
    } while (result.nextTargetQpc < minimumNextQpc);
    return result;
}

}  // namespace ce::fps_limiter_policy
