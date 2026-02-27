// clang-format off
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
// clang-format on
#include <intrin.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "../common/config.h"
#include "../common/logging.h"
#include "../common/process_ipc.h"
#include "../common/shared_defs.h"

/*
 * FPS LIMITER - HIGH-RESOLUTION TIMER BEHAVIOR
 * =============================================
 *
 * This FPS limiter uses Windows high-resolution timers to achieve precise frame
 * pacing.
 *
 * KEY BEHAVIORS:
 *
 * 1. timeBeginPeriod(1):
 *    - Sets system timer resolution to 1ms (default is 15.6ms)
 *    - Affects ALL processes system-wide (not just this one)
 *    - Increases power consumption slightly
 *    - MUST be paired with timeEndPeriod(1) on shutdown
 *    - Required for Sleep() to be accurate below 15ms
 *
 * 2. Sleep() Accuracy:
 *    - With timeBeginPeriod(1): Sleep(N) sleeps for N±1ms
 *    - Without it: Sleep(N) can sleep for 15-20ms even if N=1
 *    - Still not sub-millisecond accurate (use spin-wait for that)
 *
 * 3. Current Implementation:
 *    - Uses Sleep() for bulk of wait time
 *    - Spin-waits for final <1ms to hit exact target
 *    - QueryPerformanceCounter() for high-precision timing
 *
 * 4. Alternative: NtDelayExecution
 *    - Undocumented NT kernel function
 *    - Can achieve sub-ms precision without spin-wait
 *    - More complex to use (requires LARGE_INTEGER time units)
 *    - Not currently implemented (Sleep + spin is sufficient)
 *
 * 5. Frame Pacing Strategy:
 *    - Target frame time = 1000ms / target_fps
 *    - Sleep for (target_time - 2ms) to avoid oversleep
 *    - Spin-wait for remaining time using QPC
 *    - Accounts for previous frame's timing error
 *
 * PERFORMANCE IMPACT:
 * - CPU usage: ~0.1% idle, <1% during active limiting
 * - Latency: Sub-millisecond precision achieved
 * - Power: Slight increase due to timeBeginPeriod(1)
 */

#pragma comment(lib, "avrt.lib")

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

static std::atomic<bool> g_Running{true};
static std::atomic<bool> g_LimiterActive{true};

// Limiter state
static LARGE_INTEGER g_QpcFreq = {{0}};
static LARGE_INTEGER g_TargetTime = {{0}};
static int64_t g_FrameCount = 0;
// Ramp-up for smooth activation (Reduced for faster lock)
constexpr int64_t RAMP_UP_FRAMES = 10;
static int64_t g_LastIntervalTicks = 0;
// Variance window stats (last 120 frames = ~1 sec at 120fps)
static constexpr int VARIANCE_WINDOW = 120;

