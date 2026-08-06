#include "dx9_hook_internal.h"


D3D9SamplerCallbacks ResolveD3D9SamplerCallbacks(IDirect3DDevice9* device) {


    uintptr_t* vtable = device ? *(uintptr_t**)device : nullptr;
    D3D9SamplerVTableRecord* record = nullptr;
    if (vtable && dx9_hook_t_D3D9SamplerVTable == vtable && dx9_hook_t_D3D9SamplerVTableRecord) {
        record = dx9_hook_t_D3D9SamplerVTableRecord;
    } else if (vtable) {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        for (const auto& entry : dx9_hook_g_D3D9SamplerVTables) {
            if (entry->vtable == vtable) {
                record = entry.get();
                break;
            }
        }
        dx9_hook_t_D3D9SamplerVTable = vtable;
        dx9_hook_t_D3D9SamplerVTableRecord = record;
    }

    if (!record) {
        return {dx9_hook_oSetTexture, dx9_hook_oGetSamplerState, dx9_hook_oSetSamplerState, nullptr, nullptr};
    }
    return {
        record->setTexture.load(std::memory_order_acquire),
        record->getSamplerState.load(std::memory_order_acquire),
        record->setSamplerState.load(std::memory_order_acquire),
        record->createStateBlock.load(std::memory_order_acquire),
        record->endStateBlock.load(std::memory_order_acquire),
    };

}

bool IsDX9InternalHelperBypassActive() {


    return dx9_hook_g_InternalHelperBypassDepth != 0;

}

bool IsDX9InternalHelperDevice(IDirect3DDevice9* device) {


    if (!device) {
        return false;
    }

    std::lock_guard<std::mutex> lock(dx9_hook_g_InternalHelperDeviceMutex);
    return dx9_hook_g_InternalHelperDevices.find(device) != dx9_hook_g_InternalHelperDevices.end();

}

bool ShouldBypassDX9HooksForDevice(IDirect3DDevice9* device) {


    return IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device);

}

void RegisterD3D9DeviceIdentity(IDirect3DDevice9* device,  bool isEx,  const char* evidence) {


    if (!device || IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device))
        return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9IdentityMutex);
        const auto it = dx9_hook_g_D3D9ExDevices.find(device);
        changed = it == dx9_hook_g_D3D9ExDevices.end() || it->second != isEx;
        dx9_hook_g_D3D9ExDevices[device] = isEx;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D9 device identity device=%p api=%s evidence=%s", device,
                         isEx ? "DX9Ex" : "DX9", evidence ? evidence : "unknown");
    }

}

bool ResolveD3D9DeviceIsEx(IDirect3DDevice9* device) {


    if (!device)
        return false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9IdentityMutex);
        const auto it = dx9_hook_g_D3D9ExDevices.find(device);
        if (it != dx9_hook_g_D3D9ExDevices.end())
            return it->second;
    }

    IDirect3DDevice9Ex* deviceEx = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&deviceEx))) && deviceEx;
    if (deviceEx)
        deviceEx->Release();
    RegisterD3D9DeviceIdentity(device, isEx, "late-device-interface-probe");
    return isEx;

}

bool ShouldBypassDX9HooksForSwapChain(IDirect3DSwapChain9* swapChain) {


    if (IsDX9InternalHelperBypassActive()) {
        return true;
    }
    if (!swapChain) {
        return false;
    }

    IDirect3DDevice9* device = nullptr;
    const HRESULT hr = swapChain->GetDevice(&device);
    if (FAILED(hr) || !device) {
        return false;
    }

    const bool bypass = IsDX9InternalHelperDevice(device);
    device->Release();
    return bypass;

}

bool ShouldSkipDX9PresentForVulkan() {


    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;

    if (IsDXVKD3D9WrapperLoaded()) {
        static int dxvkPreferLogCount = 0;
        if (dxvkPreferLogCount < 6) {
            HookLogImportant("DX9: DXVK d3d9 wrapper detected; keeping DX9 present path active");
            dxvkPreferLogCount++;
        }
        return false;
    }

    return true;

}

bool ShouldSkipDX9OverlayForVulkan() {


    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;
    if (!shm->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive))
        return false;

    static int overlaySkipLogCount = 0;
    if (overlaySkipLogCount < 6) {
        HookLogImportant("DX9: Vulkan layer overlay active; skipping DX9 overlay rendering");
        overlaySkipLogCount++;
    }
    return true;

}

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

const char* D3D9FormatName(D3DFORMAT format) {


    switch (format) {
        case D3DFMT_A8R8G8B8:
            return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:
            return "X8R8G8B8";
        case D3DFMT_A2B10G10R10:
            return "A2B10G10R10";
        case D3DFMT_UNKNOWN:
            return "UNKNOWN";
        default:
            return "OTHER";
    }

}

D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa) {


    if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
        return D3DMULTISAMPLE_2_SAMPLES;
    if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
        return D3DMULTISAMPLE_4_SAMPLES;
    if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
        return D3DMULTISAMPLE_8_SAMPLES;
    return D3DMULTISAMPLE_NONE;

}

