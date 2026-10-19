#pragma once

// The limiter's 120-frame cadence report. Split out of apply.h so the hot
// Apply() path stays readable and the unit stays under the source-size
// ceiling; the emission itself is rate-limited by the stats window.

#include "hook/pacing/fps_limiter.h"

#include <cstdio>

inline void FpsLimiter::EmitLocalCadenceStats(const LocalCadenceResult& cadence, int effectiveTargetFps) {
    if (!cadence.emitStats) {
        return;
    }

    const PresentToDisplaySnapshot p2dDiagnostics = SnapshotPresentToDisplay();
    // How precisely the waits returned and whether the frame reached the GPU before them: the two
    // parts of the present timeline's jitter that are CE's own (see NoteSmartWaitWake, PreWaitSubmission).
    const SmartWaitWindow& wake = cadence.statsSmartWait;
    char wakeText[256];
    snprintf(wakeText, sizeof(wakeText),
             "wake(n=%u lateMeanUs=%lld lateMaxUs=%lld late=%u timerLate=%u marginUs=%lld fineMeanUs=%lld) "
             "preWaitSubmits=%u submitMaxUs=%lld",
             wake.waits, wake.waits != 0 ? wake.lateUsSum / static_cast<int64_t>(wake.waits) : 0, wake.lateUsMax,
             wake.fineLate + wake.timerLate, wake.timerLate, adaptiveFineMarginUs_,
             wake.waits != 0 ? wake.fineUsSum / static_cast<int64_t>(wake.waits) : 0, cadence.statsPreWaitSubmits,
             cadence.statsPreWaitSubmitUsMax);
    // Which timestamps the pre-limiter captures of this window got (fps_limiter_detail/release_forecast.h).
    char stampText[192];
    fps_limiter_detail::FormatReleaseForecastWindow(releaseForecast_.TakeWindow(), qpcFrequency, stampText,
                                                    sizeof(stampText));

    // The full line goes to fps_limiter_trace.log; hook_debug.log gets the short form only once the
    // trace file stops taking lines (cap reached or debug logging off), so the stats are never logged twice.
    const bool traced = TraceLog(
        "Apply: LOCAL timer stats frames=%u scheduledWaitUs=%lld actualWaitUs=%lld lateUs=%lld "
        "avgFps=%.1f instFps=%.1f target=%d waited=%u late=%u avgLateUs=%lld maxLateUs=%lld "
        "resets=%u phaseSkipped=%u dedup=%u activeDedup=%u frontLoad=%d budgetUs=%lld "
        "workCeilingUs=%lld headroomUs=%lld gpuHeadroomUs=%lld p2dUs=%lld p2dFloorUs=%lld overruns=%u "
        "releaseWaitUs=%lld releases=%u %s%s",
        cadence.frameCount, cadence.scheduledWaitUs, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps,
        cadence.instantFps, effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames,
        cadence.statsAvgLateUs, cadence.statsMaxLateUs, cadence.statsResetFrames,
        cadence.statsSkippedGridSlots, applyDedupCount_, applyActiveDedupCount_,
        timerPostPresentPending_ ? 1 : 0, frameWorkBudgetUs_, observedFrameWorkCeilingUs_,
        frontLoadHeadroomUs_, frontLoadGpuHeadroomUs_, p2dDiagnostics.recentUs, p2dDiagnostics.floorUs,
        frontLoadOverrunCount_, lastFrontLoadedReleaseWaitUs_, frontLoadedReleaseCount_, wakeText, stampText);
    if (!traced) {
        HookLog(
            "FPS Limiter: Local timer stats (%u frames): lastWait=%lldus late=%lldus avgFps=%.1f "
            "instFps=%.1f target=%d waited=%u lateFrames=%u resets=%u phaseSkipped=%u activeDedup=%u "
            "frontLoad=%d p2dUs=%lld p2dFloorUs=%lld %s%s",
            cadence.frameCount, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps, cadence.instantFps,
            effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames, cadence.statsResetFrames,
            cadence.statsSkippedGridSlots, applyActiveDedupCount_, timerPostPresentPending_ ? 1 : 0,
            p2dDiagnostics.recentUs, p2dDiagnostics.floorUs, wakeText, stampText);
    }
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
