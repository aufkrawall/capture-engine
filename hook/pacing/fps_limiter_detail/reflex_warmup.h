#pragma once

// Out-of-line FpsLimiter members for handing pacing back to a game-owned Reflex
// Sleep: the Sleep progress seen at each Apply() evaluation, the driver-owned
// warmup while the fresh streak builds, the stall that ends it and the handoff.
// Included by fps_limiter.h after the class body; the policy lives in
// hook/pacing/reflex_warmup_policy.h.

#include "common/logging/log_meter.h"
#include "hook/pacing/fps_limiter.h"

namespace fps_limiter_detail {

// Stall and resume lines of one warmup episode (since the last driver handoff):
// the first few, then every 64th. A Sleep judged on and off every frame logged
// three lines a frame (20261019_204226: 26 stall/resume pairs in 1.6 s); the
// handoff line carries the totals.
inline bool ShouldLogReflexWarmupTransition(uint32_t stallsSinceHandoff) {
    return stallsSinceHandoff == 0 || ce::log_meter::ShouldLogCadence(stallsSinceHandoff, 3, 64);
}

}  // namespace fps_limiter_detail

inline void FpsLimiter::ResetReflexSleepProgress() {
    reflexSleepBaselineCount_ = 0;
    reflexLastEvaluatedGameSleepCount_ = 0;
    reflexEvaluationsWithoutSleep_ = 0;
    reflexRecentPresentGap_ = false;
    reflexWarmupActive_ = false;
    reflexWarmupSameFrameHolds_ = 0;
    reflexWarmupStalls_ = 0;
}

inline ce::fps_limiter_policy::ReflexSleepProgress FpsLimiter::ObserveReflexSleepProgress(
    bool gameSleepObserved, uint32_t evaluationsPerGameFrame) {
    ce::fps_limiter_policy::ReflexSleepProgress progress;
    progress.gameSleepCount = g_ReflexLimiter.GetGameSleepCount();
    progress.evaluationsPerGameFrame = evaluationsPerGameFrame;
    const bool gameSleepCounterReset = progress.gameSleepCount < reflexLastEvaluatedGameSleepCount_;
    reflexSleepBaselineCount_ = ce::fps_limiter_policy::RebaseGameSleepBaselineForCounterEpoch(
        reflexSleepBaselineCount_, progress.gameSleepCount, reflexLastEvaluatedGameSleepCount_);
    if (gameSleepCounterReset) {
        TraceLog("Apply: REFLEX game Sleep epoch reset previous=%u current=%u", reflexLastEvaluatedGameSleepCount_,
                 progress.gameSleepCount);
    }
    progress.advanced = gameSleepObserved && progress.gameSleepCount != reflexLastEvaluatedGameSleepCount_;
    reflexLastEvaluatedGameSleepCount_ = progress.gameSleepCount;
    // The warmup's frame clock: evaluations since the one that saw the latest
    // new Sleep (saturating; only small values are ever compared).
    if (progress.advanced) {
        reflexEvaluationsWithoutSleep_ = 0;
    } else if (reflexEvaluationsWithoutSleep_ != UINT32_MAX) {
        ++reflexEvaluationsWithoutSleep_;
    }
    progress.evaluationsWithoutSleep = reflexEvaluationsWithoutSleep_;
    const DWORD sleepThreadId = g_ReflexLimiter.GetLastGameSleepThreadId();
    progress.asyncToSleep = sleepThreadId != 0 && sleepThreadId != GetCurrentThreadId();
    progress.coversEvaluation = gameSleepObserved && ce::fps_limiter_policy::IsEvaluationCoveredByGameSleep(
                                                         progress.advanced, progress.evaluationsWithoutSleep,
                                                         evaluationsPerGameFrame, progress.asyncToSleep);
    progress.recentPresentGap = HasRecentLargePresentGap(500);
    if (progress.recentPresentGap && !reflexRecentPresentGap_) {
        reflexSleepBaselineCount_ = progress.gameSleepCount;
    }
    reflexRecentPresentGap_ = progress.recentPresentGap;
    progress.freshSleepCount = (progress.gameSleepCount > reflexSleepBaselineCount_)
                                   ? (progress.gameSleepCount - reflexSleepBaselineCount_)
                                   : 0;
    return progress;
}

