#pragma once

// Out-of-line FpsLimiter member definitions. Included by fps_limiter.h
// after the class body; kept inline so the hot path is unchanged.

#include "../fps_limiter.h"

inline FpsLimiter::LocalCadenceResult FpsLimiter::RunLocalCadence(int effectiveTargetFps, bool preserveCaptureSyncPhase) {
    LocalCadenceResult result;
    if (effectiveTargetFps <= 0) {
        return result;
    }

    if (qpcFrequency == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;
    }

    if (localIntervalFps_ != effectiveTargetFps) {
        localIntervalFps_ = effectiveTargetFps;
        localIntervalRemainder_ = 0;
    }
    const auto NextIntervalTicks = [&]() {
        return ce::fps_limiter_policy::NextRationalIntervalTicks(qpcFrequency, effectiveTargetFps,
                                                                 localIntervalRemainder_);
    };

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (localTargetTime_ == 0) {
        // Start later in the current frame rather than a full frame ahead.
        // This preserves the low-latency behavior expected from Reflex.
        const int64_t phaseOffsetTicks = std::max<int64_t>(1, (qpcFrequency / effectiveTargetFps) / 2);
        localTargetTime_ = now.QuadPart + phaseOffsetTicks;
        localFrameCount_ = 0;
        localStatsIntervalStart_ = now.QuadPart;
        localStatsFrameCount_ = 0;
        localStatsWaitedFrames_ = 0;
        localStatsLateFrames_ = 0;
        localStatsResetFrames_ = 0;
        localStatsSkippedGridSlots_ = 0;
        localStatsLateUsSum_ = 0;
        localStatsMaxLateUs_ = 0;
    }

    const int64_t waitTicks = localTargetTime_ - now.QuadPart;
    if (waitTicks > 0) {
        result.waited = true;
        result.scheduledWaitUs = waitTicks * 1000000 / qpcFrequency;
        ++localStatsWaitedFrames_;
    } else if (waitTicks < 0) {
        result.lateUs = (-waitTicks * 1000000) / qpcFrequency;
        ++localStatsLateFrames_;
        localStatsLateUsSum_ += result.lateUs;
        if (result.lateUs > localStatsMaxLateUs_) {
            localStatsMaxLateUs_ = result.lateUs;
        }
    }

    LARGE_INTEGER beforeWait;
    QueryPerformanceCounter(&beforeWait);
    SmartWait(localTargetTime_);
    LARGE_INTEGER afterWait;
    QueryPerformanceCounter(&afterWait);
    result.actualWaitUs = ((afterWait.QuadPart - beforeWait.QuadPart) * 1000000) / qpcFrequency;
    lastActualWaitUs_ = result.actualWaitUs;

    QueryPerformanceCounter(&now);
    if (waitTicks <= 0) {
        // Capture sync owns a cadence grid, not merely a frequency. Preserve that grid's phase
        // through a hitch so the source and immutable CFR timelines do not remain half a frame
        // apart afterward. General limiting keeps its established now-relative behavior.
        if (preserveCaptureSyncPhase) {
            const auto advance = ce::fps_limiter_policy::AdvanceCaptureSyncDeadlineAfterLateFrame(
                localTargetTime_, now.QuadPart, qpcFrequency, effectiveTargetFps, localIntervalRemainder_);
            localTargetTime_ = advance.nextTargetQpc;
            result.statsSkippedGridSlots = advance.skippedGridSlots;
            localStatsSkippedGridSlots_ += advance.skippedGridSlots;
        } else {
            localTargetTime_ = now.QuadPart + NextIntervalTicks();
        }
        result.resetCadence = true;
        ++localStatsResetFrames_;
    } else {
        localTargetTime_ += NextIntervalTicks();
    }

    localFrameCount_++;
    localStatsFrameCount_++;
    result.frameCount = localFrameCount_;

    if (localFrameCount_ % 120 == 0) {
        const int64_t intervalUs = ((now.QuadPart - localStatsIntervalStart_) * 1000000) / qpcFrequency;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        result.avgFps = (intervalUs > 0) ? (localStatsFrameCount_ * 1000000.0 / intervalUs) : 0;
        if (lastApplyEntryQpc_ != 0) {
            const int64_t interFrameUs = ((now.QuadPart - lastApplyEntryQpc_) * 1000000) / qpcFrequency;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            result.instantFps = (interFrameUs > 0) ? (1000000.0 / interFrameUs) : 0;
        }
        result.statsWaitedFrames = localStatsWaitedFrames_;
        result.statsLateFrames = localStatsLateFrames_;
        result.statsResetFrames = localStatsResetFrames_;
        result.statsSkippedGridSlots = localStatsSkippedGridSlots_;
        result.statsMaxLateUs = localStatsMaxLateUs_;
        result.statsAvgLateUs =
            (localStatsLateFrames_ > 0) ? (localStatsLateUsSum_ / static_cast<int64_t>(localStatsLateFrames_)) : 0;
        result.emitStats = true;
        localStatsIntervalStart_ = now.QuadPart;
        localStatsFrameCount_ = 0;
        localStatsWaitedFrames_ = 0;
        localStatsLateFrames_ = 0;
        localStatsResetFrames_ = 0;
        localStatsSkippedGridSlots_ = 0;
        localStatsLateUsSum_ = 0;
        localStatsMaxLateUs_ = 0;
    }

    lastApplyEntryQpc_ = now.QuadPart;
    return result;
}