void ApplyFramePacing(SharedMemoryLayout* shm) {
    if (!shm)
        return;

    // Pin limiter thread to a single core to minimize scheduling jitter
    // Core 1 is usually a safe bet for low-latency background tasks
    static bool affinitySet = false;
    if (!affinitySet) {
        SetThreadAffinityMask(GetCurrentThread(), 0x02);  // Core 1
        affinitySet = true;
    }

    bool isRecording = shm->runtimeState.isRecording;

    // Determine active limiter and target interval in QPC ticks for maximum
    // precision
    bool limiterActive = false;
    int64_t intervalTicks = 0;

    if (isRecording && shm->fpsLimiter.GetCaptureSyncEnabled()) {
        int captureFps = shm->fpsLimiter.GetCaptureFps();
        int multiplier = shm->fpsLimiter.GetCaptureSyncMultiplier();
        if (captureFps > 0 && multiplier >= 1 && multiplier <= 8) {
            intervalTicks = g_QpcFreq.QuadPart / (captureFps * multiplier);
            limiterActive = true;
            static bool loggedOnce = false;
            if (!loggedOnce) {
                LogInfo(
                    "[Limiter] Capture sync: captureFps=%d, mult=%d, targetFps=%d, "
                    "intervalTicks=%lld",
                    captureFps, multiplier, captureFps * multiplier, intervalTicks);
                loggedOnce = true;
            }
        }
    } else if (shm->fpsLimiter.GetGeneralEnabled()) {
        int targetFps = shm->fpsLimiter.GetGeneralFps();
        if (targetFps > 0) {
            intervalTicks = g_QpcFreq.QuadPart / targetFps;
            limiterActive = true;
        }
    }

    if (!limiterActive) {
        // Limiter inactive - immediately release
        g_TargetTime.QuadPart = 0;
        g_FrameCount = 0;
        g_LastIntervalTicks = 0;
        return;
    }

    // Reset timing if cadence changed significantly (e.g. toggled recording)
    if (g_LastIntervalTicks > 0 && intervalTicks != g_LastIntervalTicks) {
        LogInfo("[Limiter] Rate change: %lld -> %lld ticks (%.2f fps). Resetting.", g_LastIntervalTicks, intervalTicks,
                (double)g_QpcFreq.QuadPart / (double)intervalTicks);
        g_TargetTime.QuadPart = 0;
    }
    g_LastIntervalTicks = intervalTicks;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Initialize target time on first frame
    if (g_TargetTime.QuadPart == 0) {
        g_TargetTime = now;
        g_FrameCount = 0;
        LogInfo("[Limiter] Activated (Interval: %lld ticks)", intervalTicks);
    }

    // Calculate wait time
    int64_t ticksUntilTarget = g_TargetTime.QuadPart - now.QuadPart;

    // Ramp-up: Gradually increase limiter strength for smooth entry
    if (g_FrameCount < RAMP_UP_FRAMES && ticksUntilTarget > 0) {
        double rampFactor = (double)g_FrameCount / (double)RAMP_UP_FRAMES;
        ticksUntilTarget = (int64_t)(ticksUntilTarget * rampFactor);
    }

    // Wait if needed
    if (ticksUntilTarget > 0) {
        // Convert to microseconds for Sleep threshold
        int64_t usUntilTarget = (ticksUntilTarget * 1000000) / g_QpcFreq.QuadPart;

        // Fixed timing values
        constexpr int32_t sleepMarginUs = 1500;  // Wake 1.5ms early for spin

        // Sleep for bulk of wait (hybrid sleep+spin approach)
        if (usUntilTarget > sleepMarginUs) {
            int64_t sleepMs = (usUntilTarget - sleepMarginUs) / 1000;
            if (sleepMs > 0) {
                Sleep((DWORD)sleepMs);
            }
        }

        // Spin until EXACT target time (no early release)
        // This ensures hook wakes at precisely the right time
        // Safety: max iterations to prevent infinite spin on QPC anomalies
        constexpr int32_t maxSpinIterations = 10000000;  // ~10ms at 1ns/iteration
        int32_t spinCount = 0;
        while (true) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= g_TargetTime.QuadPart)
                break;
            if (++spinCount >= maxSpinIterations) {
                // Bail out to prevent infinite spin - something is wrong
                break;
            }
            _mm_pause();  // CPU hint for spin-wait
        }
    }

    // Publish EXACT target to shared memory for hook-side fine trim
    shm->fpsLimiter.targetTimeTicks.store(g_TargetTime.QuadPart, std::memory_order_release);

    // Advance target by fixed interval (preserves absolute cadence, avoids drift)
    g_TargetTime.QuadPart += intervalTicks;
    g_FrameCount++;

    // Reset if we fell behind significantly (avoids huge catch-up bursts)
    // Fixed 2-frame tolerance: allows minor hitches while preventing runaway lag
    QueryPerformanceCounter(&now);
    int64_t lagTicks = now.QuadPart - g_TargetTime.QuadPart;
    if (lagTicks > intervalTicks * 2) {
        g_TargetTime = now;
    }
}

