#pragma once

// clang-format off
#include <windows.h>
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
// clang-format on
#include <intrin.h>
#include <atomic>
#include "antilag2_limiter.h"
#include "fg_detection.h"
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

    // Ensure 1ms timer resolution is enabled
    void EnsureTimerResolution() {
        if (!timerResolutionSet) {
            timeBeginPeriod(1);
            timerResolutionSet = true;
        }
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
            if (!highResTimer) {
                // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2
                highResTimer = CreateWaitableTimerExW(NULL, NULL, 0x2, TIMER_ALL_ACCESS);
                if (!highResTimer) {
                    highResTimerFailed = true;  // Fall back to polling
                }
            }

            if (highResTimer) {
                // Snapshot handle to a local to prevent TOCTOU race with Shutdown() closing it
                // between the null check and SetWaitableTimer/WaitForSingleObject.
                HANDLE localTimer = highResTimer;
                // Convert to 100ns intervals (negative = relative)
                // Use double to avoid int64 overflow when diff * 10000000 > INT64_MAX.
                // At qpcFrequency ~10MHz and diff up to ~200ms, diff*10M can exceed 2^53 in double
                // but stays well within int64 range for reasonable frame times (<1s).
                // The divide-before-multiply approach prevents overflow for large diffs.
                LARGE_INTEGER dueTime;
                dueTime.QuadPart =
                    -static_cast<int64_t>(static_cast<double>(diff) * (10000000.0 / static_cast<double>(qpcFrequency)));

                if (SetWaitableTimer(localTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(localTimer, (DWORD)(diffUs / 1000 + 5));
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
        static HANDLE s_TraceFile = INVALID_HANDLE_VALUE;
        static char s_TraceLogPath[MAX_PATH] = {0};

        std::unique_lock<std::mutex> lock(s_TraceLogMutex, std::defer_lock);
        if (!lock.try_lock())
            return;  // Drop trace if another thread is writing; never stall Present

        if (s_TraceLogPath[0] == '\0') {
            HMODULE hMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)this, &hMod);
            if (hMod) {
                GetModuleFileNameA(hMod, s_TraceLogPath, MAX_PATH);
                char* lastSlash = strrchr(s_TraceLogPath, '\\');
                if (lastSlash)
                    *lastSlash = '\0';
                strncat(s_TraceLogPath, "\\logs\\fps_limiter_trace.log", MAX_PATH - strlen(s_TraceLogPath) - 1);
            }
        }
        if (s_TraceLogPath[0] == '\0')
            return;

        if (s_TraceFile == INVALID_HANDLE_VALUE) {
            s_TraceFile = CreateFileA(s_TraceLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (s_TraceFile == INVALID_HANDLE_VALUE)
                return;
        }

        DWORD written = 0;
        WriteFile(s_TraceFile, line, static_cast<DWORD>(len), &written, nullptr);
    }

    // Called each frame before present
    void Apply() {
        SharedMemoryLayout* shm = nullptr;
        if (dbgShm) {
            shm = dbgShm;
        } else if (ipc) {
            shm = ipc->GetSharedMem();
        }

        if (!shm) {
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

        bool isRecording = shm->runtimeState.isRecording;
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

        // Lazily initialize Reflex hook (only once, on first Apply call)
        if (!reflexInitAttempted_) {
            reflexInitAttempted_ = true;
            g_ReflexLimiter.Init();
        }

        // Check if limiter should be active
        bool limiterActive = false;
        int targetFps = 0;
        bool usingCaptureSync = false;
        uint32_t configuredMode = LimiterModeValues::kAuto;

        if (isRecording && captureSyncEnabled) {
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
            lastActualWaitUs_ = 0;
            loggedNativeFallback_ = false;
            // Release timer resolution if we had it set
            if (timerResolutionSet) {
                timeEndPeriod(1);
                timerResolutionSet = false;
            }
            // Reset local limiter state when inactive
            if (localTargetTime_ != 0) {
                localTargetTime_ = 0;
                localFrameCount_ = 0;
                localStatsIntervalStart_ = 0;
                localStatsFrameCount_ = 0;
            }
            // Clear Reflex override when limiter is inactive
            if (reflexLimiterActive_) {
                g_ReflexLimiter.SetTargetFps(0);
                reflexLimiterActive_ = false;
            }
            if (!loggedInactive_) {
                TraceLog("Apply: INACTIVE rec=%d capSync=%d genEn=%d genFps=%d capFps=%d vfr=%d", isRecording ? 1 : 0,
                         captureSyncEnabled ? 1 : 0, generalEnabled ? 1 : 0, generalFps, captureFps, useVFR ? 1 : 0);
                HookLog(
                    "FPS Limiter: Inactive (general_enabled=%d, generalFps=%d, "
                    "captureSync=%d, isRecording=%d, useVFR=%d)",
                    generalEnabled ? 1 : 0, generalFps, captureSyncEnabled ? 1 : 0, isRecording ? 1 : 0,
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
            // Priority: Reflex (NVIDIA) → XeLL (Intel) → Anti-Lag 2 (AMD) → FG fallback → basic
            if (g_ReflexLimiter.IsAvailable()) {
                effectiveMode = LimiterModeValues::kNative;
            } else if (g_XeLLLimiter.IsAvailable()) {
                effectiveMode = LimiterModeValues::kXeLL;
            } else if (g_AntiLag2Limiter.IsAvailable()) {
                effectiveMode = LimiterModeValues::kAntiLag2;
            } else if (fgActive) {
                // No native low-latency API but FG active → FG-compatible fallback
                effectiveMode = LimiterModeValues::kFGFallback;
            } else {
                effectiveMode = LimiterModeValues::kBasic;
            }
        }

        // Validate: native modes require the respective DLL to be available.
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
        }

        // FG-aware FPS adjustment for FG fallback and all native low-latency modes:
        // When FG is active, the output frame rate is fgMultiplier × base rate.
        // To hit targetFps output, the base game needs to render at targetFps / fgMultiplier.
        int effectiveTargetFps = targetFps;
        bool isNativeMode =
            (effectiveMode == LimiterModeValues::kNative || effectiveMode == LimiterModeValues::kAntiLag2 ||
             effectiveMode == LimiterModeValues::kXeLL);
        if (fgActive && (effectiveMode == LimiterModeValues::kFGFallback || isNativeMode)) {
            effectiveTargetFps = targetFps / fgMultiplier;
            if (effectiveTargetFps < 1)
                effectiveTargetFps = 1;
        }

        // Log mode transitions
        if (!loggedActive_ || lastTargetFps_ != effectiveTargetFps || lastUsedCaptureSync_ != usingCaptureSync ||
            lastEffectiveMode_ != effectiveMode) {
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

            TraceLog("Apply: ACTIVE sync=%s limiter=%s target=%d effective=%d fg=%d fgMult=%d",
                     usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, fgActive ? 1 : 0,
                     fgMultiplier);
            HookLog("FPS Limiter: Active (sync=%s, limiter=%s, target=%d, effective=%d, fg=%d/%dx, isRec=%d)",
                    usingCaptureSync ? "capture" : "general", modeStr, targetFps, effectiveTargetFps, fgActive ? 1 : 0,
                    fgMultiplier, isRecording ? 1 : 0);
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

            // Try to push our limit through the Reflex pipeline
            if (g_ReflexLimiter.PushFpsLimit()) {
                reflexLimiterActive_ = true;
                loggedNativeFallback_ = false;

                // Driver handles frame pacing — we don't SmartWait
                isActivelyLimiting_.store(false, std::memory_order_relaxed);
                lastActualWaitUs_ = 0;

                LARGE_INTEGER retQpc;
                QueryPerformanceCounter(&retQpc);
                lastApplyReturnQpc = retQpc.QuadPart;
                return;
            }

            // Push failed — fall through to timer-based limiting
            if (reflexLimiterActive_) {
                HookLog("FPS Limiter: Reflex push failed, falling back to timer");
            }
            if (!loggedNativeFallback_) {
                HookLog("FPS Limiter: Reflex native mode unavailable at runtime; using timer fallback");
                loggedNativeFallback_ = true;
            }
        }

        // Clear Reflex override if we were using it but switched away
        if (reflexLimiterActive_) {
            g_ReflexLimiter.SetTargetFps(0);
            reflexLimiterActive_ = false;
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

        // LOCAL FPS LIMITING for capture sync mode.
        // Bypasses the cross-process event round-trip (hook→limiter→hook) which
        // adds ~3-4ms latency per frame and drops FPS well below target.
        // Instead, maintain a local cadence using SmartWait for sub-ms precision.
        if (usingCaptureSync) {
            int64_t intervalTicks = qpcFrequency / effectiveTargetFps;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            if (localTargetTime_ == 0) {
                // First frame: start cadence one interval from now
                localTargetTime_ = now.QuadPart + intervalTicks;
                localFrameCount_ = 0;
                localStatsIntervalStart_ = now.QuadPart;
                localStatsFrameCount_ = 0;
            }

            // Wait until the target time
            int64_t waitTicks = localTargetTime_ - now.QuadPart;
            int64_t waitUs = (waitTicks > 0) ? (waitTicks * 1000000 / qpcFrequency) : 0;
            LARGE_INTEGER beforeWait;
            QueryPerformanceCounter(&beforeWait);
            SmartWait(localTargetTime_);
            LARGE_INTEGER afterWait;
            QueryPerformanceCounter(&afterWait);
            lastActualWaitUs_ = ((afterWait.QuadPart - beforeWait.QuadPart) * 1000000) / qpcFrequency;

            // Advance target by fixed interval (preserves absolute cadence)
            localTargetTime_ += intervalTicks;

            // If we fell more than 2 frames behind, resync to avoid burst catch-up
            QueryPerformanceCounter(&now);
            if (localTargetTime_ < now.QuadPart - intervalTicks * 2) {
                localTargetTime_ = now.QuadPart + intervalTicks;
            }

            // Track stats
            localFrameCount_++;
            localStatsFrameCount_++;

            // Periodic stats logging (every 120 frames)
            if (localFrameCount_ % 120 == 0) {
                // Average FPS over the interval
                int64_t intervalUs = ((now.QuadPart - localStatsIntervalStart_) * 1000000) / qpcFrequency;
                double avgFps = (intervalUs > 0) ? (localStatsFrameCount_ * 1000000.0 / intervalUs) : 0;
                // Instantaneous FPS (last frame only)
                double instantFps = 0;
                if (lastApplyEntryQpc_ != 0) {
                    int64_t interFrameUs = ((now.QuadPart - lastApplyEntryQpc_) * 1000000) / qpcFrequency;
                    instantFps = (interFrameUs > 0) ? (1000000.0 / interFrameUs) : 0;
                }
                TraceLog("Apply: LOCAL stats frames=%u waitUs=%lld avgFps=%.1f instFps=%.1f target=%d",
                         localFrameCount_, waitUs, avgFps, instantFps, effectiveTargetFps);
                HookLog(
                    "FPS Limiter: Local capture sync (%u frames): lastWait=%lldus avgFps=%.1f "
                    "instFps=%.1f target=%d",
                    localFrameCount_, waitUs, avgFps, instantFps, effectiveTargetFps);
                localStatsIntervalStart_ = now.QuadPart;
                localStatsFrameCount_ = 0;
            }
            lastApplyEntryQpc_ = now.QuadPart;

            // Record return time for dedup guard
            QueryPerformanceCounter(&now);
            lastApplyReturnQpc = now.QuadPart;
            return;
        }

        // Initialize events if not done (general limiter mode only)
        // Only try to open events if we are not in test mode (implied by dbgShm
        // presence usually, but let's just check name)
        if (!eventsInitialized && shm->fpsLimiter.releaseEventName[0] != L'\0') {
            releaseEvent = OpenEventW(SYNCHRONIZE, FALSE, shm->fpsLimiter.releaseEventName);
            requestEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, shm->fpsLimiter.requestEventName);

            // Query QPC frequency for timing calculations
            if (qpcFrequency == 0) {
                LARGE_INTEGER freq;
                QueryPerformanceFrequency(&freq);
                qpcFrequency = freq.QuadPart;
            }

            // Only mark as initialized if we got valid events
            // If OpenEvent failed, we'll retry next frame (limiter might not be ready yet)
            if (releaseEvent && requestEvent) {
                eventsInitialized = true;
                TraceLog("Apply: Events OK release=%p request=%p target=%d", releaseEvent, requestEvent,
                         effectiveTargetFps);
                HookLog(
                    "FPS Limiter: Events Initialized (target: %d FPS, release=%p, "
                    "request=%p)",
                    effectiveTargetFps, releaseEvent, requestEvent);
            } else {
                HookLog(
                    "FPS Limiter: Failed to open events (release=%p, request=%p), "
                    "will retry. Names: %ls / %ls",
                    releaseEvent, requestEvent, shm->fpsLimiter.releaseEventName, shm->fpsLimiter.requestEventName);
                // Don't mark as initialized - we'll retry next frame
            }
        }

        // In test mode (dbgShm), we might assume events are initialized or not
        // needed for SmartWait test But for full Apply() test, we need them if we
        // want to hit the event path. If not, we fall through.

        // Request next frame timing
        uint32_t myRequest = shm->fpsLimiter.requestCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (requestEvent)
            SetEvent(requestEvent);

        // Wait for release from limiter process
        if (releaseEvent || dbgShm) {  // Allow test mode to use SmartWait if manual
                                       // targetTime is set
            // For real usage: check releaseEvent. For test: if dbgShm is set, we
            // might skip the WaitForSingleObject or mocking it. But typically tests
            // won't have the external limiter process running. So let's make the test
            // set targetTimeTicks manually, and we skp WaitForSingleObject if it's a
            // test? Better: In a test, we can CreateEvent ourselves.

            if (releaseEvent) {
                // Calculate appropriate timeout based on target frame interval
                // Use 3x frame interval for safety margin
                DWORD frameTimeMs = 1000 / effectiveTargetFps;
                DWORD timeoutMs = frameTimeMs * 3;
                if (timeoutMs < 10)
                    timeoutMs = 10;
                if (timeoutMs > 100)
                    timeoutMs = 100;

                DWORD waitResult = WaitForSingleObject(releaseEvent, timeoutMs);

                // Log wait statistics and measured FPS periodically
                applyWaitCount_++;
                if (waitResult == WAIT_OBJECT_0) {
                    applySuccessCount_++;
                }
                // Measure inter-frame interval (Apply-to-Apply time = true game FPS)
                if (lastApplyEntryQpc_ != 0) {
                    int64_t interFrameUs = ((nowQpc.QuadPart - lastApplyEntryQpc_) * 1000000) / qpcFrequency;
                    applyInterFrameSum_ += interFrameUs;
                    applyInterFrameCount_++;
                }
                lastApplyEntryQpc_ = nowQpc.QuadPart;

                if (applyWaitCount_ % 120 == 0) {
                    LARGE_INTEGER afterQpc;
                    QueryPerformanceCounter(&afterQpc);
                    int64_t waitUs = ((afterQpc.QuadPart - nowQpc.QuadPart) * 1000000) / qpcFrequency;
                    double measuredFps = 0;
                    if (applyInterFrameCount_ > 0) {
                        double avgInterUs = (double)applyInterFrameSum_ / applyInterFrameCount_;
                        measuredFps = 1000000.0 / avgInterUs;
                    }
                    TraceLog("Apply: Stats calls=%u ok=%u timeout=%u measFps=%.1f waitUs=%lld dedup=%u",
                             applyWaitCount_, applySuccessCount_, applyWaitCount_ - applySuccessCount_, measuredFps,
                             waitUs, applyDedupCount_);
                    HookLog(
                        "FPS Limiter: Stats (%u calls): success=%u timeout=%u "
                        "measuredFps=%.1f lastWait=%lldus",
                        applyWaitCount_, applySuccessCount_, applyWaitCount_ - applySuccessCount_, measuredFps, waitUs);
                    applyInterFrameSum_ = 0;
                    applyInterFrameCount_ = 0;
                }

                if (waitResult == WAIT_TIMEOUT) {
                    missedFrames++;
                    if (timeoutLogCount_++ < 10) {
                        TraceLog("Apply: TIMEOUT missed=%u", missedFrames);
                        HookLog("FPS Limiter: TIMEOUT waiting for release (missed=%u)", missedFrames);
                    }
                    return;
                }
            } else {
                if (!loggedNoEvent_) {
                    HookLog("FPS Limiter: No releaseEvent available!");
                    loggedNoEvent_ = true;
                }
            }

            int64_t target = shm->fpsLimiter.targetTimeTicks.load(std::memory_order_acquire);
            if (target > 0) {
                SmartWait(target);
            } else {
                if (targetLogCount_++ < 10) {
                    TraceLog("Apply: targetTimeTicks=%lld (not waiting)", target);
                    HookLog("FPS Limiter: targetTimeTicks is %lld (not waiting)", target);
                }
            }
            // Record time Apply() returned so sequential duplicate presents
            // (e.g. DXVK Present+PresentEx) are deduped on the next call.
            QueryPerformanceCounter(&nowQpc);
            lastApplyReturnQpc = nowQpc.QuadPart;
        } else {
            // Fallback: spin wait on release count (no event available)
            DWORD start = GetTickCount();
            while (shm->fpsLimiter.releaseCount.load(std::memory_order_acquire) < myRequest) {
                if (GetTickCount() - start > 100) {
                    missedFrames++;
                    break;
                }
                SwitchToThread();  // Yield instead of Sleep(0) for better responsiveness
            }
        }
    }

    void Shutdown() {
        if (releaseEvent) {
            CloseHandle(releaseEvent);
            releaseEvent = NULL;
        }
        if (requestEvent) {
            CloseHandle(requestEvent);
            requestEvent = NULL;
        }
        if (highResTimer) {
            CloseHandle(highResTimer);
            highResTimer = NULL;
        }
        if (timerResolutionSet) {
            timeEndPeriod(1);
            timerResolutionSet = false;
        }
        eventsInitialized = false;
        sessionIdPublished = false;
        highResTimerFailed = false;
        loggedInactive_ = false;
        loggedNoEvent_ = false;
        loggedActive_ = false;
        missedFrames = 0;
        // CRITICAL FIX: Reset per-instance log counters on shutdown
        timeoutLogCount_ = 0;
        targetLogCount_ = 0;
        lastTargetFps_ = 0;
        lastUsedCaptureSync_ = false;
        lastEffectiveMode_ = LimiterModeValues::kAuto;
        lastApplyReturnQpc = 0;
        isActivelyLimiting_.store(false, std::memory_order_relaxed);
        localTargetTime_ = 0;
        localFrameCount_ = 0;
        localStatsIntervalStart_ = 0;
        localStatsFrameCount_ = 0;
        reflexInitAttempted_ = false;
        reflexDeviceProvided_ = false;
        if (reflexLimiterActive_) {
            g_ReflexLimiter.SetTargetFps(0);
            reflexLimiterActive_ = false;
        }
        g_ReflexLimiter.Shutdown();
        antilag2InitAttempted_ = false;
        g_AntiLag2Limiter.Shutdown();
        xellInitAttempted_ = false;
        g_XeLLLimiter.Shutdown();
    }

private:
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
    int lastTargetFps_ = 0;
    bool lastUsedCaptureSync_ = false;
    uint32_t lastEffectiveMode_ = LimiterModeValues::kAuto;  // Track mode changes for logging
    bool reflexInitAttempted_ = false;                       // Lazy init flag for Reflex hook
    bool reflexLimiterActive_ = false;                       // True when Reflex is handling pacing
    bool reflexDeviceProvided_ = false;                      // True once we've given device to ReflexLimiter
    bool loggedNativeFallback_ = false;                      // Avoid spam when native mode falls back to timer
    bool antilag2InitAttempted_ = false;                     // Lazy init flag for Anti-Lag 2
    bool xellInitAttempted_ = false;                         // Lazy init flag for XeLL
    int64_t lastApplyReturnQpc = 0;                // QPC tick when Apply() last returned from wait (dedup guard)
    int64_t localTargetTime_ = 0;                  // QPC target for local capture sync cadence
    uint32_t localFrameCount_ = 0;                 // Frame count for local capture sync stats
    int64_t localStatsIntervalStart_ = 0;          // QPC start of current stats interval
    uint32_t localStatsFrameCount_ = 0;            // Frame count within current stats interval
    int64_t lastActualWaitUs_ = 0;                 // Last Apply() actual wait time in μs
    std::atomic<bool> isActivelyLimiting_{false};  // True when limiter is actively pacing frames
    uint32_t applyWaitCount_ = 0;
    uint32_t applySuccessCount_ = 0;
    int64_t lastApplyEntryQpc_ = 0;
    int64_t applyInterFrameSum_ = 0;
    uint32_t applyInterFrameCount_ = 0;
    int applyTraceCount_ = 0;
    uint32_t applyDedupCount_ = 0;
    int traceLogCount_ = 0;
};

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
