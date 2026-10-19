#pragma once

#include <cstdint>

// How the FPS limiter hands pacing back to a game-owned Reflex Sleep (D3D NvAPI / Streamline): the fresh-Sleep
// streak and the driver-owned warmup while it builds. Included by fps_limiter_policy.h.
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

}  // namespace ce::fps_limiter_policy
