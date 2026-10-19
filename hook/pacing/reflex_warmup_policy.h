#pragma once

#include <algorithm>
#include <cstdint>

// How the FPS limiter hands pacing back to a game-owned Reflex Sleep (D3D NvAPI / Streamline): the fresh-Sleep
// streak, the driver-owned warmup while it builds, and how a Sleep is matched to the Apply() evaluations of the
// game frame it paced. Included by fps_limiter_policy.h.
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

// Whether the game's latest Reflex Sleep still covers this Apply() evaluation: it advanced here, or this
// evaluation can still belong to a game frame a Sleep paced. One Sleep paces one game frame, and that frame
// reaches Apply() at most `evaluationsPerGameFrame` times (N, ResolveEvaluationsPerGameFrame).
//
// Requiring a new Sleep at every evaluation assumed N == 1. Under DLSS 4x on Talos's pre-FSR normal route
// every output is a standalone Streamline Present at the DXGI hook: four per game frame, flip-metered into a
// burst of ~2.5 ms. The 2 ms duplicate window swallowed outputs 2-3, output 4 escaped it, saw no new Sleep and
// was judged a stall: timer fallback, a ~25 ms CE wait on Streamline's presenter thread and a re-based Sleep
// streak, every frame for 1.6 s (20261019_204226).
//
// The next frame's Sleep runs on the game thread, asynchronous to the outputs: it can land after any of the
// current frame's N outputs. Between two healthy Sleeps the gap is therefore at most N - 1 outputs of the
// Sleep's own frame plus N - 1 of the next, 2(N - 1) evaluations without a new Sleep. One more proves a game
// frame went by without its Sleep, so fallback still resumes within the first frame whose Sleep is missing,
// long before the recency grace. N == 1 (an application Present, an admitted output group) keeps the original
// rule: the very next evaluation without a new Sleep falls back.
inline bool IsEvaluationCoveredByGameSleep(bool gameSleepAdvanced, uint32_t evaluationsWithoutSleep,
                                           uint32_t evaluationsPerGameFrame) {
    if (gameSleepAdvanced) {
        return true;
    }
    const uint32_t perFrame = std::clamp<uint32_t>(evaluationsPerGameFrame, 1, 16);
    return evaluationsWithoutSleep <= 2 * (perFrame - 1);
}

// The limiter's view of the game's Sleep at one Apply() evaluation (FpsLimiter::ObserveReflexSleepProgress).
struct ReflexSleepProgress {
    uint32_t gameSleepCount = 0;
    uint32_t freshSleepCount = 0;          // Sleeps since the last disruption (stall, Present gap, epoch reset)
    uint32_t evaluationsWithoutSleep = 0;  // evaluations since the one that saw the latest new Sleep
    uint32_t evaluationsPerGameFrame = 1;
    bool advanced = false;          // a Sleep completed since the previous evaluation
    bool coversEvaluation = false;  // IsEvaluationCoveredByGameSleep
    bool recentPresentGap = false;
};

inline ReflexPacingDecision ResolveReflexPacingDecision(bool explicitReflexMode, bool gameActivated,
                                                        bool gameSleepObserved, bool gameSleepRecent,
                                                        bool gameSleepCoversEvaluation, uint32_t freshSleepCount,
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
    // again before the stable streak is complete, fallback resumes within the
    // first game frame without a Sleep (IsEvaluationCoveredByGameSleep) instead
    // of waiting out the recency grace.
    decision.useGameSleepWarmup = gameActivated && gameSleepObserved && gameSleepRecent &&
                                  gameSleepCoversEvaluation && freshSleepCount < 3;
    decision.useExplicitLocalCadence =
        explicitReflexMode && !decision.useGameSleepHandoff && !decision.useGameSleepWarmup;
    return decision;
}

inline bool ShouldRunExplicitReflexCadencePostPresent(const ReflexPacingDecision& decision,
                                                      bool callSiteSupportsPostPresentCadence) {
    return decision.useExplicitLocalCadence && callSiteSupportsPostPresentCadence;
}

}  // namespace ce::fps_limiter_policy
