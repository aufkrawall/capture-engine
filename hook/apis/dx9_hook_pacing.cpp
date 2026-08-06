#include "dx9_hook_internal.h"


void EnsureDwmFlushLoaded() {


    if (dx9_hook_g_DwmFlush)
        return;
    HMODULE hDwm = GetModuleHandleA("dwmapi.dll");
    if (!hDwm)
        hDwm = ce::security::LoadSystemLibrary(L"dwmapi.dll");
    if (!hDwm)
        return;
    dx9_hook_g_DwmFlush = (DwmFlush_t)GetProcAddress(hDwm, "DwmFlush");

}


int64_t GetQpcFreqCached() {


    if (dx9_hook_g_QpcFreqCached == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        dx9_hook_g_QpcFreqCached = f.QuadPart;
    }
    return dx9_hook_g_QpcFreqCached;

}

HANDLE GetPaceTimerHandle() {


    if (dx9_hook_g_PaceTimer)
        return dx9_hook_g_PaceTimer;

    // Prefer high-resolution timers when available (Win10+).
    typedef HANDLE(WINAPI * CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    CreateWaitableTimerExW_t pCreateWaitableTimerExW =
        hKernel32 ? (CreateWaitableTimerExW_t)GetProcAddress(hKernel32, "CreateWaitableTimerExW") : nullptr;

    if (pCreateWaitableTimerExW) {
        dx9_hook_g_PaceTimer =
            pCreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (!dx9_hook_g_PaceTimer) {
        dx9_hook_g_PaceTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    return dx9_hook_g_PaceTimer;

}


void WaitUsHighRes(int64_t waitUs) {


    if (waitUs <= 0)
        return;
    HANDLE timer = GetPaceTimerHandle();
    if (!timer)
        return;

    LARGE_INTEGER due;
    due.QuadPart = -(waitUs * 10);  // relative in 100ns
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    }

}


int GetDesktopRefreshHzCached() {


    DWORD now = GetTickCount();
    if (dx9_hook_g_RefreshHzCached > 0 && (now - dx9_hook_g_RefreshHzLastTick) < 2000) {
        return dx9_hook_g_RefreshHzCached;
    }
    dx9_hook_g_RefreshHzLastTick = now;

    const int oldHz = dx9_hook_g_RefreshHzCached;
    int hz = 0;
    HDC hdc = GetDC(nullptr);
    if (hdc) {
        hz = GetDeviceCaps(hdc, VREFRESH);
        ReleaseDC(nullptr, hdc);
    }
    if (hz <= 1 || hz > 1000)
        hz = 60;
    dx9_hook_g_RefreshHzCached = hz;
    if (hz != oldHz) {
        HookLog("DX9: Desktop refresh reported as %d Hz", hz);
    }
    return hz;

}


void PaceToRefreshQpc() {


    const int hz = GetDesktopRefreshHzCached();
    const int64_t qpcFreq = GetQpcFreqCached();
    if (hz <= 0 || qpcFreq <= 0)
        return;

    const int64_t frameTicks = qpcFreq / (int64_t)hz;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (dx9_hook_g_LastPacedQpc == 0) {
        dx9_hook_g_LastPacedQpc = now.QuadPart;
        return;
    }

    // If we were stalled for a while (e.g. alt-tab), reset to avoid weird
    // catch-up behavior.
    if (now.QuadPart - dx9_hook_g_LastPacedQpc > frameTicks * 4) {
        dx9_hook_g_LastPacedQpc = now.QuadPart;
        return;
    }

    int64_t target = dx9_hook_g_LastPacedQpc + frameTicks;
    if (now.QuadPart < target) {
        // Safety timeout: max 50ms or 2x expected frame time to prevent infinite loops
        const int64_t maxWaitTicks = (qpcFreq * 50) / 1000;  // 50ms in QPC ticks
        const int64_t timeoutQpc = now.QuadPart + maxWaitTicks;
        int iterations = 0;
        const int kMaxIterations = 100000;  // Prevent infinite spinning

        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= target)
                break;
            // Safety checks: timeout or max iterations
            if (now.QuadPart >= timeoutQpc || iterations >= kMaxIterations) {
                static int timeoutLogCount = 0;
                if (timeoutLogCount < 5) {
                    HookLog("DX9: PaceToRefreshQpc timeout (iter=%d, waited=%lld us)", iterations,
                            (now.QuadPart - (target - frameTicks)) * 1000000 / qpcFreq);
                    timeoutLogCount++;
                }
                break;
            }
            iterations++;

            int64_t remainingTicks = target - now.QuadPart;
            int64_t remainingUs = (remainingTicks * 1000000) / qpcFreq;

            // Use high-res waitable timer for the bulk of the wait.
            // Keep a small spin/yield tail to hit the target accurately.
            if (remainingUs > 2000) {
                WaitUsHighRes(remainingUs - 1000);
            } else {
                YieldProcessor();
            }
        }
    }
    dx9_hook_g_LastPacedQpc = target;

}


