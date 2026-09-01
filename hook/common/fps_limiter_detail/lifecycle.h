#pragma once

// Out-of-line FpsLimiter member definitions. Included by fps_limiter.h
// after the class body; kept inline so the hot path is unchanged.

#include "../fps_limiter.h"

struct FpsLimiterTraceFileState {
    HANDLE file = INVALID_HANDLE_VALUE;
    char path[MAX_PATH] = {0};
    std::mutex mutex;
    ~FpsLimiterTraceFileState() {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
    }
};

inline FpsLimiterTraceFileState& GetFpsLimiterTraceFileState() {
    static FpsLimiterTraceFileState state;
    return state;
}

inline void FpsLimiter::TraceLog(const char* fmt, ...) {
    if (!HookDebugLoggingEnabled())
        return;
    if (traceLogCount_ >= 200)
        return;
    traceLogCount_++;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    int msgLen = vsnprintf(buf, sizeof(buf) - 32, fmt, args);
    va_end(args);
    if (msgLen <= 0)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[600];
    int len = snprintf(line, sizeof(line) - 1, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond,
                       st.wMilliseconds, buf);
    if (len <= 0)
        return;
    FpsLimiterTraceFileState& s_TraceState = GetFpsLimiterTraceFileState();

    std::unique_lock<std::mutex> lock(s_TraceState.mutex, std::defer_lock);
    if (!lock.try_lock())
        return;  // Drop trace if another thread is writing; never stall Present

    if (s_TraceState.path[0] == '\0') {
        BuildLogFilePathForModuleAddress((const void*)this, "fps_limiter_trace.log", s_TraceState.path,
                                         sizeof(s_TraceState.path));
    }
    if (s_TraceState.path[0] == '\0')
        return;

    if (s_TraceState.file == INVALID_HANDLE_VALUE) {
        s_TraceState.file = CreateFileA(s_TraceState.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s_TraceState.file == INVALID_HANDLE_VALUE)
            return;
    }

    DWORD written = 0;
    if (!WriteFile(s_TraceState.file, line, static_cast<DWORD>(len), &written, nullptr) ||
        written != static_cast<DWORD>(len)) {
        CloseHandle(s_TraceState.file);
        s_TraceState.file = INVALID_HANDLE_VALUE;
    }
}

inline void FpsLimiter::ResetTraceLogPath() {
    FpsLimiterTraceFileState& s_TraceState = GetFpsLimiterTraceFileState();
    std::lock_guard<std::mutex> lock(s_TraceState.mutex);
    if (s_TraceState.file != INVALID_HANDLE_VALUE) {
        CloseHandle(s_TraceState.file);
        s_TraceState.file = INVALID_HANDLE_VALUE;
    }
    s_TraceState.path[0] = '\0';
}

