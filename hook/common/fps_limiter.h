#pragma once

// clang-format off
#include <windows.h>
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
// clang-format on
#include <intrin.h>
#include <atomic>
#include <mutex>
#include "antilag2_limiter.h"
#include "fg_detection.h"
#include "fps_limiter_policy.h"
#include "hook_common.h"
#include "hook_context.h"
#include "ipc_client.h"
#include "reflex_limiter.h"
#include "xell_limiter.h"

// LimiterMode values matching the enum in config.h (duplicated here to avoid
// config.h dependency in the hook DLL which has no STL string support at load).
namespace LimiterModeValues {
constexpr uint32_t kBasic = 0;
constexpr uint32_t kFGFallback = 1;
constexpr uint32_t kNative = 2;  // NVIDIA Reflex
constexpr uint32_t kAuto = 3;
constexpr uint32_t kAntiLag2 = 4;  // AMD Anti-Lag 2
constexpr uint32_t kXeLL = 5;      // Intel XeLL
}  // namespace LimiterModeValues

// Shared FPS limiter - event-based synchronization with limiter process
// Call Apply() each frame before present
//
// Improvements:
// - Hybrid sleep/spin strategy for lower CPU usage
// - Frame drop tracking
// - Better timeout calculation
// - High-resolution waitable timer for sub-ms precision (Windows 10 1803+)
// - 1ms timer resolution via timeBeginPeriod
class FpsLimiter {
private:
    struct LocalCadenceResult {
        int64_t scheduledWaitUs = 0;
        int64_t actualWaitUs = 0;
        int64_t lateUs = 0;
        uint32_t frameCount = 0;
        uint32_t statsWaitedFrames = 0;
        uint32_t statsLateFrames = 0;
        uint32_t statsResetFrames = 0;
        int64_t statsAvgLateUs = 0;
        int64_t statsMaxLateUs = 0;
        bool emitStats = false;
        bool waited = false;
        bool resetCadence = false;
        double avgFps = 0;
        double instantFps = 0;
    };