int LimiterProcessMain(const AppConfig& config) {
    // Set realtime priority for minimal jitter
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Set thread affinity to Core 1 (Core 0 is usually busy with OS, Core 2/3 for
    // Capture)
    SetThreadAffinityMask(GetCurrentThread(), 0x02);

    // MMCSS: Set thread characteristics for "Pro Audio" (highest priority)
    DWORD dummy = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &dummy);
    if (hTask)
        AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);

    LogInfo("Limiter Thread: Core 1, REALTIME_PRIORITY, MMCSS Pro Audio");

    // Enable 1ms timer resolution
    timeBeginPeriod(1);

    // Initialize QPC frequency
    QueryPerformanceFrequency(&g_QpcFreq);

    // Setup IPC server
    ProcessIPCServer ipc(ProcessMode::Limiter);
    if (!ipc.Init()) {
        LogError("[Limiter] Failed to initialize IPC");
        timeEndPeriod(1);
        return 1;
    }

    // Open shared memory (created by inject process)
    // Use discovery shared memory for O(1) lookup
    HANDLE hMapFile = NULL;
    SharedMemoryLayout* shm = nullptr;

    HANDLE hDiscovery = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
    if (hDiscovery) {
        DiscoveryInfo* pDiscovery =
            (DiscoveryInfo*)MapViewOfFile(hDiscovery, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

        if (pDiscovery && pDiscovery->magic == DISCOVERY_MAGIC && pDiscovery->injectPid.load() != 0) {
            wchar_t sharedMemName[64];
            GenerateSharedMemName(sharedMemName, 64, pDiscovery->injectPid.load());

            hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
            if (hMapFile) {
                shm =
                    (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));

                if (shm && shm->GetHostPID() != 0) {
                    LogInfo("[Limiter] Connected via discovery (inject PID: %u)", pDiscovery->injectPid.load());
                } else {
                    if (shm) {
                        UnmapViewOfFile(shm);
                        shm = nullptr;
                    }
                    CloseHandle(hMapFile);
                    hMapFile = NULL;
                }
            }
            UnmapViewOfFile(pDiscovery);
        }
        CloseHandle(hDiscovery);
    }

    LogInfo("[Limiter] Process started (PID: %d)", GetCurrentProcessId());

    // Create synchronization events
    wchar_t releaseName[64];
    wchar_t requestName[64];
    swprintf_s(releaseName, L"Local\\CaptureLimiterRelease_%d", GetCurrentProcessId());
    swprintf_s(requestName, L"Local\\CaptureLimiterRequest_%d", GetCurrentProcessId());

    HANDLE hReleaseEvent = CreateEventW(NULL, FALSE, FALSE, releaseName);  // Auto-reset
    HANDLE hRequestEvent = CreateEventW(NULL, FALSE, FALSE, requestName);  // Auto-reset

    if (!hReleaseEvent || !hRequestEvent) {
        LogError("[Limiter] Failed to create sync events");
        return 1;
    }

    // Wait for SHM if not found initially (retry with discovery)
    while (!shm && g_Running) {
        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (pDisc && pDisc->magic == DISCOVERY_MAGIC && pDisc->injectPid != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDisc->injectPid);

                hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (hMapFile) {
                    shm = (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             sizeof(SharedMemoryLayout));
                    if (shm && shm->GetHostPID() == 0) {
                        UnmapViewOfFile(shm);
                        shm = nullptr;
                        CloseHandle(hMapFile);
                        hMapFile = NULL;
                    }
                }
                UnmapViewOfFile(pDisc);
            }
            CloseHandle(hDisc);
        }
        if (!shm) {
            Sleep(100);
        }
    }

    if (shm) {
        wcscpy_s(shm->fpsLimiter.releaseEventName, releaseName);
        wcscpy_s(shm->fpsLimiter.requestEventName, requestName);
        LogInfo("[Limiter] Published event names");
    }

    // Main loop: Handle requests from hook via shared memory
    uint32_t lastReleasedCount = 0;
    // Initialize with current session ID if available, or 0
    uint32_t lastSessionId = 0;
    if (shm)
        lastSessionId = shm->fpsLimiter.hookSessionId.load(std::memory_order_relaxed);

    LogInfo("[Limiter] Entering main loop");

    while (g_Running) {
        // Check for IPC commands (non-blocking)
        ProcessCommand cmd;
        if (ipc.PollCommand(cmd)) {
            switch (cmd) {
                case ProcessCommand::Shutdown:
                    LogInfo("[Limiter] Shutdown command received");
                    g_Running = false;
                    ipc.SendResponse(ProcessResponse::Ack);
                    continue;  // Exit loop
                case ProcessCommand::Ping:
                    ipc.SendResponse(ProcessResponse::Pong);
                    break;
                case ProcessCommand::ReloadConfig:
                    g_TargetTime.QuadPart = 0;  // Reset timing
                    g_FrameCount = 0;
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
                default:
                    ipc.SendResponse(ProcessResponse::Ack);
                    break;
            }
        }

        if (!shm) {
            Sleep(100);
            continue;
        }

        // Check for Session ID change (Game Restart)
        uint32_t currentSessionId = shm->fpsLimiter.hookSessionId.load(std::memory_order_acquire);
        if (currentSessionId != lastSessionId) {
            LogInfo("[Limiter] Hook session changed (%u -> %u). Resetting state.", lastSessionId, currentSessionId);
            lastSessionId = currentSessionId;
            g_TargetTime.QuadPart = 0;
            g_FrameCount = 0;
            lastReleasedCount = 0;  // Hook resets shared counters on start
        }

        // Wait for hook request (Event-driven)
        // We wait for the event OR a timeout to poll IPC/Check validity
        DWORD waitResult = WaitForSingleObject(hRequestEvent, 100);  // 100ms timeout for IPC check

        if (waitResult == WAIT_OBJECT_0) {
            // Event signaled - Hook requested a frame

            // Check request count
            uint32_t requested = shm->fpsLimiter.requestCount.load(std::memory_order_acquire);

            if (requested > lastReleasedCount) {
                // Apply frame pacing - this limits FPS!
                ApplyFramePacing(shm);

                // Update release
                shm->fpsLimiter.releaseCount.fetch_add(1, std::memory_order_release);
                SetEvent(hReleaseEvent);
                lastReleasedCount++;
            }
        } else if (waitResult == WAIT_TIMEOUT) {
            // Timeout - just loop back to check IPC
        }
    }

    // Cleanup
    LogInfo("[Limiter] Shutting down");
    if (hReleaseEvent)
        CloseHandle(hReleaseEvent);
    if (shm)
        UnmapViewOfFile(shm);
    if (hMapFile)
        CloseHandle(hMapFile);
    timeEndPeriod(1);

    return 0;
}