inline void FpsLimiter::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(eventStateMutex_);
        if (releaseEvent) {
            CloseHandle(releaseEvent);
            releaseEvent = NULL;
        }
        if (requestEvent) {
            CloseHandle(requestEvent);
            requestEvent = NULL;
        }
        eventsInitialized = false;
    }
    {
        std::lock_guard<std::mutex> lock(timerStateMutex_);
        if (highResTimer) {
            CancelWaitableTimer(highResTimer);
            CloseHandle(highResTimer);
            highResTimer = NULL;
        }
        if (timerResolutionSet) {
            if (s_TimerResolutionRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                timeEndPeriod(1);
            }
            timerResolutionSet = false;
        }
        highResTimerFailed = false;
    }
    sessionIdPublished = false;
    loggedInactive_ = false;
    loggedNoEvent_ = false;
    loggedActive_ = false;
    missedFrames = 0;
    // CRITICAL FIX: Reset per-instance log counters on shutdown
    timeoutLogCount_ = 0;
    targetLogCount_ = 0;
    targetHitLogCount_ = 0;
    lastTargetFps_ = 0;
    lastUsedCaptureSync_ = false;
    lastEffectiveMode_ = LimiterModeValues::kAuto;
    lastCaptureOutputEquivalentFps_ = 0;
    lastGeneralConstraintFps_ = 0;
    lastCaptureSourceFinalOutput_ = false;
    lastApplyReturnQpc = 0;
    isActivelyLimiting_.store(false, std::memory_order_relaxed);
    injectFinalOutputCaptureAvailable_.store(false, std::memory_order_release);
    localTargetTime_ = 0;
    localIntervalFps_ = 0;
    localIntervalRemainder_ = 0;
    localFrameCount_ = 0;
    localStatsIntervalStart_ = 0;
    localStatsFrameCount_ = 0;
    localStatsWaitedFrames_ = 0;
    localStatsLateFrames_ = 0;
    localStatsResetFrames_ = 0;
    localStatsSkippedGridSlots_ = 0;
    localStatsLateUsSum_ = 0;
    localStatsMaxLateUs_ = 0;
    nativeApiRecheckCounter_ = 0;
    reflexDeviceProvided_ = false;
    reflexNativeSleepActive_ = false;
    reflexSleepBaselineCount_ = 0;
    reflexRecentPresentGap_ = false;
    {
        std::lock_guard<std::mutex> admissionLock(admissionMutex_);
        groupAdmission_.Reset();
        lastAdmissionKey_ = 0;
    }
    boundaryCallbackCount_.store(0, std::memory_order_relaxed);
    pacedGroupCount_.store(0, std::memory_order_relaxed);
    generatedSlotPassCount_.store(0, std::memory_order_relaxed);
    groupAdmissionResetCount_.store(0, std::memory_order_relaxed);
    statsSnapshotBoundaryCallbacks_ = 0;
    statsSnapshotPacedGroups_ = 0;
    statsSnapshotGeneratedPasses_ = 0;
    statsSnapshotGroupResets_ = 0;
    statsSnapshotConcurrentSkips_ = 0;
    lastCadenceTargetFps_ = 0;
    lastCadenceScale_ = 1;
    ResetReflexNativePacingState();
    nativePacingBackend_ = {};
    g_ReflexLimiter.Shutdown();
}

inline void FpsLimiter::RecordTimerOvershoot(int64_t overshootUs) {
    timerOvershootUs_[timerOvershootCursor_] = std::clamp<int64_t>(overshootUs, 0, 2000);
    timerOvershootCursor_ = (timerOvershootCursor_ + 1) % timerOvershootUs_.size();
    if (timerOvershootSampleCount_ < timerOvershootUs_.size()) {
        ++timerOvershootSampleCount_;
    }

    std::array<int64_t, 64> sorted = timerOvershootUs_;
    std::sort(sorted.begin(), sorted.begin() + timerOvershootSampleCount_);
    const size_t p99Index = timerOvershootSampleCount_ > 1 ? ((timerOvershootSampleCount_ * 99 + 99) / 100) - 1 : 0;
    adaptiveFineMarginUs_ = std::clamp<int64_t>(sorted[p99Index] + 25, 50, 250);
}

inline void FpsLimiter::ResetReflexNativePacingState() {
    if (reflexLimiterActive_ || reflexNativeSleepActive_ || g_ReflexLimiter.GetTargetIntervalUs() != 0) {
        g_ReflexLimiter.SetTargetFps(0);
        g_ReflexLimiter.DisableHybridPacing();
    }
    g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
    reflexLimiterActive_ = false;
    reflexNativeSleepActive_ = false;
    reflexPostPresentCadencePending_ = false;
    reflexPostPresentTargetFps_ = 0;
    reflexPostPresentCaptureSync_ = false;
    reflexPostPresentPushOk_ = false;
    reflexPostPresentDeviceReady_ = false;
    reflexPostPresentRecentGap_ = false;
    reflexPostPresentSkipSleep_ = false;
    reflexPostPresentArmedLogged_ = false;
    if ((externalNativeTargetFps_ != 0 || externalNativePostPresentPending_) && nativePacingBackend_.context &&
        nativePacingBackend_.clear) {
        nativePacingBackend_.clear(nativePacingBackend_.context);
    }
    externalNativePostPresentPending_ = false;
    externalNativeTargetFps_ = 0;
    externalNativeLoggedSuccess_ = false;
    reflexSleepBaselineCount_ = 0;
    reflexLastEvaluatedGameSleepCount_ = 0;
    reflexRecentPresentGap_ = false;
    reflexLoggedSuccess_ = false;
}