DWORD WINAPI DwmFlushThreadProc(LPVOID param) {


    auto flushFunc = reinterpret_cast<DwmFlush_t>(param);
    if (flushFunc)
        flushFunc();
    return 0;

}


void MaybeWaitForVSyncAfterPresent(int64_t presentUs) {


    VSyncOverride vsync = GetVSyncOverride();
    if (!vsync.shouldOverride || vsync.presentInterval <= 0)
        return;
    // For legacy non-Ex DX9 staging capture, extra post-present pacing can
    // amplify already expensive readback cost. Favor minimal overhead while
    // recording.
    if (dx9_hook_g_DX9StagingCaptureActive.load(std::memory_order_acquire) && g_IPC && g_IPC->IsRecording()) {
        return;
    }
    // DXVK has its own frame pacing - skip our software pacing to avoid conflicts
    if (IsDXVKD3D9WrapperLoaded()) {
        return;
    }
    const int hz = GetDesktopRefreshHzCached();
    const bool windowed = dx9_hook_g_WindowedPresent;
    const UINT liveInterval = dx9_hook_g_LivePresentInterval.load(std::memory_order_acquire);
    const bool needsFullscreenFallback =
        !windowed && vsync.presentInterval > 0 && liveInterval != (UINT)vsync.presentInterval;
    const bool shouldPace = (windowed && (presentUs < 3000)) || needsFullscreenFallback;
    {
        static thread_local int lastHz = 0;
        static thread_local int lastShouldPace = -1;
        static thread_local UINT lastLiveInterval = 0;
        static thread_local int lastFallback = -1;
        static thread_local DWORD lastTick = 0;
        DWORD now = GetTickCount();
        if (hz != lastHz || (int)shouldPace != lastShouldPace || liveInterval != lastLiveInterval ||
            (int)needsFullscreenFallback != lastFallback || (now - lastTick) > 2000) {
            if (needsFullscreenFallback) {
                HookLogImportant(
                    "DX9: VSyncPace state: windowed=%d interval=%d "
                    "liveInterval=%u presentUs=%lld hz=%d pace=%d "
                    "fallback=%d",
                    windowed ? 1 : 0, vsync.presentInterval, liveInterval, (long long)presentUs, hz, shouldPace ? 1 : 0,
                    needsFullscreenFallback ? 1 : 0);
            } else {
                HookLog(
                    "DX9: VSyncPace state: windowed=%d interval=%d liveInterval=%u "
                    "presentUs=%lld hz=%d pace=%d fallback=%d",
                    windowed ? 1 : 0, vsync.presentInterval, liveInterval, (long long)presentUs, hz, shouldPace ? 1 : 0,
                    needsFullscreenFallback ? 1 : 0);
            }
            lastHz = hz;
            lastShouldPace = shouldPace ? 1 : 0;
            lastLiveInterval = liveInterval;
            lastFallback = needsFullscreenFallback ? 1 : 0;
            lastTick = now;
        }
    }

    if (!shouldPace)
        return;
    if (!windowed) {
        PaceToRefreshQpc();
        return;
    }

    const int64_t expectedUs = (hz > 0) ? (1000000LL / (int64_t)hz) : 0;

    // If DwmFlush ever starts blocking at an unexpected cadence (e.g. ~10ms ->
    // ~100Hz), we can't "undo" that wait after the fact. In that situation,
    // temporarily stop calling DwmFlush and use pure QPC pacing to the desktop
    // refresh instead.
    static DWORD s_DwmDisabledUntilTick = 0;
    static int s_DwmBadCadenceCount = 0;

    // Prefer DwmFlush when available. It blocks against DWM's compositor timing
    // and avoids double-pacing (which can create weird stable cadences like ~100
    // FPS).
    EnsureDwmFlushLoaded();
    const DWORD nowTick = GetTickCount();
    if (dx9_hook_g_DwmFlush && nowTick >= s_DwmDisabledUntilTick) {
        const int64_t qpcFreq = GetQpcFreqCached();
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);

        // DwmFlush can hang indefinitely with DXVK - use a timeout mechanism
        // Use a separate thread with a timeout to prevent indefinite blocking
        HANDLE hDwmThread =
            CreateThread(nullptr, 0, DwmFlushThreadProc, reinterpret_cast<LPVOID>(dx9_hook_g_DwmFlush), 0, nullptr);

        if (hDwmThread) {
            // Wait max 100ms for DwmFlush to complete
            DWORD waitResult = WaitForSingleObject(hDwmThread, 100);
            if (waitResult == WAIT_TIMEOUT) {
                // DwmFlush is hanging - terminate the thread and disable DwmFlush
                TerminateThread(hDwmThread, 1);
                static int dwmTimeoutLogCount = 0;
                if (dwmTimeoutLogCount < 5) {
                    HookLog("DX9: DwmFlush timed out after 100ms, disabling for 10s");
                    dwmTimeoutLogCount++;
                }
                s_DwmDisabledUntilTick = nowTick + 10000;  // Disable for 10s
            }
            CloseHandle(hDwmThread);
        } else {
            // Fallback: call directly (risky but no other option)
            dx9_hook_g_DwmFlush();
        }

        QueryPerformanceCounter(&t1);
        const int64_t dwmUs = (qpcFreq > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000) / qpcFreq : 0;

        // If DwmFlush blocks, only accept it if it matches the expected refresh
        // cadence. Some systems can report an unexpected compositor cadence (e.g.
        // ~100Hz) which would incorrectly cap FPS even when the desktop reports
        // 144Hz.
        bool acceptDwm = false;
        if (dwmUs > 3000 && expectedUs > 0) {
            // Tight tolerance: DwmFlush should be close to 1 / desktop_hz.
            // We intentionally reject ~10ms (100Hz) when desktop is 144Hz (~6.94ms).
            const int64_t lower = (expectedUs * 85) / 100;
            const int64_t upper = (expectedUs * 115) / 100;
            acceptDwm = (dwmUs >= lower && dwmUs <= upper);

            static DWORD lastDecisionLogTick = 0;
            static int lastAccept = -1;
            const DWORD nowTick = GetTickCount();
            if (lastAccept != (acceptDwm ? 1 : 0) || (nowTick - lastDecisionLogTick) > 2000) {
                lastDecisionLogTick = nowTick;
                lastAccept = acceptDwm ? 1 : 0;
                HookLog("DX9: DwmFlush pacing: dwmUs=%lld expectedUs=%lld hz=%d accept=%d", dwmUs, expectedUs, hz,
                        acceptDwm ? 1 : 0);
            }
        }

        if (acceptDwm) {
            s_DwmBadCadenceCount = 0;
            return;
        }

        // If DwmFlush blocked but at an unexpected cadence, disable it for a bit so
        // we don't keep paying that wrong wait every frame.
        if (dwmUs > 3000 && expectedUs > 0) {
            s_DwmBadCadenceCount++;
            if (s_DwmBadCadenceCount >= 3) {
                s_DwmBadCadenceCount = 0;
                s_DwmDisabledUntilTick = nowTick + 5000;
                HookLog(
                    "DX9: DwmFlush disabled for 5000ms (dwmUs=%lld expectedUs=%lld "

                    "hz=%d)",
                    dwmUs, expectedUs, hz);
            }
        } else {
            s_DwmBadCadenceCount = 0;
        }

        // If DwmFlush didn't actually block (or blocked at an unexpected cadence),
        // fall back.
    }

    // Fallback: deterministic pacer to the desktop refresh.
    PaceToRefreshQpc();

}
