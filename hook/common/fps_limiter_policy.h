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

}  // namespace ce::fps_limiter_policy
