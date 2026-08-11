#pragma once

// Out-of-line ReflexLimiter member definitions. Included by reflex_limiter.h
// after the class body; kept inline so the hot path is unchanged.

#include "../reflex_limiter.h"

inline void ReflexLimiter::SetTargetFps(int fps) {
    const uint32_t oldInterval = targetIntervalUs_.load(std::memory_order_acquire);
    uint32_t newInterval = 0;
    if (fps <= 0) {
        newInterval = 0;
    } else {
        newInterval = 1000000 / fps;
    }

    if (oldInterval == newInterval) {
        return;
    }

    targetIntervalUs_.store(newInterval, std::memory_order_release);
    lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    ceOwnedSleepLogged_.store(false, std::memory_order_release);
    manualRearmBeforeNextPush_.store(newInterval != 0, std::memory_order_release);

    if (newInterval == 0) {
        if (oldInterval != 0) {
            ClearFpsLimit();
        }
        return;
    }

    HookLogImportant("ReflexLimiter: Target FPS set to %d (intervalUs=%u, inlineHooks=%d)", fps, newInterval,
                     AreInlineHooksInstalled() ? 1 : 0);
    EnsureGameOwnedReflexHooks();
}

inline void ReflexLimiter::ConfigureHybridPacing(int64_t qpcFreq, int fps) {
    if (qpcFreq <= 0 || fps <= 0) {
        hybridIntervalTicks_.store(0, std::memory_order_release);
        hybridTargetTick_.store(0, std::memory_order_release);
        return;
    }

    const int64_t newIntervalTicks = qpcFreq / fps;
    const int64_t oldIntervalTicks = hybridIntervalTicks_.load(std::memory_order_acquire);
    hybridQpcFrequency_.store(qpcFreq, std::memory_order_release);
    hybridIntervalTicks_.store(newIntervalTicks, std::memory_order_release);
    if (newIntervalTicks != oldIntervalTicks) {
        hybridTargetTick_.store(0, std::memory_order_release);
    }
}

