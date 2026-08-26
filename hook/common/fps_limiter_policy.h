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

struct PhasePreservingLateAdvance {
    int64_t nextTargetQpc = 0;
    uint32_t skippedGridSlots = 0;
};

inline PhasePreservingLateAdvance AdvanceCaptureSyncDeadlineAfterLateFrame(int64_t currentTargetQpc,
                                                                           int64_t nowQpc, int64_t frequency,
                                                                           int fps, int64_t& remainder) {
    PhasePreservingLateAdvance result{currentTargetQpc, 0};
    if (currentTargetQpc <= 0 || nowQpc <= 0 || frequency <= 0 || fps <= 0) {
        result.nextTargetQpc = nowQpc + NextRationalIntervalTicks(frequency, fps, remainder);
        result.skippedGridSlots = 1;
        return result;
    }

    // Keep the original rational-grid phase after a hitch. A half-interval guard prevents an
    // immediate short catch-up Present, while advancing by whole grid slots avoids the permanent
    // phase rebase that otherwise makes a matched capture/output cadence straddle the CFR
    // selector's half-frame boundary for the rest of the recording.
    const int64_t halfInterval = std::max<int64_t>(1, (frequency / fps) / 2);
    const int64_t minimumNextQpc = nowQpc <= INT64_MAX - halfInterval ? nowQpc + halfInterval : INT64_MAX;
    do {
        const int64_t step = NextRationalIntervalTicks(frequency, fps, remainder);
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
