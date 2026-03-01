#pragma once

// clang-format off
#include <windows.h>
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
// clang-format on
#include <intrin.h>
#include <atomic>
#include "hook_common.h"
#include "ipc_client.h"

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
                    return true;
                }
            }
        }

        // Fallback: Hybrid sleep strategy
        // - If > 2ms remaining, use Sleep(1) for power efficiency
        // - If 0.5ms - 2ms, use SwitchToThread() or Sleep(0)
        // - If < 0.5ms, spin-wait for precision
        while (diffUs > 0) {
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
        if (traceLogCount_ >= 30)
            return;
        traceLogCount_++;
        // Resolve absolute path from DLL location
        if (traceLogPath_[0] == '\0') {
            HMODULE hMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)this, &hMod);
            if (hMod) {
                GetModuleFileNameA(hMod, traceLogPath_, MAX_PATH);
                char* lastSlash = strrchr(traceLogPath_, '\\');
                if (lastSlash)
                    *lastSlash = '\0';
                strncat(traceLogPath_, "\\logs\\fps_limiter_trace.log", MAX_PATH - strlen(traceLogPath_) - 1);
            }
        }
        if (traceLogPath_[0] == '\0')
            return;
        char buf[512];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
        va_end(args);
        if (len <= 0)
            return;
        buf[len] = '\n';
        HANDLE hFile = CreateFileA(traceLogPath_, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, buf, (DWORD)(len + 1), &written, nullptr);
            CloseHandle(hFile);
        }
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
        const int64_t kDedupTicks = qpcFrequency / 500;  // 2ms
        if (lastApplyReturnQpc != 0 && (nowQpc.QuadPart - lastApplyReturnQpc) < kDedupTicks) {
            applyDedupCount_++;
            return;
        }

        bool isRecording = shm->runtimeState.isRecording;
        bool captureSyncEnabled = shm->fpsLimiter.GetCaptureSyncEnabled();
        int captureSyncMultiplier = shm->fpsLimiter.GetCaptureSyncMultiplier();
        bool generalEnabled = shm->fpsLimiter.GetGeneralEnabled();
        int generalFps = shm->fpsLimiter.GetGeneralFps();
        int captureFps = shm->fpsLimiter.GetCaptureFps();
        bool useVFR = shm->fpsLimiter.GetUseVFR();

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

        // Check if limiter should be active
        bool limiterActive = false;
        int targetFps = 0;
        bool usingCaptureSync = false;

        if (isRecording && captureSyncEnabled) {
            if (captureFps > 0 && captureSyncMultiplier >= 1 && captureSyncMultiplier <= 8) {
                limiterActive = true;
                targetFps = captureFps * captureSyncMultiplier;
                usingCaptureSync = true;
            }
        } else if (generalEnabled && generalFps > 0) {
            limiterActive = true;
            targetFps = generalFps;
        }

        // VFR Mode Passthrough: Disable limiter if VFR is active
        if (useVFR) {
            limiterActive = false;
        }

        if (!limiterActive) {
            // Release timer resolution if we had it set
            if (timerResolutionSet) {
                timeEndPeriod(1);
                timerResolutionSet = false;
            }
            // Reset local limiter state when inactive
            if (localTargetTime_ != 0) {
                localTargetTime_ = 0;
                localFrameCount_ = 0;
            }
            if (!loggedInactive_) {
                TraceLog("Apply: INACTIVE rec=%d capSync=%d genEn=%d genFps=%d capFps=%d vfr=%d",
                         isRecording ? 1 : 0, captureSyncEnabled ? 1 : 0,
                         generalEnabled ? 1 : 0, generalFps, captureFps, useVFR ? 1 : 0);
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
        if (!loggedActive_ || lastTargetFps_ != targetFps || lastUsedCaptureSync_ != usingCaptureSync) {
            TraceLog("Apply: ACTIVE mode=%s target=%d capFps=%d mult=%d rec=%d",
                     usingCaptureSync ? "capture_sync" : "general", targetFps, captureFps,
                     captureSyncMultiplier, isRecording ? 1 : 0);
            HookLog(
                "FPS Limiter: Active (mode=%s, target=%d, captureFps=%d, mult=%d, general=%d/%d, isRecording=%d)",
                usingCaptureSync ? "capture_sync" : "general", targetFps, captureFps, captureSyncMultiplier,
                generalEnabled ? 1 : 0, generalFps, isRecording ? 1 : 0);
            loggedActive_ = true;
            lastTargetFps_ = targetFps;
            lastUsedCaptureSync_ = usingCaptureSync;
        }

        // Ensure 1ms timer resolution when limiter is active
        EnsureTimerResolution();

        if (targetFps <= 0)
            targetFps = 60;

        // LOCAL FPS LIMITING for capture sync mode.
        // Bypasses the cross-process event round-trip (hook→limiter→hook) which
        // adds ~3-4ms latency per frame and drops FPS well below target.
        // Instead, maintain a local cadence using SmartWait for sub-ms precision.
        if (usingCaptureSync) {
            int64_t intervalTicks = qpcFrequency / targetFps;

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            if (localTargetTime_ == 0) {
                // First frame: start cadence one interval from now
                localTargetTime_ = now.QuadPart + intervalTicks;
                localFrameCount_ = 0;
            }

            // Wait until the target time
            int64_t waitTicks = localTargetTime_ - now.QuadPart;
            int64_t waitUs = (waitTicks > 0) ? (waitTicks * 1000000 / qpcFrequency) : 0;
            SmartWait(localTargetTime_);

            // Advance target by fixed interval (preserves absolute cadence)
            localTargetTime_ += intervalTicks;

            // If we fell more than 2 frames behind, resync to avoid burst catch-up
            QueryPerformanceCounter(&now);
            if (localTargetTime_ < now.QuadPart - intervalTicks * 2) {
                localTargetTime_ = now.QuadPart + intervalTicks;
            }

            // Periodic stats logging
            localFrameCount_++;
            if (localFrameCount_ % 120 == 0) {
                // Measure actual inter-frame interval
                if (lastApplyEntryQpc_ != 0) {
                    int64_t interFrameUs = ((now.QuadPart - lastApplyEntryQpc_) * 1000000) / qpcFrequency;
                    double measuredFps = (interFrameUs > 0) ? (1000000.0 / interFrameUs) : 0;
                    TraceLog("Apply: LOCAL stats frames=%u waitUs=%lld measFps=%.1f target=%d",
                             localFrameCount_, waitUs, measuredFps, targetFps);
                    HookLog("FPS Limiter: Local capture sync (%u frames): lastWait=%lldus measFps=%.1f target=%d",
                            localFrameCount_, waitUs, measuredFps, targetFps);
                }
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
                TraceLog("Apply: Events OK release=%p request=%p target=%d",
                         releaseEvent, requestEvent, targetFps);
                HookLog(
                    "FPS Limiter: Events Initialized (target: %d FPS, release=%p, "
                    "request=%p)",
                    targetFps, releaseEvent, requestEvent);
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
                DWORD frameTimeMs = 1000 / targetFps;
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
        lastApplyReturnQpc = 0;
        localTargetTime_ = 0;
        localFrameCount_ = 0;
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
    int64_t lastApplyReturnQpc = 0;  // QPC tick when Apply() last returned from wait (dedup guard)
    int64_t localTargetTime_ = 0;    // QPC target for local capture sync cadence
    uint32_t localFrameCount_ = 0;   // Frame count for local capture sync stats
    uint32_t applyWaitCount_ = 0;
    uint32_t applySuccessCount_ = 0;
    int64_t lastApplyEntryQpc_ = 0;
    int64_t applyInterFrameSum_ = 0;
    uint32_t applyInterFrameCount_ = 0;
    int applyTraceCount_ = 0;
    uint32_t applyDedupCount_ = 0;
    int traceLogCount_ = 0;
    char traceLogPath_[MAX_PATH] = {0};
};

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