inline void ReflexLimiter::ApplyHybridPacingBeforeNativeSleep() {
    const int64_t intervalTicks = hybridIntervalTicks_.load(std::memory_order_acquire);
    const int64_t qpcFrequency = hybridQpcFrequency_.load(std::memory_order_acquire);
    if (intervalTicks <= 0 || qpcFrequency <= 0) {
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    int64_t targetTick = hybridTargetTick_.load(std::memory_order_acquire);
    if (targetTick == 0) {
        targetTick = now.QuadPart + intervalTicks / 2;
    }

    while (targetTick > now.QuadPart) {
        const int64_t diffTicks = targetTick - now.QuadPart;
        const int64_t diffUs = (diffTicks * 1000000) / qpcFrequency;
        if (diffUs > 2000) {
            ::Sleep(0);
        } else if (diffUs > 500) {
            SwitchToThread();
        } else {
            _mm_pause();
        }
        QueryPerformanceCounter(&now);
    }

    hybridTargetTick_.store(targetTick + intervalTicks, std::memory_order_release);
}

inline bool ReflexLimiter::ClearFpsLimit() {
    auto forwardSetSleepMode = GetForwardSetSleepMode();
    if (!forwardSetSleepMode || !lastDevice_) {
        if (pushSucceeded_.exchange(false, std::memory_order_acq_rel)) {
            HookLogImportant("ReflexLimiter: Could not clear FPS limit (SetSleepMode=%p, device=%p)",
                             (void*)forwardSetSleepMode, lastDevice_);
        }
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        return false;
    }

    NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(0, false);
    NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
    if (status == NVAPI_OK) {
        if (pushSucceeded_.exchange(false, std::memory_order_acq_rel)) {
            HookLogImportant("ReflexLimiter: Cleared FPS limit (boost=%u markers=%u lowLatency=%u)",
                             params.bLowLatencyBoost, params.bUseMarkersToOptimize, params.bLowLatencyMode);
        }
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
        return true;
    }

    HookLogImportant("ReflexLimiter: Clear FPS limit failed (status=%d boost=%u markers=%u lowLatency=%u)", status,
                     params.bLowLatencyBoost, params.bUseMarkersToOptimize, params.bLowLatencyMode);
    pushSucceeded_.store(false, std::memory_order_release);
    lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    return false;
}

inline bool ReflexLimiter::PushFpsLimit() {
    auto forwardSetSleepMode = GetForwardSetSleepMode();
    if (!forwardSetSleepMode || !lastDevice_) {
        if (!loggedMissingDevice_) {
            HookLogImportant(
                "ReflexLimiter: PushFpsLimit skipped (SetSleepMode=%p, device=%p, intervalUs=%u, available=%d)",
                (void*)forwardSetSleepMode, lastDevice_, targetIntervalUs_.load(std::memory_order_acquire),
                available_.load(std::memory_order_acquire) ? 1 : 0);
            loggedMissingDevice_ = true;
        }
        pushSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    loggedMissingDevice_ = false;

    uint32_t intervalUs = targetIntervalUs_.load(std::memory_order_acquire);
    if (intervalUs == 0) {
        ClearFpsLimit();
        return false;
    }

    if (manualLimiterConfiguredOrActive_.load(std::memory_order_acquire) &&
        manualRearmBeforeNextPush_.exchange(false, std::memory_order_acq_rel)) {
        ForceLowLatencyResetBeforeManualPush(forwardSetSleepMode, intervalUs);
        pushSucceeded_.store(false, std::memory_order_release);
        lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    }

    if (pushSucceeded_.load(std::memory_order_acquire) &&
        lastPushedIntervalUs_.load(std::memory_order_acquire) == intervalUs) {
        return true;
    }

    NV_SET_SLEEP_MODE_PARAMS params = BuildSleepModeParams(intervalUs, true);

    NvAPI_Status status = forwardSetSleepMode(lastDevice_, &params);
    if (status == NVAPI_OK) {
        if (!pushSucceeded_.load(std::memory_order_acquire)) {
            HookLogImportant(
                "ReflexLimiter: Pushed FPS limit (device=%p intervalUs=%u boost=%u markers=%u lowLatency=%u "
                "version=0x%08X)",
                lastDevice_, intervalUs, params.bLowLatencyBoost, params.bUseMarkersToOptimize,
                params.bLowLatencyMode, params.version);
        }
        lastPushedIntervalUs_.store(intervalUs, std::memory_order_release);
        pushSucceeded_.store(true, std::memory_order_release);
        return true;
    }
    static std::atomic<int> s_pushFailLogCount{0};
    const int failCount = s_pushFailLogCount.fetch_add(1, std::memory_order_relaxed);
    if (failCount < 5 || (failCount % 300) == 0) {
        HookLogImportant(
            "ReflexLimiter: PushFpsLimit failed (status=%d device=%p version=0x%08X intervalUs=%u boost=%u "
            "markers=%u lowLatency=%u inlineHooks=%d gameActive=%d)",
            status, lastDevice_, params.version, intervalUs, params.bLowLatencyBoost, params.bUseMarkersToOptimize,
            params.bLowLatencyMode, AreInlineHooksInstalled() ? 1 : 0,
            gameActivated_.load(std::memory_order_acquire) ? 1 : 0);
    }
    pushSucceeded_.store(false, std::memory_order_release);
    return false;
}

inline bool ReflexLimiter::Sleep() {
    auto forwardSleep = GetForwardSleep();
    if (!forwardSleep || !lastDevice_) {
        if (!loggedMissingSleepDevice_) {
            HookLog("ReflexLimiter: Sleep skipped (Sleep=%p, device=%p)", (void*)forwardSleep, lastDevice_);
            loggedMissingSleepDevice_ = true;
        }
        sleepSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    loggedMissingSleepDevice_ = false;

    bool pushOk = PushFpsLimit();
    if (!pushOk && !gameActivated_.load(std::memory_order_acquire)) {
        static std::atomic<int> s_sleepSkippedAfterPushFailLogCount{0};
        const int skipCount = s_sleepSkippedAfterPushFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (skipCount < 5 || (skipCount % 300) == 0) {
            HookLogImportant(
                "ReflexLimiter: Sleep skipped after failed FPS-limit push (device=%p intervalUs=%u gameActive=0)",
                lastDevice_, targetIntervalUs_.load(std::memory_order_acquire));
        }
        sleepSucceeded_.store(false, std::memory_order_release);
        return false;
    }

    NvAPI_Status status = forwardSleep(lastDevice_);
    if (status == NVAPI_OK) {
        if (!pushOk && gameActivated_.load(std::memory_order_acquire)) {
            HookLog("ReflexLimiter: Sleep succeeded using game-managed SetSleepMode state");
        } else if (!ceOwnedSleepLogged_.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant("ReflexLimiter: CE-owned NvAPI Sleep succeeded (pushOk=%d intervalUs=%u)",
                             pushOk ? 1 : 0, targetIntervalUs_.load(std::memory_order_acquire));
        }
        sleepSucceeded_.store(true, std::memory_order_release);
        return true;
    }
    static std::atomic<int> s_sleepFailLogCount{0};
    const int failCount = s_sleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
    if (failCount < 5 || (failCount % 300) == 0) {
        HookLogImportant("ReflexLimiter: Sleep failed (status=%d device=%p pushOk=%d gameActive=%d intervalUs=%u)",
                         status, lastDevice_, pushOk ? 1 : 0,
                         gameActivated_.load(std::memory_order_acquire) ? 1 : 0,
                         targetIntervalUs_.load(std::memory_order_acquire));
    }
    sleepSucceeded_.store(false, std::memory_order_release);
    return false;
}

inline void ReflexLimiter::Shutdown() {
    targetIntervalUs_.store(0, std::memory_order_release);
    pushSucceeded_.store(false, std::memory_order_release);
    sleepSucceeded_.store(false, std::memory_order_release);
    loggedMissingDevice_ = false;
    loggedMissingSleepDevice_ = false;
    DisableHybridPacing();
    hasLastSleepModeParams_.store(false, std::memory_order_release);
    ZeroMemory(&lastSleepModeParams_, sizeof(lastSleepModeParams_));
    gameActivated_.store(false, std::memory_order_release);
    lastDevice_ = nullptr;
    manualLimiterConfiguredOrActive_.store(false, std::memory_order_release);
    manualRearmBeforeNextPush_.store(false, std::memory_order_release);
    lastNativePacingSignalTick_.store(0, std::memory_order_release);
    lastGameSleepTick_.store(0, std::memory_order_release);
    gameSleepObserved_.store(false, std::memory_order_release);
    gameSleepCount_.store(0, std::memory_order_release);
    lastPushedIntervalUs_.store(UINT32_MAX, std::memory_order_release);
    ceOwnedSleepLogged_.store(false, std::memory_order_release);
    // Cooperative dejection deliberately leaves code/IAT hooks resident.
    // Preserve every predecessor/trampoline and installed bit so a dormant
    // detour remains an exact forwarder and a later host can reactivate it.
}