    LocalCadenceResult RunLocalCadence(int effectiveTargetFps) {
        LocalCadenceResult result;
        if (effectiveTargetFps <= 0) {
            return result;
        }

        if (qpcFrequency == 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            qpcFrequency = freq.QuadPart;
        }

        int64_t intervalTicks = qpcFrequency / effectiveTargetFps;
        int64_t phaseOffsetTicks = intervalTicks / 2;
        if (phaseOffsetTicks < 1) {
            phaseOffsetTicks = 1;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (localTargetTime_ == 0) {
            // Start later in the current frame rather than a full frame ahead.
            // This preserves the low-latency behavior expected from Reflex.
            localTargetTime_ = now.QuadPart + phaseOffsetTicks;
            localFrameCount_ = 0;
            localStatsIntervalStart_ = now.QuadPart;
            localStatsFrameCount_ = 0;
            localStatsWaitedFrames_ = 0;
            localStatsLateFrames_ = 0;
            localStatsResetFrames_ = 0;
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

        localTargetTime_ += intervalTicks;

        QueryPerformanceCounter(&now);
        if (localTargetTime_ < now.QuadPart - intervalTicks * 2) {
            localTargetTime_ = now.QuadPart + phaseOffsetTicks;
            result.resetCadence = true;
            ++localStatsResetFrames_;
        }

        localFrameCount_++;
        localStatsFrameCount_++;
        result.frameCount = localFrameCount_;

        if (localFrameCount_ % 120 == 0) {
            const int64_t intervalUs = ((now.QuadPart - localStatsIntervalStart_) * 1000000) / qpcFrequency;
            result.avgFps = (intervalUs > 0) ? (localStatsFrameCount_ * 1000000.0 / intervalUs) : 0;
            if (lastApplyEntryQpc_ != 0) {
                const int64_t interFrameUs = ((now.QuadPart - lastApplyEntryQpc_) * 1000000) / qpcFrequency;
                result.instantFps = (interFrameUs > 0) ? (1000000.0 / interFrameUs) : 0;
            }
            result.statsWaitedFrames = localStatsWaitedFrames_;
            result.statsLateFrames = localStatsLateFrames_;
            result.statsResetFrames = localStatsResetFrames_;
            result.statsMaxLateUs = localStatsMaxLateUs_;
            result.statsAvgLateUs =
                (localStatsLateFrames_ > 0) ? (localStatsLateUsSum_ / static_cast<int64_t>(localStatsLateFrames_)) : 0;
            result.emitStats = true;
            localStatsIntervalStart_ = now.QuadPart;
            localStatsFrameCount_ = 0;
            localStatsWaitedFrames_ = 0;
            localStatsLateFrames_ = 0;
            localStatsResetFrames_ = 0;
            localStatsLateUsSum_ = 0;
            localStatsMaxLateUs_ = 0;
        }

        lastApplyEntryQpc_ = now.QuadPart;
        return result;
    }

public:
    void SetIPCClient(IPCClient* ipc) {
        this->ipc = ipc;
    }

    // For testing: inject mock shared memory
    void SetSharedMemory(SharedMemoryLayout* shm) {
        this->dbgShm = shm;
    }

    // Get count of frames where limiter couldn't keep up
    uint32_t GetMissedFrames() const {
        return missedFrames;
    }
    void ResetMissedFrames() {
        missedFrames = 0;
    }
    bool IsEventsInitialized() const {
        std::lock_guard<std::mutex> lock(eventStateMutex_);
        return eventsInitialized;
    }
    // Get last actual wait time in microseconds (for perf logging)
    int64_t GetLastWaitUs() const {
        return lastActualWaitUs_;
    }
    // Returns true when the limiter is actively pacing frames (capture_sync or general).
    // Used by Present hooks to disable vsync (SyncInterval=0) so that the limiter
    // has full control over frame pacing — vsync's vblank wait absorbs our delay otherwise.
    bool IsActivelyLimiting() const {
        return isActivelyLimiting_.load(std::memory_order_relaxed);
    }

    // Check whether the general FPS limiter is configured (enabled + fps > 0)
    // using shared-memory values, without requiring the limiter to be actively
    // pacing yet.  Used during device creation where shared memory is available
    // but the limiter Apply() hasn't run.
    static bool IsGeneralConfigured(SharedMemoryLayout* shm) {
        if (!shm)
            return false;
        return shm->fpsLimiter.GetGeneralEnabled() && shm->fpsLimiter.GetGeneralFps() > 0;
    }

    // Ensure 1ms timer resolution is enabled
    void EnsureTimerResolution() {
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

    // Smart wait until target QPC time
    // Returns true if we waited, false if we were already past target
    bool SmartWait(int64_t targetTick) {
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

        // Try high-resolution waitable timer for waits > 1ms (available on Win10
        // 1803+)
        if (diffUs > 1000 && !highResTimerFailed) {
            std::lock_guard<std::mutex> lock(timerStateMutex_);
            if (!highResTimer) {
                // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2
                highResTimer = CreateWaitableTimerExW(NULL, NULL, 0x2, TIMER_ALL_ACCESS);
                if (!highResTimer) {
                    highResTimerFailed = true;  // Fall back to polling
                }
            }

            if (highResTimer) {
                // Convert to 100ns intervals (negative = relative)
                // Use double to avoid int64 overflow when diff * 10000000 > INT64_MAX.
                // At qpcFrequency ~10MHz and diff up to ~200ms, diff*10M can exceed 2^53 in double
                // but stays well within int64 range for reasonable frame times (<1s).
                // The divide-before-multiply approach prevents overflow for large diffs.
                LARGE_INTEGER dueTime;
                dueTime.QuadPart =
                    -static_cast<int64_t>(static_cast<double>(diff) * (10000000.0 / static_cast<double>(qpcFrequency)));

                if (SetWaitableTimer(highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(highResTimer, (DWORD)(diffUs / 1000 + 5));
                    // Fall through to spin-wait for sub-μs precision trim
                    QueryPerformanceCounter(&now);
                    diff = targetTick - now.QuadPart;
                    diffUs = (diff * 1000000) / qpcFrequency;
                    if (diff <= 0)
                        return true;
                    // Remaining time handled by spin-wait below
                }
            }
        }

        // Fallback: Hybrid sleep strategy
        // - If > 2ms remaining, use Sleep(1) for power efficiency
        // - If 0.5ms - 2ms, use SwitchToThread() or Sleep(0)
        // - If < 0.5ms, spin-wait for precision
        // Exit when QPC ticks reach target (not when diffUs rounds to 0)
        while (diff > 0) {
            if (diffUs > 10000) {  // > 10ms
                Sleep(1);
            } else if (diffUs > 2000) {  // 2ms - 10ms
                Sleep(0);                // Yield but remain schedulable soon
            } else if (diffUs > 500) {
                // Very short yield
                SwitchToThread();
            } else {
                // Final <0.5ms - tight spin for sub-ms precision
                _mm_pause();
            }

            // Recalculate remaining time
            QueryPerformanceCounter(&now);
            diff = targetTick - now.QuadPart;
            diffUs = (diff * 1000000) / qpcFrequency;
        }
        return true;
    }

    // Direct trace log for debugging — bypasses all log infrastructure
    void TraceLog(const char* fmt, ...) {
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
        static std::mutex s_TraceLogMutex;
        struct TraceFileState {
            HANDLE file = INVALID_HANDLE_VALUE;
            char path[MAX_PATH] = {0};
            ~TraceFileState() {
                if (file != INVALID_HANDLE_VALUE) {
                    CloseHandle(file);
                }
            }
        };
        static TraceFileState s_TraceState;

        std::unique_lock<std::mutex> lock(s_TraceLogMutex, std::defer_lock);
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

    void ApplyPostPresent() {
        if (!reflexPostPresentCadencePending_) {
            return;
        }

        reflexPostPresentCadencePending_ = false;
        const int targetFps = reflexPostPresentTargetFps_;
        if (targetFps <= 0) {
            return;
        }

        EnsureTimerResolution();
        isActivelyLimiting_.store(true, std::memory_order_relaxed);

        const auto cadence = RunLocalCadence(targetFps);

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
                    targetFps, cadence.actualWaitUs,
                    reflexPostPresentPushOk_ ? 1 : 0, reflexPostPresentDeviceReady_ ? 1 : 0,
                    reflexPostPresentRecentGap_ ? 1 : 0);
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
                TraceLog("Apply: REFLEX post-present stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d "
                         "sleep=skip gameAct=%d sleepRecent=%d sleepCount=%u inlineHooks=%d",
                         cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps,
                         diagGameActivated ? 1 : 0, diagGameSleepRecent ? 1 : 0, diagSleepCount,
                         diagInlineHooks ? 1 : 0);
                HookLog(
                    "FPS Limiter: Reflex post-present cadence (%u frames): lastWait=%lldus avgFps=%.1f "
                    "instFps=%.1f target=%d sleep=skip gameAct=%d sleepRecent=%d sleepCount=%u inlineHooks=%d",
                    cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps, targetFps,
                    diagGameActivated ? 1 : 0, diagGameSleepRecent ? 1 : 0, diagSleepCount,
                    diagInlineHooks ? 1 : 0);
            }
        }

        LARGE_INTEGER retQpc;
        QueryPerformanceCounter(&retQpc);
        lastApplyReturnQpc = retQpc.QuadPart;
    }

    // Called each frame before present. DXGI/DX12 call sites can allow explicit
    // CE-owned Reflex pacing to defer its wait until after Present returns, so
    // the blocked time sits before the next frame's simulation/render work.
    void Apply(bool allowPostPresentReflexCadence = false) {
        SharedMemoryLayout* shm = nullptr;
        if (dbgShm) {
            shm = dbgShm;
        } else if (ipc) {
            shm = ipc->GetSharedMem();
        }

        if (!shm) {
            g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
            applyTraceCount_++;
            if (applyTraceCount_ <= 3)
                TraceLog("Apply: no shm ipc=%p", (void*)ipc);
            return;
        }

        // Dedup guard: DXVK calls Present and PresentEx sequentially on the same
        // thread for each visual frame. Each call enters DX9_PresentBegin with
        // g_PresentRecurse == 1 (they are sequential, not nested), so both fire
        // Apply(). The second call occurs within ~1ms of the first Apply() returning,
        // while the next legitimate frame's Apply() arrives at least 2ms later
        // (after Vulkan QueuePresent + game render loop). Skip if called within 2ms
        // of the previous Apply() returning.
        if (qpcFrequency == 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            qpcFrequency = freq.QuadPart;
        }
        LARGE_INTEGER nowQpc;
        QueryPerformanceCounter(&nowQpc);

        // Dedup guard: DXVK fires Present+PresentEx per render frame. The second call
        // arrives within ~1ms. Skip if called within 2ms of the previous return.
        // BUT: when the FPS limiter is active and ALLOW_TEARING disables vsync,
        // frames arrive very fast (1-2ms) and dedup would skip legitimate frames.
        // Only apply dedup when the limiter is NOT active.
        if (!isActivelyLimiting_.load(std::memory_order_relaxed)) {
            const int64_t kDedupTicks = qpcFrequency / 500;  // 2ms
            if (lastApplyReturnQpc != 0 && (nowQpc.QuadPart - lastApplyReturnQpc) < kDedupTicks) {
                applyDedupCount_++;
                lastActualWaitUs_ = 0;
                return;
            }
        }

        bool captureRequested = shm->runtimeState.captureRequested.load(std::memory_order_acquire);
        bool captureSyncEnabled = shm->fpsLimiter.GetCaptureSyncEnabled();
        int captureSyncMultiplier = shm->fpsLimiter.GetCaptureSyncMultiplier();
        bool generalEnabled = shm->fpsLimiter.GetGeneralEnabled();
        int generalFps = shm->fpsLimiter.GetGeneralFps();
        int captureFps = shm->fpsLimiter.GetCaptureFps();
        bool useVFR = shm->fpsLimiter.GetUseVFR();
        uint32_t captureSyncMode = shm->fpsLimiter.GetCaptureSyncLimiterMode();
        uint32_t generalMode = shm->fpsLimiter.GetGeneralLimiterMode();

        // Publish session ID once — use QPC ticks for better entropy
        if (!sessionIdPublished) {
            LARGE_INTEGER qpc;
            QueryPerformanceCounter(&qpc);
            uint32_t sid = GetCurrentProcessId() ^ GetTickCount() ^ static_cast<uint32_t>(qpc.QuadPart) ^
                           static_cast<uint32_t>(qpc.QuadPart >> 32);
            shm->fpsLimiter.hookSessionId.store(sid, std::memory_order_release);
            sessionIdPublished = true;
            HookLog("FPS Limiter: Published Session ID: %u", sid);
        }

        // Periodically check for native low-latency APIs (Reflex, AntiLag2, XeLL).
        // Games may load these dynamically (e.g., user enables Reflex in settings).
        // Re-check every ~250ms (15 frames at 60fps) to catch late-loaded APIs
        // and in-game Reflex toggles faster.
        nativeApiRecheckCounter_++;
        if (nativeApiRecheckCounter_ >= 15) {
            nativeApiRecheckCounter_ = 0;
            bool reflexWasAvailable = g_ReflexLimiter.IsAvailable();
            g_ReflexLimiter.Init();
            bool reflexNowAvailable = g_ReflexLimiter.IsAvailable();
            if (!reflexWasAvailable && reflexNowAvailable) {
                HookLogImportant("FPS Limiter: Reflex became available (nvapi64.dll loaded late)");
            }
        }

        // Check if limiter should be active
        bool limiterActive = false;
        int targetFps = 0;
        bool usingCaptureSync = false;
        uint32_t configuredMode = LimiterModeValues::kAuto;

        if (captureRequested && captureSyncEnabled) {
            if (captureFps > 0 && captureSyncMultiplier >= 1 && captureSyncMultiplier <= 8) {
                limiterActive = true;
                targetFps = captureFps * captureSyncMultiplier;
                usingCaptureSync = true;
                configuredMode = captureSyncMode;
            }
        } else if (generalEnabled && generalFps > 0) {
            limiterActive = true;
            targetFps = generalFps;
            configuredMode = generalMode;
        }

        // VFR Mode Passthrough: Disable limiter if VFR is active
        if (useVFR) {
            limiterActive = false;
        }

        if (!limiterActive) {
            isActivelyLimiting_.store(false, std::memory_order_relaxed);
            g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
            lastActualWaitUs_ = 0;
            loggedNativeFallback_ = false;
            ResetReflexNativePacingState();
            // Release timer resolution if we had it set
            {
                std::lock_guard<std::mutex> lock(timerStateMutex_);
                if (timerResolutionSet) {
                    if (s_TimerResolutionRefCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        timeEndPeriod(1);
                    }
                    timerResolutionSet = false;
                }
            }
            // Reset local limiter state when inactive
            if (localTargetTime_ != 0) {
                localTargetTime_ = 0;
                localFrameCount_ = 0;
                localStatsIntervalStart_ = 0;
                localStatsFrameCount_ = 0;
                localStatsWaitedFrames_ = 0;
                localStatsLateFrames_ = 0;
                localStatsResetFrames_ = 0;
                localStatsLateUsSum_ = 0;
                localStatsMaxLateUs_ = 0;
            }
            if (!loggedInactive_) {
                TraceLog("Apply: INACTIVE capReq=%d capSync=%d genEn=%d genFps=%d capFps=%d vfr=%d",
                         captureRequested ? 1 : 0, captureSyncEnabled ? 1 : 0, generalEnabled ? 1 : 0, generalFps,
                         captureFps, useVFR ? 1 : 0);
                HookLog(
                    "FPS Limiter: Inactive (general_enabled=%d, generalFps=%d, "
                    "captureSync=%d, captureRequested=%d, useVFR=%d)",
                    generalEnabled ? 1 : 0, generalFps, captureSyncEnabled ? 1 : 0, captureRequested ? 1 : 0,
                    useVFR ? 1 : 0);
                loggedInactive_ = true;
            }
            loggedActive_ = false;
            return;
        }
        loggedInactive_ = false;  // Reset so transitions back to inactive are logged

        // =====================================================================
        // Resolve effective limiter mode (auto fallback chain)
        // =====================================================================
        bool fgActive = g_FGCompat.IsFGActive();
        int fgMultiplier = fgActive ? g_FGCompat.GetFGMultiplier() : 1;
        // FG implies at least 2x output (1 real + 1 interpolated per base frame).
        // Pattern detection may determine higher (3x/4x for multi-frame gen),
        // but 2x is the safe minimum when FG is API-confirmed.
        if (fgActive && fgMultiplier < 2)
            fgMultiplier = 2;
        if (fgMultiplier > 4)
            fgMultiplier = 4;

        uint32_t effectiveMode = configuredMode;

        if (configuredMode == LimiterModeValues::kAuto) {
            // Priority: Reflex (NVIDIA, game-activated) → Anti-Lag 2 (AMD, game-activated) →
            //            XeLL (Intel, game-activated) → FG fallback → basic
            // Native modes require the game to have activated the API, not just API availability
            bool reflexAvail = g_ReflexLimiter.IsAvailable();
            bool reflexActive = g_ReflexLimiter.IsGameActivated();
            bool al2Avail = g_AntiLag2Limiter.IsAvailable();
            bool al2Active = g_AntiLag2Limiter.IsGameActivated();
            bool xellAvail = g_XeLLLimiter.IsAvailable();
            bool xellActive = g_XeLLLimiter.IsGameActivated();

            if (reflexAvail && reflexActive) {
                effectiveMode = LimiterModeValues::kNative;
            } else if (al2Avail && al2Active) {
                effectiveMode = LimiterModeValues::kAntiLag2;
            } else if (xellAvail && xellActive) {
                effectiveMode = LimiterModeValues::kXeLL;
            } else if (fgActive) {
                // No native low-latency API but FG active → FG-compatible fallback
                effectiveMode = LimiterModeValues::kFGFallback;
            } else {
                effectiveMode = LimiterModeValues::kBasic;
            }

            // Log auto mode decision (only on changes or first activation)
            static uint32_t lastLoggedAutoMode = 0;
            if (effectiveMode != lastLoggedAutoMode || !loggedActive_) {
                lastLoggedAutoMode = effectiveMode;
                const char* reason = "";
                if (effectiveMode == LimiterModeValues::kNative)
                    reason = "reflex available + game activated";
                else if (effectiveMode == LimiterModeValues::kAntiLag2)
                    reason = "anti-lag2 available + game activated";
                else if (effectiveMode == LimiterModeValues::kXeLL)
                    reason = "xell available + game activated";
                else if (effectiveMode == LimiterModeValues::kFGFallback)
                    reason = "frame generation active";
                else if (effectiveMode == LimiterModeValues::kBasic)
                    reason = "no native API active";

                HookLog("FPS Limiter [AUTO]: reflex=%s(%s) antiLag2=%s(%s) xell=%s(%s) fg=%s → selected=%s (%s)",
                        reflexAvail ? "avail" : "n/a", reflexActive ? "active" : "inactive", al2Avail ? "avail" : "n/a",
                        al2Active ? "active" : "inactive", xellAvail ? "avail" : "n/a",
                        xellActive ? "active" : "inactive", fgActive ? "yes" : "no",
                        (effectiveMode == LimiterModeValues::kNative)       ? "reflex"
                        : (effectiveMode == LimiterModeValues::kAntiLag2)   ? "anti_lag2"
                        : (effectiveMode == LimiterModeValues::kXeLL)       ? "xell"
                        : (effectiveMode == LimiterModeValues::kFGFallback) ? "fg_fallback"
                                                                            : "basic",
                        reason);
            }
        }

        // Validate: native modes require the respective DLL to be available.
        // In explicit mode, availability is sufficient (user override).
        // In auto mode, we already checked game activation above.
        // Fall back gracefully if the selected mode is not supported on this system.
        if (effectiveMode == LimiterModeValues::kNative && !g_ReflexLimiter.IsAvailable()) {
            effectiveMode = fgActive ? LimiterModeValues::kFGFallback : LimiterModeValues::kBasic;
        }
        if (effectiveMode == LimiterModeValues::kAntiLag2 && !g_AntiLag2Limiter.IsAvailable()) {
            effectiveMode = fgActive ? LimiterModeValues::kFGFallback : LimiterModeValues::kBasic;
        }
        if (effectiveMode == LimiterModeValues::kXeLL && !g_XeLLLimiter.IsAvailable()) {
            effectiveMode = fgActive ? LimiterModeValues::kFGFallback : LimiterModeValues::kBasic;
        }
        if (effectiveMode != LimiterModeValues::kNative) {
            loggedNativeFallback_ = false;
            reflexSleepBaselineCount_ = 0;
            reflexRecentPresentGap_ = false;
        }

        // FG-aware FPS adjustment for FG fallback and all native low-latency modes:
        // When FG is active, the output frame rate is fgMultiplier × base rate.
        // To hit targetFps output, the base game needs to render at targetFps / fgMultiplier.
        int effectiveTargetFps = targetFps;
        bool isNativeMode =
            (effectiveMode == LimiterModeValues::kNative || effectiveMode == LimiterModeValues::kAntiLag2 ||
             effectiveMode == LimiterModeValues::kXeLL);
        const bool explicitReflexMode = configuredMode == LimiterModeValues::kNative;
        g_ReflexLimiter.SetManualLimiterConfiguredOrActive(limiterActive && explicitReflexMode);
        if (fgActive && (effectiveMode == LimiterModeValues::kFGFallback || isNativeMode)) {
            effectiveTargetFps = targetFps / fgMultiplier;
            if (effectiveTargetFps < 1)
                effectiveTargetFps = 1;
        }

        // Log mode transitions - always log on first activation to confirm mode
        if (!loggedActive_ || lastTargetFps_ != effectiveTargetFps || lastUsedCaptureSync_ != usingCaptureSync ||
            lastEffectiveMode_ != effectiveMode) {
            // Reset pacing cadence immediately when FPS or mode changes so hot
            // config reloads apply on the next frame instead of riding stale state.
            localTargetTime_ = 0;
            localFrameCount_ = 0;
            localStatsIntervalStart_ = 0;
            localStatsFrameCount_ = 0;
            localStatsWaitedFrames_ = 0;
            localStatsLateFrames_ = 0;
            localStatsResetFrames_ = 0;
            localStatsLateUsSum_ = 0;
            localStatsMaxLateUs_ = 0;
            lastApplyEntryQpc_ = 0;
            applyInterFrameSum_ = 0;
            applyInterFrameCount_ = 0;
            reflexPostPresentCadencePending_ = false;
            reflexPostPresentSkipSleep_ = false;
            reflexPostPresentArmedLogged_ = false;

            const char* modeStr = "basic";
            if (effectiveMode == LimiterModeValues::kFGFallback)
                modeStr = "fg_fallback";
            else if (effectiveMode == LimiterModeValues::kNative)
                modeStr = "reflex";
            else if (effectiveMode == LimiterModeValues::kAntiLag2)
                modeStr = "anti_lag2";
            else if (effectiveMode == LimiterModeValues::kXeLL)
                modeStr = "xell";
            else if (effectiveMode == LimiterModeValues::kAuto)
                modeStr = "auto";

            // Check if native API is actually available (not just selected)
            const char* availNote = "";
            if (effectiveMode == LimiterModeValues::kNative && !g_ReflexLimiter.IsAvailable())
                availNote = " [API UNAVAILABLE - will fallback]";
            else if (effectiveMode == LimiterModeValues::kAntiLag2 && !g_AntiLag2Limiter.IsAvailable())
                availNote = " [API UNAVAILABLE - will fallback]";
            else if (effectiveMode == LimiterModeValues::kXeLL && !g_XeLLLimiter.IsAvailable())
                availNote = " [API UNAVAILABLE - will fallback]";

            TraceLog("Apply: ACTIVE sync=%s limiter=%s target=%d effective=%d fg=%d fgMult=%d",
                     usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, fgActive ? 1 : 0,
                     fgMultiplier);
            HookLog("FPS Limiter: Active (sync=%s, limiter=%s, target=%d, effective=%d, fg=%d/%dx, capReq=%d)%s",
                    usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, fgActive ? 1 : 0,
                    fgMultiplier, captureRequested ? 1 : 0, availNote);
            loggedActive_ = true;
            lastTargetFps_ = effectiveTargetFps;
            lastUsedCaptureSync_ = usingCaptureSync;
            lastEffectiveMode_ = effectiveMode;
        }

        // =====================================================================
        // Native (Reflex) mode: delegate pacing to the driver's pipeline
        // =====================================================================
        if (effectiveMode == LimiterModeValues::kNative && g_ReflexLimiter.IsAvailable()) {
            // Lazy init: provide device from HookContext if not yet set
            if (!reflexDeviceProvided_) {
                auto* ctx = ce::GetHookContext();
                if (ctx) {
                    IUnknown* dev = nullptr;
                    if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX11) {
                        dev = static_cast<IUnknown*>(ctx->graphicsData.dx11.device);
                    } else if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                        dev = static_cast<IUnknown*>(ctx->graphicsData.dx12.device);
                    }
                    if (dev) {
                        g_ReflexLimiter.SetDevice(dev);
                        reflexDeviceProvided_ = true;
                    }
                }
            }

            g_ReflexLimiter.SetTargetFps(effectiveTargetFps);
            const bool gameSleepObserved = g_ReflexLimiter.HasObservedGameSleep();
            uint32_t reflexSleepGraceMs = 50;
            if (effectiveTargetFps > 0) {
                uint32_t frameTimeMs = static_cast<uint32_t>(1000 / effectiveTargetFps);
                if (frameTimeMs == 0) {
                    frameTimeMs = 1;
                }
                const uint32_t dynamicGraceMs = frameTimeMs * 3;
                if (dynamicGraceMs > reflexSleepGraceMs) {
                    reflexSleepGraceMs = dynamicGraceMs;
                }
                if (reflexSleepGraceMs > 250) {
                    reflexSleepGraceMs = 250;
                }
            }
            const bool gameSleepRecent = g_ReflexLimiter.HasRecentGameSleep(reflexSleepGraceMs);
            const bool gameActivated = g_ReflexLimiter.IsGameActivated();
            const uint32_t gameSleepCount = g_ReflexLimiter.GetGameSleepCount();
            const bool recentPresentGap = HasRecentLargePresentGap(500);
            if (recentPresentGap && !reflexRecentPresentGap_) {
                reflexSleepBaselineCount_ = gameSleepCount;
            }
            reflexRecentPresentGap_ = recentPresentGap;
            const uint32_t freshSleepCount =
                (gameSleepCount > reflexSleepBaselineCount_) ? (gameSleepCount - reflexSleepBaselineCount_) : 0;
            const auto reflexDecision = ce::fps_limiter_policy::ResolveReflexPacingDecision(
                explicitReflexMode, gameSleepObserved, gameSleepRecent, freshSleepCount, recentPresentGap);
            const bool reflexHandoffReady = reflexDecision.useGameSleepHandoff;
            const bool reflexPushOk = g_ReflexLimiter.PushFpsLimit();
            const bool reflexDeviceReady = g_ReflexLimiter.HasDevice();

            if (reflexHandoffReady) {
                reflexLimiterActive_ = true;
                loggedNativeFallback_ = false;
                g_ReflexLimiter.ConfigureHybridPacing(qpcFrequency, effectiveTargetFps);

                if (!reflexNativeSleepActive_) {
                    reflexNativeSleepActive_ = true;
                    TraceLog("Apply: REFLEX native resume");
                    HookLog("FPS Limiter: Reflex Sleep resumed; returning to native pacing");
                }

                if (!reflexLoggedSuccess_) {
                    TraceLog("Apply: REFLEX hybrid target=%d gameSleep=%d gameActive=%d fresh=%u", effectiveTargetFps,
                             gameSleepObserved ? 1 : 0, gameActivated ? 1 : 0, freshSleepCount);
                    HookLog(
                        "FPS Limiter: Reflex hybrid active (target=%d fps, native low-latency + local pacing, "
                        "gameSleep=%d, gameActive=%d, freshSleep=%u)",
                        effectiveTargetFps, gameSleepObserved ? 1 : 0, gameActivated ? 1 : 0, freshSleepCount);
                    reflexLoggedSuccess_ = true;
                }

                isActivelyLimiting_.store(false, std::memory_order_relaxed);
                lastActualWaitUs_ = 0;
                LARGE_INTEGER retQpc;
                QueryPerformanceCounter(&retQpc);
                lastApplyReturnQpc = retQpc.QuadPart;
                return;
            } else {
                g_ReflexLimiter.DisableHybridPacing();
                const bool ceOwnedSleepCandidate = reflexDecision.useExplicitLocalCadence;
                bool ceOwnedSleepOk = false;
                int64_t ceOwnedSleepUs = 0;
                if (ceOwnedSleepCandidate) {
                    EnsureTimerResolution();
                    isActivelyLimiting_.store(true, std::memory_order_relaxed);
                    if (ce::fps_limiter_policy::ShouldRunExplicitReflexCadencePostPresent(
                            reflexDecision, allowPostPresentReflexCadence)) {
                        reflexPostPresentCadencePending_ = true;
                        reflexPostPresentTargetFps_ = effectiveTargetFps;
                        reflexPostPresentPushOk_ = reflexPushOk;
                        reflexPostPresentDeviceReady_ = reflexDeviceReady;
                        reflexPostPresentRecentGap_ = recentPresentGap;
                        reflexPostPresentSkipSleep_ = reflexPushOk;
                        reflexLimiterActive_ = true;
                        reflexNativeSleepActive_ = false;
                        loggedNativeFallback_ = false;
                        lastActualWaitUs_ = 0;
                        if (!reflexPostPresentArmedLogged_) {
                            TraceLog("Apply: REFLEX post-present armed target=%d push=%d device=%d gap=%d "
                                     "skipSleep=%d",
                                     effectiveTargetFps, reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0,
                                     recentPresentGap ? 1 : 0, reflexPostPresentSkipSleep_ ? 1 : 0);
                            HookLog(
                                "FPS Limiter: Reflex explicit mode armed for post-present pacing "
                                "(target=%d fps, push=%d, device=%d, skipSleep=%d)",
                                effectiveTargetFps, reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0,
                                reflexPostPresentSkipSleep_ ? 1 : 0);
                            reflexPostPresentArmedLogged_ = true;
                        }
                        LARGE_INTEGER retQpc;
                        QueryPerformanceCounter(&retQpc);
                        lastApplyReturnQpc = retQpc.QuadPart;
                        return;
                    }
                    const auto cadence = RunLocalCadence(effectiveTargetFps);

                    LARGE_INTEGER sleepStart;
                    LARGE_INTEGER sleepEnd;
                    QueryPerformanceCounter(&sleepStart);
                    ceOwnedSleepOk = g_ReflexLimiter.Sleep();
                    QueryPerformanceCounter(&sleepEnd);
                    ceOwnedSleepUs = ((sleepEnd.QuadPart - sleepStart.QuadPart) * 1000000) / qpcFrequency;

                    reflexLimiterActive_ = true;
                    reflexNativeSleepActive_ = false;
                    loggedNativeFallback_ = false;
                    lastActualWaitUs_ = cadence.actualWaitUs + ceOwnedSleepUs;
                    if (!reflexLoggedSuccess_) {
                        TraceLog(
                            "Apply: REFLEX local cadence target=%d waitUs=%lld sleepUs=%lld sleepOk=%d push=%d "
                            "device=%d gap=%d",
                            effectiveTargetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                            reflexPushOk ? 1 : 0, reflexDeviceReady ? 1 : 0, recentPresentGap ? 1 : 0);
                        HookLog(
                            "FPS Limiter: Reflex explicit mode active (target=%d fps, local low-latency cadence + "
                            "CE-owned NvAPI Sleep, wait=%lldus, sleep=%lldus, sleepOk=%d, device=%d)",
                            effectiveTargetFps, cadence.actualWaitUs, ceOwnedSleepUs, ceOwnedSleepOk ? 1 : 0,
                            reflexDeviceReady ? 1 : 0);
                        reflexLoggedSuccess_ = true;
                    }
                    if (cadence.emitStats) {
                        TraceLog("Apply: REFLEX local stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d",
                                 cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps,
                                 effectiveTargetFps);
                        HookLog(
                            "FPS Limiter: Reflex local cadence (%u frames): lastWait=%lldus avgFps=%.1f "
                            "instFps=%.1f target=%d sleepOk=%d",
                            cadence.frameCount, cadence.scheduledWaitUs, cadence.avgFps, cadence.instantFps,
                            effectiveTargetFps, ceOwnedSleepOk ? 1 : 0);
                    }
                    LARGE_INTEGER retQpc;
                    QueryPerformanceCounter(&retQpc);
                    lastApplyReturnQpc = retQpc.QuadPart;
                    return;
                }

                if (reflexNativeSleepActive_) {
                    reflexSleepBaselineCount_ = gameSleepCount;
                    reflexNativeSleepActive_ = false;
                    TraceLog("Apply: REFLEX sleep stalled graceMs=%u", reflexSleepGraceMs);
                    HookLog("FPS Limiter: Reflex Sleep paused; using timer fallback pacing");
                }
                reflexNativeSleepActive_ = false;
                reflexLimiterActive_ = false;
                if (!loggedNativeFallback_) {
                    TraceLog(
                        "Apply: REFLEX timer fallback gameActive=%d gameSleep=%d push=%d sleepCount=%u fresh=%u "
                        "recent=%d gap=%d device=%d inlineHooks=%d",
                        gameActivated ? 1 : 0, gameSleepObserved ? 1 : 0, reflexPushOk ? 1 : 0, gameSleepCount,
                        freshSleepCount, gameSleepRecent ? 1 : 0, recentPresentGap ? 1 : 0, reflexDeviceReady ? 1 : 0,
                        g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                    if (recentPresentGap) {
                        HookLog(
                            "FPS Limiter: Recent Present gap detected during Reflex activation; holding timer fallback "
                            "until pacing restabilizes");
                    } else if (gameSleepObserved && freshSleepCount < 3) {
                        HookLog(
                            "FPS Limiter: Reflex Sleep observed but waiting for a fresh stable Sleep streak; using "
                            "timer fallback");
                    } else if (reflexPushOk) {
                        HookLog(
                            "FPS Limiter: Reflex armed but native Sleep cadence is not stable yet; using timer "
                            "fallback (gameSleep=%d sleepRecent=%d sleepCount=%u fresh=%u inlineHooks=%d)",
                            gameSleepObserved ? 1 : 0, gameSleepRecent ? 1 : 0, gameSleepCount, freshSleepCount,
                            g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                    } else {
                        HookLog("FPS Limiter: Reflex native mode unavailable at runtime; using timer fallback");
                    }
                    loggedNativeFallback_ = true;
                } else {
                    static uint32_t s_fallbackDiagCounter = 0;
                    s_fallbackDiagCounter++;
                    if (s_fallbackDiagCounter % 600 == 0) {
                        HookLog(
                            "FPS Limiter: Reflex timer fallback diagnostic (frame %u): gameActive=%d sleepObserved=%d "
                            "sleepRecent=%d sleepCount=%u fresh=%u push=%d gap=%d inlineHooks=%d",
                            s_fallbackDiagCounter, gameActivated ? 1 : 0, gameSleepObserved ? 1 : 0,
                            gameSleepRecent ? 1 : 0, gameSleepCount, freshSleepCount, reflexPushOk ? 1 : 0,
                            recentPresentGap ? 1 : 0, g_ReflexLimiter.AreInlineHooksInstalled() ? 1 : 0);
                    }
                }
            }
        } else if (reflexLimiterActive_ || reflexNativeSleepActive_ || g_ReflexLimiter.GetTargetIntervalUs() != 0) {
            // Clear any stale Reflex override even if native pacing never fully
            // handed off. Otherwise later game-managed Reflex calls can inherit
            // our old interval after FG turns off.
            ResetReflexNativePacingState();
        } else if (!explicitReflexMode) {
            g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
        }

        // =====================================================================
        // AMD Anti-Lag 2 mode: delegate pacing to AMD driver extension
        // =====================================================================
        if (effectiveMode == LimiterModeValues::kAntiLag2) {
            // Lazy init: Anti-Lag 2 requires a DX12 device (DX12 only)
            if (!antilag2InitAttempted_) {
                auto* ctx = ce::GetHookContext();
                if (ctx && ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                    auto* dev = static_cast<ID3D12Device*>(ctx->graphicsData.dx12.device);
                    g_AntiLag2Limiter.Init(dev);
                }
                antilag2InitAttempted_ = true;
            }

            g_AntiLag2Limiter.SetTargetFps(effectiveTargetFps);

            if (g_AntiLag2Limiter.Update()) {
                isActivelyLimiting_.store(false, std::memory_order_relaxed);
                lastActualWaitUs_ = 0;
                LARGE_INTEGER retQpc;
                QueryPerformanceCounter(&retQpc);
                lastApplyReturnQpc = retQpc.QuadPart;
                return;
            }

            // Anti-Lag 2 update failed — fall through to timer-based limiting
        }

        // =====================================================================
        // Intel XeLL mode: delegate pacing to Intel Arc driver
        // =====================================================================
        if (effectiveMode == LimiterModeValues::kXeLL) {
            // Lazy init: XeLL requires a DX12 device (DX12 only)
            if (!xellInitAttempted_) {
                auto* ctx = ce::GetHookContext();
                if (ctx && ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                    auto* dev = static_cast<ID3D12Device*>(ctx->graphicsData.dx12.device);
                    g_XeLLLimiter.Init(dev);
                }
                xellInitAttempted_ = true;
            }

            g_XeLLLimiter.SetTargetFps(effectiveTargetFps);

            if (g_XeLLLimiter.Sleep()) {
                isActivelyLimiting_.store(false, std::memory_order_relaxed);
                lastActualWaitUs_ = 0;
                LARGE_INTEGER retQpc;
                QueryPerformanceCounter(&retQpc);
                lastApplyReturnQpc = retQpc.QuadPart;
                return;
            }

            // XeLL sleep failed — fall through to timer-based limiting
        }

        // =====================================================================
        // Timer-based limiting (basic or FG fallback)
        // Both use the same SmartWait mechanism; the only difference is that
        // FG fallback has already adjusted effectiveTargetFps above.
        // =====================================================================

        // Ensure 1ms timer resolution when limiter is active
        EnsureTimerResolution();
        isActivelyLimiting_.store(true, std::memory_order_relaxed);

        if (effectiveTargetFps <= 0)
            effectiveTargetFps = 60;

        const bool localCadenceFirstFrame = localTargetTime_ == 0;
        if (!usingCaptureSync && !localCadenceFirstFrame && lastApplyReturnQpc != 0) {
            LARGE_INTEGER activeDedupQpc;
            QueryPerformanceCounter(&activeDedupQpc);
            int64_t activeDedupTicks = qpcFrequency / 500;  // 2ms maximum duplicate window.
            int64_t intervalTicks = qpcFrequency / effectiveTargetFps;
            if (intervalTicks < 1) {
                intervalTicks = 1;
            }
            const int64_t intervalBoundTicks = intervalTicks / 3;
            if (intervalBoundTicks > 0 && intervalBoundTicks < activeDedupTicks) {
                activeDedupTicks = intervalBoundTicks;
            }
            const int64_t minDedupTicks = qpcFrequency / 2000;  // 0.5ms minimum for timer jitter.
            if (activeDedupTicks < minDedupTicks) {
                activeDedupTicks = minDedupTicks;
            }

            const int64_t sinceReturnTicks = activeDedupQpc.QuadPart - lastApplyReturnQpc;
            if (sinceReturnTicks >= 0 && sinceReturnTicks < activeDedupTicks) {
                applyActiveDedupCount_++;
                lastActualWaitUs_ = 0;
                const int64_t sinceReturnUs = (sinceReturnTicks * 1000000) / qpcFrequency;
                const int64_t activeDedupUs = (activeDedupTicks * 1000000) / qpcFrequency;
                if (applyActiveDedupCount_ <= 12 || (applyActiveDedupCount_ % 600) == 0) {
                    TraceLog(
                        "Apply: ACTIVE dedup sync=%s mode=%u configured=%u target=%d effective=%d "
                        "sinceReturnUs=%lld thresholdUs=%lld activeDedup=%u inactiveDedup=%u",
                        usingCaptureSync ? "capture" : "general", effectiveMode, configuredMode, targetFps,
                        effectiveTargetFps, sinceReturnUs, activeDedupUs, applyActiveDedupCount_, applyDedupCount_);
                }
                return;
            }
        }

        // Timer fallback/basic/FG fallback pacing is hook-local.  Waiting for
        // the helper process here is fragile because per-game config can enable
        // the limiter after startup; an unanswered event used to cost one full
        // timeout per frame before local fallback ran.
        const auto cadence = RunLocalCadence(effectiveTargetFps);
        if (localCadenceFirstFrame) {
            TraceLog("Apply: LOCAL timer start sync=%s mode=%u configured=%u target=%d effective=%d events=%d/%d "
                     "firstWaitUs=%lld firstLateUs=%lld",
                     usingCaptureSync ? "capture" : "general", effectiveMode, configuredMode, targetFps,
                     effectiveTargetFps, releaseEvent ? 1 : 0, requestEvent ? 1 : 0, cadence.scheduledWaitUs,
                     cadence.lateUs);
            HookLog("FPS Limiter: Local timer cadence active (sync=%s, mode=%u, target=%d, effective=%d)",
                    usingCaptureSync ? "capture" : "general", effectiveMode, targetFps, effectiveTargetFps);
        }
        if (cadence.emitStats) {
            TraceLog("Apply: LOCAL timer stats frames=%u scheduledWaitUs=%lld actualWaitUs=%lld lateUs=%lld "
                     "avgFps=%.1f instFps=%.1f target=%d waited=%u late=%u avgLateUs=%lld maxLateUs=%lld "
                     "resets=%u dedup=%u activeDedup=%u",
                     cadence.frameCount, cadence.scheduledWaitUs, cadence.actualWaitUs, cadence.lateUs,
                     cadence.avgFps, cadence.instantFps, effectiveTargetFps, cadence.statsWaitedFrames,
                     cadence.statsLateFrames, cadence.statsAvgLateUs, cadence.statsMaxLateUs,
                     cadence.statsResetFrames, applyDedupCount_, applyActiveDedupCount_);
            HookLog(
                "FPS Limiter: Local timer stats (%u frames): lastWait=%lldus late=%lldus avgFps=%.1f "
                "instFps=%.1f target=%d waited=%u lateFrames=%u resets=%u activeDedup=%u",
                cadence.frameCount, cadence.actualWaitUs, cadence.lateUs, cadence.avgFps, cadence.instantFps,
                effectiveTargetFps, cadence.statsWaitedFrames, cadence.statsLateFrames, cadence.statsResetFrames,
                applyActiveDedupCount_);
        }

        // Record time Apply() returned so sequential duplicate presents
        // (e.g. DXVK Present+PresentEx) are deduped on the next call.
        QueryPerformanceCounter(&nowQpc);
        lastApplyReturnQpc = nowQpc.QuadPart;
    }

    void Shutdown() {
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
        lastApplyReturnQpc = 0;
        isActivelyLimiting_.store(false, std::memory_order_relaxed);
        localTargetTime_ = 0;
        localFrameCount_ = 0;
        localStatsIntervalStart_ = 0;
        localStatsFrameCount_ = 0;
        localStatsWaitedFrames_ = 0;
        localStatsLateFrames_ = 0;
        localStatsResetFrames_ = 0;
        localStatsLateUsSum_ = 0;
        localStatsMaxLateUs_ = 0;
        nativeApiRecheckCounter_ = 0;
        reflexDeviceProvided_ = false;
        reflexNativeSleepActive_ = false;
        reflexSleepBaselineCount_ = 0;
        reflexRecentPresentGap_ = false;
        ResetReflexNativePacingState();
        g_ReflexLimiter.Shutdown();
        antilag2InitAttempted_ = false;
        g_AntiLag2Limiter.Shutdown();
        xellInitAttempted_ = false;
        g_XeLLLimiter.Shutdown();
    }

private:
    void ResetReflexNativePacingState() {
        if (reflexLimiterActive_ || reflexNativeSleepActive_ || g_ReflexLimiter.GetTargetIntervalUs() != 0) {
            g_ReflexLimiter.SetTargetFps(0);
            g_ReflexLimiter.DisableHybridPacing();
        }
        g_ReflexLimiter.SetManualLimiterConfiguredOrActive(false);
        reflexLimiterActive_ = false;
        reflexNativeSleepActive_ = false;
        reflexPostPresentCadencePending_ = false;
        reflexPostPresentTargetFps_ = 0;
        reflexPostPresentPushOk_ = false;
        reflexPostPresentDeviceReady_ = false;
        reflexPostPresentRecentGap_ = false;
        reflexPostPresentSkipSleep_ = false;
        reflexPostPresentArmedLogged_ = false;
        reflexSleepBaselineCount_ = 0;
        reflexRecentPresentGap_ = false;
        reflexLoggedSuccess_ = false;
    }

    IPCClient* ipc = nullptr;
    SharedMemoryLayout* dbgShm = nullptr;  // Direct injection for testing
    HANDLE releaseEvent = NULL;
    HANDLE requestEvent = NULL;
    HANDLE highResTimer = NULL;  // High-resolution waitable timer (Win10 1803+)
    bool eventsInitialized = false;
    bool sessionIdPublished = false;
    bool timerResolutionSet = false;  // Whether timeBeginPeriod(1) was called
    bool highResTimerFailed = false;  // Fall back to polling if timer creation fails
    bool loggedInactive_ = false;     // Tracks whether the inactive log was already emitted
    bool loggedNoEvent_ = false;      // Tracks whether the no-event warning was already emitted
    bool loggedActive_ = false;       // Tracks whether the active-state log was already emitted
    int64_t qpcFrequency = 0;
    uint32_t missedFrames = 0;  // Track frames where limiter couldn't keep up
    // CRITICAL FIX: Per-instance log counters (was static, never reset)
    int timeoutLogCount_ = 0;
    int targetLogCount_ = 0;
    int targetHitLogCount_ = 0;
    int lastTargetFps_ = 0;
    bool lastUsedCaptureSync_ = false;
    uint32_t lastEffectiveMode_ = LimiterModeValues::kAuto;  // Track mode changes for logging
    int nativeApiRecheckCounter_ = 0;                        // Frame counter for periodic native API re-check
    bool reflexLimiterActive_ = false;                       // True when Reflex is handling pacing
    bool reflexDeviceProvided_ = false;                      // True once we've given device to ReflexLimiter
    bool reflexNativeSleepActive_ = false;                   // True while recent game Sleep calls are pacing natively
    bool reflexLoggedSuccess_ = false;                       // True once we've logged successful Reflex activation
    bool loggedNativeFallback_ = false;                      // Avoid spam when native mode falls back to timer
    bool reflexPostPresentCadencePending_ = false;           // True when explicit Reflex waits after Present returns
    int reflexPostPresentTargetFps_ = 0;                     // Target for pending post-present Reflex cadence
    bool reflexPostPresentPushOk_ = false;                   // Pre-present push state captured for diagnostics
    bool reflexPostPresentDeviceReady_ = false;              // Device state captured for diagnostics
    bool reflexPostPresentRecentGap_ = false;                // Present-gap state captured for diagnostics
    bool reflexPostPresentSkipSleep_ = false;                // Skip CE-owned Sleep in ApplyPostPresent (game owns Reflex)
    bool reflexPostPresentArmedLogged_ = false;              // Avoid spam when arming post-present cadence
    uint32_t reflexSleepBaselineCount_ =
        0;  // Sleep count at the last disruption; native handoff needs a fresh streak after it
    bool reflexRecentPresentGap_ = false;          // Edge detector for recent large Present gaps
    bool antilag2InitAttempted_ = false;           // Lazy init flag for Anti-Lag 2
    bool xellInitAttempted_ = false;               // Lazy init flag for XeLL
    int64_t lastApplyReturnQpc = 0;                // QPC tick when Apply() last returned from wait (dedup guard)
    int64_t localTargetTime_ = 0;                  // QPC target for local capture sync cadence
    uint32_t localFrameCount_ = 0;                 // Frame count for local capture sync stats
    int64_t localStatsIntervalStart_ = 0;          // QPC start of current stats interval
    uint32_t localStatsFrameCount_ = 0;            // Frame count within current stats interval
    uint32_t localStatsWaitedFrames_ = 0;          // Frames in current interval where local cadence waited
    uint32_t localStatsLateFrames_ = 0;            // Frames in current interval that arrived after the target
    uint32_t localStatsResetFrames_ = 0;           // Cadence resets caused by long gaps or slow frames
    int64_t localStatsLateUsSum_ = 0;              // Sum of late frame time in current interval
    int64_t localStatsMaxLateUs_ = 0;              // Worst late frame time in current interval
    int64_t lastActualWaitUs_ = 0;                 // Last Apply() actual wait time in μs
    std::atomic<bool> isActivelyLimiting_{false};  // True when limiter is actively pacing frames
    uint32_t applyActiveDedupCount_ = 0;
    uint32_t applyWaitCount_ = 0;
    uint32_t applySuccessCount_ = 0;
    int64_t lastApplyEntryQpc_ = 0;
    int64_t applyInterFrameSum_ = 0;
    uint32_t applyInterFrameCount_ = 0;
    int applyTraceCount_ = 0;
    uint32_t applyDedupCount_ = 0;
    int traceLogCount_ = 0;
    mutable std::mutex eventStateMutex_;
    mutable std::mutex timerStateMutex_;
    static inline std::atomic<int> s_TimerResolutionRefCount{0};
};

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