inline void FpsLimiter::EnsureTimerResolution() {
    std::lock_guard<std::mutex> lock(timerStateMutex_);
    if (timerResolutionSet)
        return;

    if (s_TimerResolutionRefCount.fetch_add(1, std::memory_order_acq_rel) == 0) {
        if (timeBeginPeriod(1) != TIMERR_NOERROR) {
            s_TimerResolutionRefCount.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
    }
    timerResolutionSet = true;
}

inline bool FpsLimiter::SmartWait(int64_t targetTick) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (qpcFrequency == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;
    }

    int64_t diff = targetTick - now.QuadPart;
    if (diff <= 0)
        return false;

    // Convert to microseconds for better precision
    int64_t diffUs = (diff * 1000000) / qpcFrequency;

    const int64_t fineMarginUs = adaptiveFineMarginUs_;

    // Arm the kernel timer before the deadline, then trim only the measured
    // scheduler tail. Arming at the deadline itself turns wake-up latency
    // directly into frame-time variance.
    if (diffUs > fineMarginUs + 100 && !highResTimerFailed) {
        std::lock_guard<std::mutex> lock(timerStateMutex_);
        if (!highResTimer) {
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2
            highResTimer = CreateWaitableTimerExW(NULL, NULL, 0x2, TIMER_ALL_ACCESS);
            if (!highResTimer) {
                highResTimerFailed = true;  // Fall back to polling
            }
        }

        if (highResTimer) {
            const int64_t coarseUs = diffUs - fineMarginUs;
            const int64_t coarseTargetTick = targetTick - (fineMarginUs * qpcFrequency / 1000000);
            LARGE_INTEGER dueTime;
            dueTime.QuadPart = -coarseUs * 10;

            if (SetWaitableTimer(highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(highResTimer, static_cast<DWORD>((coarseUs + 999) / 1000 + 2));
                QueryPerformanceCounter(&now);
                const int64_t coarseOvershootUs =
                    std::max<int64_t>(0, (now.QuadPart - coarseTargetTick) * 1000000 / qpcFrequency);
                RecordTimerOvershoot(coarseOvershootUs);
                diff = targetTick - now.QuadPart;
                diffUs = (diff * 1000000) / qpcFrequency;
                if (diff <= 0)
                    return true;
                // Remaining time handled by spin-wait below
            }
        }
    }

    // The high-resolution timer normally leaves only 50-250us. Yield while
    // there is still scheduler headroom and spin only for the final 50us.
    while (diff > 0) {
        if (diffUs > 2000) {
            Sleep(1);
        } else if (diffUs > 50) {
            SwitchToThread();
        } else {
            _mm_pause();
        }

        // Recalculate remaining time
        QueryPerformanceCounter(&now);
        diff = targetTick - now.QuadPart;
        diffUs = (diff * 1000000) / qpcFrequency;
    }
    return true;
}

inline void FpsLimiter::ApplyPostPresent() {
    std::unique_lock<std::mutex> cadenceLock(cadenceMutex_, std::try_to_lock);
    if (!cadenceLock.owns_lock()) {
        concurrentApplySkips_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!reflexPostPresentCadencePending_) {
        return;
    }

    reflexPostPresentCadencePending_ = false;
    const int targetFps = reflexPostPresentTargetFps_;
    const bool preserveCaptureSyncPhase = reflexPostPresentCaptureSync_;
    if (targetFps <= 0) {
        return;
    }

    isActivelyLimiting_.store(true, std::memory_order_relaxed);

    const auto cadence = RunLocalCadence(targetFps, preserveCaptureSyncPhase);

    LARGE_INTEGER sleepStart;
    LARGE_INTEGER sleepEnd;
    bool ceOwnedSleepOk = false;
    int64_t ceOwnedSleepUs = 0;
    if (!reflexPostPresentSkipSleep_) {
        QueryPerformanceCounter(&sleepStart);
        ceOwnedSleepOk = g_ReflexLimiter.Sleep();
        QueryPerformanceCounter(&sleepEnd);
        ceOwnedSleepUs = ((sleepEnd.QuadPart - sleepStart.QuadPart) * 1000000) / qpcFrequency;
    } else {
        ceOwnedSleepOk = true;
    }

    reflexLimiterActive_ = true;
    reflexNativeSleepActive_ = false;
    loggedNativeFallback_ = false;
    lastActualWaitUs_ = cadence.actualWaitUs + ceOwnedSleepUs;

    if (!reflexLoggedSuccess_) {
        if (!reflexPostPresentSkipSleep_) {
            TraceLog(
                "Apply: REFLEX post-present cadence target=%d waitUs=%lld sleepUs=%lld sleepOk=%d push=%d "
                "device=%d gap=%d",
                targetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                reflexPostPresentPushOk_ ? 1 : 0, reflexPostPresentDeviceReady_ ? 1 : 0,
                reflexPostPresentRecentGap_ ? 1 : 0);
            HookLog(
                "FPS Limiter: Reflex explicit mode active (target=%d fps, post-present local low-latency cadence + "
                "CE-owned NvAPI Sleep, wait=%lldus, sleep=%lldus, sleepOk=%d, device=%d)",
                targetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                reflexPostPresentDeviceReady_ ? 1 : 0);
        } else {
            TraceLog(
                "Apply: REFLEX post-present cadence target=%d waitUs=%lld sleep=skip push=%d "
                "device=%d gap=%d",
                targetFps, cadence.actualWaitUs, reflexPostPresentPushOk_ ? 1 : 0,
                reflexPostPresentDeviceReady_ ? 1 : 0, reflexPostPresentRecentGap_ ? 1 : 0);
            HookLog(
                "FPS Limiter: Reflex explicit mode active (target=%d fps, post-present local cadence only, "
                "skipping CE-owned NvAPI Sleep for game-owned Reflex, wait=%lldus, device=%d)",
                targetFps, cadence.actualWaitUs, reflexPostPresentDeviceReady_ ? 1 : 0);
        }
        reflexLoggedSuccess_ = true;
    }

    if (cadence.emitStats) {
        if (!reflexPostPresentSkipSleep_) {
            TraceLog("Apply: REFLEX post-present stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d",
                     cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps);
            HookLog(
                "FPS Limiter: Reflex post-present cadence (%u frames): lastWait=%lldus avgFps=%.1f "
                "instFps=%.1f target=%d sleepOk=%d",
                cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps,
                ceOwnedSleepOk ? 1 : 0);
        } else {
            const bool diagGameSleepRecent = g_ReflexLimiter.HasRecentGameSleep(250);
            const bool diagGameActivated = g_ReflexLimiter.IsGameActivated();
            const uint32_t diagSleepCount = g_ReflexLimiter.GetGameSleepCount();
            const bool diagInlineHooks = g_ReflexLimiter.AreInlineHooksInstalled();
            TraceLog(
                "Apply: REFLEX post-present stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d "
                "sleep=skip gameAct=%d sleepRecent=%d sleepCount=%u inlineHooks=%d",
                cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps,
                diagGameActivated ? 1 : 0, diagGameSleepRecent ? 1 : 0, diagSleepCount, diagInlineHooks ? 1 : 0);
            HookLog(
                "FPS Limiter: Reflex post-present cadence (%u frames): lastWait=%lldus avgFps=%.1f "
                "instFps=%.1f target=%d sleep=skip gameAct=%d sleepRecent=%d sleepCount=%u inlineHooks=%d",
                cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps,
                diagGameActivated ? 1 : 0, diagGameSleepRecent ? 1 : 0, diagSleepCount, diagInlineHooks ? 1 : 0);
        }
    }

    LARGE_INTEGER retQpc;
    QueryPerformanceCounter(&retQpc);
    lastApplyReturnQpc = retQpc.QuadPart;
}