void ApplyMSAAOverride(IDirect3D9* d3d,  UINT adapter,  D3DDEVTYPE deviceType,  D3DPRESENT_PARAMETERS* pp) {


    if (!pp)
        return;

    const auto& gfx = GetActiveGraphicsConfig();
    const char* msaa = gfx.msaaSamples.c_str();
    if (msaa[0] == 'd')
        return;  // default

    D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);

    EarlyLog(
        "DX9: ApplyMSAAOverride checking '%s' (Parsed=%d). BBFormat=%d "
        "Windowed=%d",
        msaa, msType, pp->BackBufferFormat, pp->Windowed);

    if (msType != D3DMULTISAMPLE_NONE) {
        DWORD quality;
        // Ensure format is valid for check? If 0 (Unknown), use adapter format?
        D3DFORMAT fmt = pp->BackBufferFormat;
        if (fmt == D3DFMT_UNKNOWN)
            fmt = D3DFMT_X8R8G8B8;  // Fallback guess

        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, deviceType, fmt, pp->Windowed, msType, &quality))) {
            pp->MultiSampleType = msType;
            pp->MultiSampleQuality = 0;
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            // Also clear flags that might conflict
            pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

            HookLog("DX9: Forcing MSAA %d samples (Format %d)", (int)msType, fmt);
        } else {
            HookLog("DX9: MSAA %d samples NOT SUPPORTED for Format %d", (int)msType, fmt);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        HookLog("DX9: Forcing MSAA OFF");
    }

}

void DrawDX9Overlay(IDirect3DDevice9* device) {


    if (ShouldSkipDX9OverlayForVulkan()) {
        return;
    }
    static int drawLogCount = 0;
    static int initFailCount = 0;

    if (drawLogCount < 5) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        HookLogImportant("DX9: DrawDX9Overlay #%d, IsInitialized=%d, IPC=%p, SHM=%p, showOverlay=%d", drawLogCount,
                         g_OverlayAdapter.IsInitialized() ? 1 : 0, (void*)g_IPC, (void*)dbgShm,
                         dbgShm ? dbgShm->ReadOverlayConfig().showOverlay : -1);
        drawLogCount++;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        // Get the window handle
        D3DDEVICE_CREATION_PARAMETERS params;
        HRESULT paramsHr = device->GetCreationParameters(&params);
        if (FAILED(paramsHr)) {
            if (initFailCount < 3) {
                EarlyLog("DX9: GetCreationParameters failed (hr=0x%08X)", paramsHr);
                initFailCount++;
            }
            return;
        }
        dx9_hook_g_CachedHwnd = params.hFocusWindow;

        // Hook Input
        InputManager::Get().HookWindow(dx9_hook_g_CachedHwnd);
        g_OverlayAdapter.SetHwnd(dx9_hook_g_CachedHwnd);

        EarlyLog("DX9: Attempting OverlayAdapter::InitDX9 (device=%p, hwnd=%p)", (void*)device, (void*)dx9_hook_g_CachedHwnd);
        if (g_OverlayAdapter.InitDX9(device)) {
            g_OverlayAdapter.SetHwnd(dx9_hook_g_CachedHwnd);
            EarlyLog("DX9: OverlayAdapter initialized successfully");
        } else {
            if (initFailCount < 3) {
                EarlyLog("DX9: OverlayAdapter::InitDX9 FAILED");
                initFailCount++;
            }
            return;
        }
    }

    // Get viewport size
    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);

    static int vpLogCount = 0;
    if (vpLogCount < 3) {
        HookLogImportant("DX9: DrawDX9Overlay vp=%ux%u (device=%p, IPC=%p)", vp.Width, vp.Height, (void*)device,
                         (void*)g_IPC);
        vpLogCount++;
    }

    g_OverlayAdapter.SetMetrics(&dx9_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(dx9_hook_g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
    const bool isEx = ResolveD3D9DeviceIsEx(device);
    const char* finalApi = ce::graphics_api_identity::D3D9Label(isEx, IsDXVKD3D9WrapperLoaded());
    g_OverlayAdapter.SetGraphicsAPI(finalApi, isEx ? "active IDirect3DDevice9Ex" : "active IDirect3DDevice9");

    // Render Custom Overlay
    // Note: RenderOverlay calls BeginFrame/RenderContent/EndFrame.
    // DX9 backend handles state saving/restoring internally.
    dx9_hook_g_InOverlayRender = true;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OverlayAdapter.RenderOverlay(vp.Width, vp.Height);
    dx9_hook_g_InOverlayRender = false;

}

void CaptureDX9Screenshot(IDirect3DDevice9* device,  SharedMemoryLayout* shm,  uint64_t requestId) {


    if (!device || !shm || requestId == 0)
        return;

    bool queued = false;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &bb)) && bb) {
        D3DSURFACE_DESC bbDesc;
        bb->GetDesc(&bbDesc);

        IDirect3DSurface9* staging = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                          &staging, NULL))) {
            if (SUCCEEDED(device->GetRenderTargetData(bb, staging))) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(staging->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
                    if (locked.Pitch > 0) {
                        queued = QueueScreenshotPixels(shm, requestId, static_cast<const uint8_t*>(locked.pBits),
                                                       bbDesc.Width, bbDesc.Height, static_cast<uint32_t>(locked.Pitch),
                                                       ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB);
                    }
                    staging->UnlockRect();
                }
            }
            staging->Release();
        }
        bb->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);

}

bool IsD3D9On12Loaded() {


    static int s_loaded = -1;
    HMODULE d3d9on12 = GetModuleHandleA("d3d9on12.dll");
    if (d3d9on12) {
        s_loaded = 1;
    } else if (s_loaded < 0) {
        s_loaded = 0;
    }
    return s_loaded > 0;

}
