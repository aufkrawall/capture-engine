#pragma once

#include <windows.h>
#include <intrin.h>
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
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

    // Called each frame before present
    void Apply() {
        SharedMemoryLayout* shm = nullptr;
        if (dbgShm) {
            shm = dbgShm;
        } else if (ipc) {
            shm = ipc->GetSharedMem();
        }

        if (!shm)
            return;

        bool isRecording = shm->runtimeState.isRecording;

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

        if (isRecording && shm->fpsLimiter.GetCaptureSyncEnabled()) {
            int multiplier = shm->fpsLimiter.GetCaptureSyncMultiplier();
            int captureFps = shm->fpsLimiter.GetCaptureFps();
            if (captureFps > 0 && multiplier >= 1 && multiplier <= 8) {
                limiterActive = true;
                targetFps = captureFps * multiplier;
            }
        } else if (shm->fpsLimiter.GetGeneralEnabled() && shm->fpsLimiter.GetGeneralFps() > 0) {
            limiterActive = true;
            targetFps = shm->fpsLimiter.GetGeneralFps();
        }

        // VFR Mode Passthrough: Disable limiter if VFR is active
        if (shm->fpsLimiter.GetUseVFR()) {
            limiterActive = false;
        }

        if (!limiterActive) {
            // Release timer resolution if we had it set
            if (timerResolutionSet) {
                timeEndPeriod(1);
                timerResolutionSet = false;
            }
            if (!loggedInactive_) {
                HookLog(
                    "FPS Limiter: Inactive (general_enabled=%d, generalFps=%d, "
                    "captureSync=%d, isRecording=%d)",
                    shm->fpsLimiter.GetGeneralEnabled(), shm->fpsLimiter.GetGeneralFps(),
                    shm->fpsLimiter.GetCaptureSyncEnabled(), isRecording);
                loggedInactive_ = true;
            }
            return;
        }
        loggedInactive_ = false;  // Reset so transitions back to inactive are logged

        // Ensure 1ms timer resolution when limiter is active
        EnsureTimerResolution();

        if (targetFps <= 0)
            targetFps = 60;

        // Initialize events if not done
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

                if (waitResult == WAIT_TIMEOUT) {
                    // Limiter didn't respond in time - track but don't block
                    missedFrames++;
                    // CRITICAL FIX: Use per-instance counter instead of static
                    if (timeoutLogCount_++ < 10) {
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
                // CRITICAL FIX: Use per-instance counter instead of static
                if (targetLogCount_++ < 10) {
                    HookLog("FPS Limiter: targetTimeTicks is %lld (not waiting)", target);
                }
            }
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
        missedFrames = 0;
        // CRITICAL FIX: Reset per-instance log counters on shutdown
        timeoutLogCount_ = 0;
        targetLogCount_ = 0;
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
    int64_t qpcFrequency = 0;
    uint32_t missedFrames = 0;  // Track frames where limiter couldn't keep up
    // CRITICAL FIX: Per-instance log counters (was static, never reset)
    int timeoutLogCount_ = 0;
    int targetLogCount_ = 0;
};

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