inline bool FpsLimiter::TryHandleReflexNativeWarmup(bool requested, bool driverTargetAccepted,
                                                    const ce::fps_limiter_policy::ReflexSleepProgress& progress) {
    if (!requested || !driverTargetAccepted) {
        return false;
    }

    // A new successful game-owned Sleep already paced this recovery frame.
    // Let the accepted driver target own it while the stable streak is being
    // confirmed; adding the local fallback here creates a visibly long frame
    // exactly at FG/cutscene/overload recovery boundaries. An evaluation
    // without a new Sleep that can still be another output of a frame a Sleep
    // paced (IsEvaluationCoveredByGameSleep) is the same recovery frame.
    g_ReflexLimiter.DisableHybridPacing();
    reflexPostPresentCadencePending_ = false;
    reflexLimiterActive_ = true;
    reflexNativeSleepActive_ = true;
    loggedNativeFallback_ = false;
    isActivelyLimiting_.store(false, std::memory_order_relaxed);
    lastActualWaitUs_ = 0;
    reflexWarmupActive_ = true;
    if (!progress.advanced) {
        ++reflexWarmupSameFrameHolds_;
    } else if (fps_limiter_detail::ShouldLogReflexWarmupTransition(reflexWarmupStalls_)) {
        TraceLog(
            "Apply: REFLEX native warmup sleepCount=%u fresh=%u gap=%d sameFrame=%u perFrame=%u async=%d stalls=%u",
            progress.gameSleepCount, progress.freshSleepCount, progress.recentPresentGap ? 1 : 0,
            reflexWarmupSameFrameHolds_, progress.evaluationsPerGameFrame, progress.asyncToSleep ? 1 : 0,
            reflexWarmupStalls_);
        if (progress.freshSleepCount <= 1) {
            HookLog(
                "FPS Limiter: Reflex Sleep resumed; driver owns recovery pacing while CE confirms a stable "
                "Sleep streak (sleepCount=%u, fresh=%u, perFrame=%u, stalls=%u)",
                progress.gameSleepCount, progress.freshSleepCount, progress.evaluationsPerGameFrame,
                reflexWarmupStalls_);
        }
    }
    LARGE_INTEGER retQpc;
    retQpc.QuadPart = ReadClock();
    lastApplyReturnQpc = retQpc.QuadPart;
    return true;
}

// The game's Sleep no longer covers this evaluation while native pacing was
// live: re-base the fresh streak and hand the frame to the timer fallback.
inline void FpsLimiter::PauseReflexNativeSleep(const ce::fps_limiter_policy::ReflexSleepProgress& progress,
                                               uint32_t graceMs, bool gameSleepRecent) {
    if (!reflexNativeSleepActive_) {
        return;
    }
    reflexSleepBaselineCount_ = progress.gameSleepCount;
    reflexNativeSleepActive_ = false;
    ++reflexWarmupStalls_;
    if (!fps_limiter_detail::ShouldLogReflexWarmupTransition(reflexWarmupStalls_)) {
        loggedNativeFallback_ = true;  // the timer-fallback reason line belongs to this stall
        return;
    }
    TraceLog(
        "Apply: REFLEX sleep stalled graceMs=%u recent=%d withoutSleep=%u perFrame=%u async=%d fresh=%u stall=%u",
        graceMs, gameSleepRecent ? 1 : 0, progress.evaluationsWithoutSleep, progress.evaluationsPerGameFrame,
        progress.asyncToSleep ? 1 : 0, progress.freshSleepCount, reflexWarmupStalls_);
    HookLog(
        "FPS Limiter: Reflex Sleep paused; using timer fallback pacing (recent=%d, withoutSleep=%u, perFrame=%u, "
        "async=%d, stall #%u since the last driver handoff)",
        gameSleepRecent ? 1 : 0, progress.evaluationsWithoutSleep, progress.evaluationsPerGameFrame,
        progress.asyncToSleep ? 1 : 0, reflexWarmupStalls_);
}

// One line per warmup episode, when the driver owns pacing again.
inline void FpsLimiter::NoteReflexDriverHandoff(const ce::fps_limiter_policy::ReflexSleepProgress& progress,
                                                ce::fps_limiter_policy::PresentSite site) {
    if (!reflexWarmupActive_ && reflexWarmupStalls_ == 0) {
        return;
    }
    TraceLog("Apply: REFLEX warmup handoff fresh=%u sameFrame=%u stalls=%u perFrame=%u async=%d site=%u",
             progress.freshSleepCount, reflexWarmupSameFrameHolds_, reflexWarmupStalls_,
             progress.evaluationsPerGameFrame, progress.asyncToSleep ? 1 : 0, static_cast<unsigned>(site));
    HookLog(
        "FPS Limiter: Reflex warmup handed pacing to the driver (fresh=%u, sameFrame=%u, stalls=%u, perFrame=%u, "
        "async=%d, site=%u)",
        progress.freshSleepCount, reflexWarmupSameFrameHolds_, reflexWarmupStalls_,
        progress.evaluationsPerGameFrame, progress.asyncToSleep ? 1 : 0, static_cast<unsigned>(site));
    reflexWarmupActive_ = false;
    reflexWarmupSameFrameHolds_ = 0;
    reflexWarmupStalls_ = 0;
}
