#pragma once

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

}  // namespace ce::fps_limiter_policy
