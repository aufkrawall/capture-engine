#pragma once

// The limiter's 120-frame cadence report. Split out of apply.h so the hot
// Apply() path stays readable and the unit stays under the source-size
// ceiling; the emission itself is rate-limited by the stats window.

#include "../fps_limiter.h"

inline void FpsLimiter::EmitLocalCadenceStats(const LocalCadenceResult& cadence, int effectiveTargetFps) {
    if (!cadence.emitStats) {
        return;
    }

    TraceLog(
        "Apply: LOCAL timer stats frames=%u scheduledWaitUs=%lld actualWaitUs=%lld lateUs=%lld "
        "avgFps=%.1f instFps=%.1f target=%d waited=%u late=%u avgLateUs=%lld maxLateUs=%lld "
        "resets=%u phaseSkipped=%u dedup=%u activeDedup=%u frontLoad=%d budgetUs=%lld "
        "workCeilingUs=%lld headroomUs=%lld overruns=%u releaseWaitUs=%lld releases=%u",
        cadence.frameCount, cadence.scheduledWaitUs, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps,
        cadence.instantFps, effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames,
        cadence.statsAvgLateUs, cadence.statsMaxLateUs, cadence.statsResetFrames,
        cadence.statsSkippedGridSlots, applyDedupCount_, applyActiveDedupCount_,
        timerPostPresentPending_ ? 1 : 0, frameWorkBudgetUs_, observedFrameWorkCeilingUs_,
        frontLoadHeadroomUs_, frontLoadOverrunCount_, lastFrontLoadedReleaseWaitUs_, frontLoadedReleaseCount_);
    HookLog(
        "FPS Limiter: Local timer stats (%u frames): lastWait=%lldus late=%lldus avgFps=%.1f "
        "instFps=%.1f target=%d waited=%u lateFrames=%u resets=%u phaseSkipped=%u activeDedup=%u",
        cadence.frameCount, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps, cadence.instantFps,
        effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames, cadence.statsResetFrames,
        cadence.statsSkippedGridSlots, applyActiveDedupCount_);
    if (cadence.statsBoundaryCallbacks > 0 || cadence.statsGeneratedPasses > 0 ||
        cadence.statsConcurrentSkips > 0 || cadence.statsGroupResets > 0) {
        // Rate-limited by the 120-frame stats window. A nonzero concurrent
        // skip while a real-boundary limiter is active is an invariant
        // violation: boundary owners block on the cadence lock and
        // generated slots never touch it, so this must not increase.
        HookLog(
            "FPS Limiter: boundary admission stats: boundaryCallbacks=%u pacedGroups=%u generatedPasses=%u "
            "groupResets=%u concurrentSkips=%u%s",
            cadence.statsBoundaryCallbacks, cadence.statsPacedGroups, cadence.statsGeneratedPasses,
            cadence.statsGroupResets, cadence.statsConcurrentSkips,
            cadence.statsConcurrentSkips > 0 ? " [INVARIANT VIOLATION: unpaced lock-contention escape]" : "");
    }
}
