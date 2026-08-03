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
#include "../common/thread_power_throttling_compat.h"

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

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#endif

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

static std::atomic<bool> g_Running{true};

BOOL WINAPI LimiterConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT ||
        ctrlType == CTRL_LOGOFF_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
        g_Running = false;
        return TRUE;
    }
    return FALSE;
}

// Limiter state
static LARGE_INTEGER g_QpcFreq = {};
static LARGE_INTEGER g_TargetTime = {};
static int64_t g_FrameCount = 0;
// Legacy ramp-up support. In practice we re-arm fallback pacing immediately
// after stalls so brief Reflex transition windows do not run uncapped.
constexpr int64_t RAMP_UP_FRAMES = 10;
static int64_t g_LastIntervalTicks = 0;

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
    int64_t phaseOffsetTicks = intervalTicks / 2;
    if (phaseOffsetTicks < 1) {
        phaseOffsetTicks = 1;
    }

    // Initialize target time on first frame
    if (g_TargetTime.QuadPart == 0) {
        g_TargetTime.QuadPart = now.QuadPart + phaseOffsetTicks;
        g_FrameCount = RAMP_UP_FRAMES;
        LogInfo("[Limiter] Activated (Interval: %lld ticks)", intervalTicks);
    } else if ((now.QuadPart - g_TargetTime.QuadPart) > intervalTicks * 2) {
        // After a long load/menu transition gap, re-arm into the current frame
        // instead of releasing immediately and letting a short uncapped burst through.
        g_TargetTime.QuadPart = now.QuadPart + phaseOffsetTicks;
        g_FrameCount = RAMP_UP_FRAMES;
    }

    // Calculate wait time
    int64_t ticksUntilTarget = g_TargetTime.QuadPart - now.QuadPart;

    // Ramp-up: Gradually increase limiter strength for smooth entry
    if (g_FrameCount < RAMP_UP_FRAMES && ticksUntilTarget > 0) {
        double rampFactor = (double)g_FrameCount / (double)RAMP_UP_FRAMES;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
        g_TargetTime.QuadPart = now.QuadPart + phaseOffsetTicks;
        g_FrameCount = RAMP_UP_FRAMES;
    }
}

int LimiterProcessMain(const AppConfig& config) {
    Log_SetLevel(config.logLevel);
    SetConsoleCtrlHandler(LimiterConsoleHandler, TRUE);

    // Set realtime priority for minimal jitter
    SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Opt out of EcoQoS: ensure this thread runs on P-cores, not E-cores
    THREAD_POWER_THROTTLING_STATE tpts = {};
    tpts.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    tpts.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    tpts.StateMask = 0;  // 0 = disable throttling (prefer performance core)
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tpts, sizeof(tpts));

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

        if (ValidateDiscoveryInfo(pDiscovery) && pDiscovery->GetInjectPid() != 0) {
            wchar_t sharedMemName[64];
            GenerateSharedMemName(sharedMemName, 64, pDiscovery->GetInjectPid());

            hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
            if (hMapFile) {
                shm =
                    (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryLayout));

                if (shm && ValidateSharedMemory(shm) && shm->GetHostPID() != 0) {
                    LogInfo("[Limiter] Connected via discovery (inject PID: %u, ABI: 0x%08X)",
                            pDiscovery->GetInjectPid(), SHARED_MEMORY_ABI_SIGNATURE);
                } else {
                    if (shm) {
                        LogError("[Limiter] Rejected incompatible shared memory ABI (version=%u size=%u abi=0x%08X)",
                                 shm->GetVersion(), shm->structSize.load(std::memory_order_acquire),
                                 shm->abiSignature.load(std::memory_order_acquire));
                    }
                    if (shm) {
                        UnmapViewOfFile(shm);
                        shm = nullptr;
                    }
                    CloseHandle(hMapFile);
                    hMapFile = NULL;
                }
            }
        }
        if (pDiscovery)
            UnmapViewOfFile(pDiscovery);
        CloseHandle(hDiscovery);
    }

    LogInfo("[Limiter] Process started (PID: %lu)", GetCurrentProcessId());

    // Use event names already created by the inject process (published in shared memory).
    // The inject process creates these events and publishes their names at CE_LR_*/CE_LQ_*.
    // We must NOT create our own events with different names, as the hook polls the names
    // from shared memory and would open the wrong events.
    wchar_t releaseName[64] = L"";
    wchar_t requestName[64] = L"";
    HANDLE hReleaseEvent = NULL;
    HANDLE hRequestEvent = NULL;

    // Wait for SHM if not found initially (retry with discovery)
    while (!shm && g_Running) {
        HANDLE hDisc = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_DISCOVERY);
        if (hDisc) {
            DiscoveryInfo* pDisc = (DiscoveryInfo*)MapViewOfFile(hDisc, FILE_MAP_READ, 0, 0, sizeof(DiscoveryInfo));

            if (ValidateDiscoveryInfo(pDisc) && pDisc->GetInjectPid() != 0) {
                wchar_t sharedMemName[64];
                GenerateSharedMemName(sharedMemName, 64, pDisc->GetInjectPid());

                hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sharedMemName);
                if (hMapFile) {
                    shm = (SharedMemoryLayout*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             sizeof(SharedMemoryLayout));
                    if (shm && (!ValidateSharedMemory(shm) || shm->GetHostPID() == 0)) {
                        LogError("[Limiter] Retry rejected invalid or incompatible shared memory");
                        UnmapViewOfFile(shm);
                        shm = nullptr;
                        CloseHandle(hMapFile);
                        hMapFile = NULL;
                    }
                }
            }
            if (pDisc)
                UnmapViewOfFile(pDisc);
            CloseHandle(hDisc);
        }
        if (!shm) {
            Sleep(100);
        }
    }

    if (shm) {
        // Open the events already created by the inject process
        if (shm->fpsLimiter.releaseEventName[0] != L'\0') {
            wcscpy_s(releaseName, 64, shm->fpsLimiter.releaseEventName);
            hReleaseEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, releaseName);
        }
        if (shm->fpsLimiter.requestEventName[0] != L'\0') {
            wcscpy_s(requestName, 64, shm->fpsLimiter.requestEventName);
            hRequestEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, requestName);
        }
        if (!hReleaseEvent || !hRequestEvent) {
            LogError("[Limiter] Failed to open inject-created events (inject may not have created them yet)");
            // Don't overwrite the names - the inject process owns them
        } else {
            LogInfo("[Limiter] Opened inject-created limiter events");
        }
    }

    // Main loop: Handle requests from hook via shared memory
    uint32_t lastReleasedCount = 0;
    // Initialize with current session ID if available, or 0
    uint32_t lastSessionId = 0;
    if (shm)
        lastSessionId = shm->fpsLimiter.hookSessionId.load(std::memory_order_relaxed);

    LogInfo("[Limiter] Entering main loop");
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

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
        if (ipc.HasFatalDisconnect()) {
            LogWarn("[Limiter] Controller IPC disconnected; exiting for a clean respawn");
            g_Running = false;
            break;
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
    if (hRequestEvent)
        CloseHandle(hRequestEvent);
    if (shm)
        UnmapViewOfFile(shm);
    if (hMapFile)
        CloseHandle(hMapFile);
    timeEndPeriod(1);

    return 0;
}
