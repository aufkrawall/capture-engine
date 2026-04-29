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
    decision.useGameSleepHandoff = gameSleepRecent && freshSleepCount >= 3 && !recentPresentGap;
    decision.useExplicitLocalCadence = explicitReflexMode && !gameSleepObserved;
    return decision;
}

inline bool ShouldRunExplicitReflexCadencePostPresent(const ReflexPacingDecision& decision,
                                                      bool callSiteSupportsPostPresentCadence) {
    return decision.useExplicitLocalCadence && callSiteSupportsPostPresentCadence;
}

}  // namespace ce::fps_limiter_policy
