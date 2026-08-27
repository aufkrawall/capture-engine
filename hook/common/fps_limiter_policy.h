#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>

namespace ce::fps_limiter_policy {

struct ReflexPacingDecision {
    bool useGameSleepHandoff = false;
    bool useExplicitLocalCadence = false;
};

inline ReflexPacingDecision ResolveReflexPacingDecision(bool explicitReflexMode, bool gameSleepObserved,
                                                        bool gameSleepRecent, uint32_t freshSleepCount,
                                                        bool recentPresentGap) {
    ReflexPacingDecision decision;
    (void)gameSleepObserved;
    decision.useGameSleepHandoff = gameSleepRecent && freshSleepCount >= 3 && !recentPresentGap;
    decision.useExplicitLocalCadence = explicitReflexMode && !decision.useGameSleepHandoff;
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

inline bool ShouldScaleTargetForFrameGeneration(bool usingCaptureSync, bool injectVideoCaptureRequested) {
    // Inject capture publishes only application-rendered frames, while WGC/DXGI
    // observe the final presented stream including generated frames.
    return !usingCaptureSync || !injectVideoCaptureRequested;
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
// inject capture-sync keeps scale 1 because its source contains only
// application-rendered frames and its target already IS the base rate.
inline int ResolveCadenceScaleMultiplier(bool frameGenerationActive, int frameGenerationMultiplier,
                                         bool scaleForFrameGeneration) {
    if (!frameGenerationActive || !scaleForFrameGeneration || frameGenerationMultiplier < 2) {
        return 1;
    }
    return std::min(frameGenerationMultiplier, 4);
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
