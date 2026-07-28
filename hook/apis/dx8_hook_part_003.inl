    }

    void CaptureFrame(IDirect3DDevice8* device, bool useFrontBuffer = true) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock())
            return;
        if (!initialized || !d3d9DeviceEx || !d3d9SharedSurface)
            return;

        HWND hwnd = overlayHwnd;
        RECT rect = {};
        if (hwnd && GetClientRect(hwnd, &rect)) {
            const uint32_t currentWidth = static_cast<uint32_t>(std::max<LONG>(0, rect.right - rect.left));
            const uint32_t currentHeight = static_cast<uint32_t>(std::max<LONG>(0, rect.bottom - rect.top));
            if (currentWidth > 0 && currentHeight > 0 && (currentWidth != width || currentHeight != height)) {
                HookLog("DX8: Capture resize detected (%ux%u -> %ux%u); rebuilding shared transport", width, height,
                        currentWidth, currentHeight);
                if (!CleanupDX8(false))
                    return;
                Init(device, hwnd);
                if (!initialized)
                    return;
            }
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int idx = FindAvailableCaptureTextureSlot(captureSharedMem, writeIndex.load(std::memory_order_relaxed));
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        const bool copied = useFrontBuffer ? CopyFrontBufferToSurface9(device, d3d9SharedSurface)
                                           : CopyBackBufferToSurface9(device, d3d9SharedSurface);
        if (!copied) {
            return;
        }

        D3DLOCKED_RECT lockedRect = {};
        const HRESULT lockHr = d3d9SharedSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY);
        if (FAILED(lockHr) || !lockedRect.pBits)
            return;
        d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, lockedRect.pBits, lockedRect.Pitch, 0);
        d3d9SharedSurface->UnlockRect();

        // Signal fence if available
        uint64_t publishedFenceValue = 0;
        if (useFences && context4 && fence) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DX8: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0)
            d3d11Context->Flush();

        // PASS RAW QPC
        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
        AdvanceWriteIndex();
    }
};

static DX8Capture g_DX8Capture;

static void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float limit) {
    if (limit < 0.0f)
        return;

    // We need D3D9Ex device for queries
    if (!g_DX8Capture.d3d9DeviceEx) {
        HWND hwnd = ResolveD3D8TargetWindow(device, g_CachedHwnd);
        if (hwnd) {
            if (!g_DX8Capture.EnsureOverlayDevice(device, hwnd))
                return;
        } else
            return;
    }

    IDirect3DDevice9Ex* dev = g_DX8Capture.d3d9DeviceEx;

    if (g_PrerenderQueries.empty()) {
        g_PrerenderQueries.resize(16, nullptr);
    }

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial (Wait for current frame)
        IDirect3DQuery9* q = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
        if (!q) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
            g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()] = q;
        }
        if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                SwitchToThread();
            }
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        IDirect3DQuery9* currentQ = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
        if (!currentQ) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &currentQ);
            g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()] = currentQ;
        }
        if (currentQ)
            currentQ->Issue(D3DISSUE_END);

        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            IDirect3DQuery9* waitQ = g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) % g_PrerenderQueries.size()];
            if (waitQ) {
                while (waitQ->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    SwitchToThread();
                }
            }
        }
    }
    g_PrerenderFrameIndex++;

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

static void DrawDX8Overlay(IDirect3DDevice8* device, HWND hwnd) {
    if (!device || !hwnd)
        return;

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    if (!ResolveD3D8RenderSize(device, hwnd, &renderWidth, &renderHeight)) {
        return;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);  // Hook input for menu
        g_OverlayAdapter.SetHwnd(hwnd);

        if (g_OverlayAdapter.InitDX8(device)) {
            g_OverlayAdapter.SetHwnd(hwnd);
            EarlyLog("DX8: OverlayAdapter initialized (direct DX8)");
        }

        if (!g_DX8HooksInitialized && device) {
            InstallD3D8SamplerHooks(device);
            HookLog("DX8: State hooks initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX8Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI("DX8", "active IDirect3DDevice8");

    {
        DX8StateHookBypassScope bypassScope;
        g_OverlayAdapter.RenderOverlay(static_cast<int>(renderWidth), static_cast<int>(renderHeight));
    }
    static uint32_t overlayRenderSubmitCount = 0;
    overlayRenderSubmitCount++;
    if (overlayRenderSubmitCount <= 8) {
        HookLogImportant("DX8: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)", hwnd, renderWidth,
                         renderHeight, overlayRenderSubmitCount);
    }
}

static HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion) {
    if (HookIsShuttingDown())
        return D3D_OK;
    D3D8SamplerVTableRecord* samplerRecord = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState = samplerRecord
                                                   ? samplerRecord->setState.load(std::memory_order_acquire)
                                                   : oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState = samplerRecord
                                                   ? samplerRecord->getState.load(std::memory_order_acquire)
                                                   : oD3D8GetTextureStageState;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D8, device,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    bool helperRequired = DX8HelperRequired(shm, isRecording);
    HWND targetHwnd = ResolveD3D8TargetWindow(device, hDestWindowOverride);
    if (targetHwnd) {
        g_CachedHwnd = targetHwnd;
    }
    auto ensureCapture = [&]() {
        if (!targetHwnd) {
            return false;
        }
        if (!g_DX8Capture.initialized) {
            g_DX8Capture.Init(device, targetHwnd);
        }
        return g_DX8Capture.initialized;
    };

    // CPU Prerender Limit
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
        ApplyPrerenderLimitDX8(device, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    if (isRecording && shouldDrawOverlay && !captureIncludeOverlay && ensureCapture()) {
        g_DX8Capture.CaptureFrame(device, false);
    }

    if (shouldDrawOverlay) {
        DrawDX8Overlay(device, targetHwnd);
    }

    HRESULT hr = oD3D8Present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

    if (isRecording && (!shouldDrawOverlay || captureIncludeOverlay) && ensureCapture()) {
        g_DX8Capture.CaptureFrame(device, true);
    } else if (!helperRequired && (g_DX8Capture.initialized || g_DX8Capture.d3d9DeviceEx)) {
        g_DX8Capture.Cleanup();
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    return hr;
}

// Hook: D3D8 Reset
static HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device, void* pPresentationParameters) {
    HookLog("DX8: Reset called");

    // Cleanup ImGui
    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Cleanup capture
    g_DX8Capture.PrepareForDeviceReset();

    // VSync Override
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default" && pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            if (mode == "off")
                pp->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_IMMEDIATE
            else if (mode == "fifo")
                pp->FullScreen_PresentationInterval = 0x00000001;  // D3DPRESENT_INTERVAL_ONE
            else if (mode == "adaptive")
                pp->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox")
                pp->FullScreen_PresentationInterval = 0x80000000;
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            pp->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        if (pPresentationParameters) {
            // We need the IDirect3D8 object to check support, but Reset doesn't
            // provide it We'll trust the user and just apply it if it's discarded
            // swap effect anyway
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
            if (msaa[0] != 'd') {
                DWORD msType = ParseD3D8MSAA(msaa);
                if (msType != 0) {
                    pp->MultiSampleType = msType;
                    pp->SwapEffect = 1;  // DISCARD
                } else if (strcmp(msaa, "off") == 0) {
                    pp->MultiSampleType = 0;
                }
            }
        }
    }

    const HRESULT hr = oD3D8Reset(device, pPresentationParameters);
    if (SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ResetDevice(ce::legacy_d3d_sampler_state::Api::D3D8, device);
    }
    return hr;
}

// Hook: D3D8 SetTextureStageState
static HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD Value) {
    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : oD3D8GetTextureStageState;
    if (g_DX8StateHookBypassDepth > 0) {
        return setState(device, Stage, Type, Value);
    }
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, Value,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                 DWORD* pValue) {
    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : oD3D8GetTextureStageState;
    if (g_DX8StateHookBypassDepth > 0) {
        return getState(device, Stage, Type, pValue);
    }
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, pValue,
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState), QueryD3D8MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device, DWORD Token) {
    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8ApplyStateBlock_t applyStateBlock =
        record ? record->applyStateBlock.load(std::memory_order_acquire) : oD3D8ApplyStateBlock;
    if (!applyStateBlock)
        return E_FAIL;
    const HRESULT hr = applyStateBlock(device, Token);
    if (SUCCEEDED(hr) && g_DX8StateHookBypassDepth == 0) {
        const D3D8SetTextureStageState_t setState =
            record ? record->setState.load(std::memory_order_acquire) : oD3D8SetTextureStageState;
        const D3D8GetTextureStageState_t getState =
            record ? record->getState.load(std::memory_order_acquire) : oD3D8GetTextureStageState;
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D8, device,
            reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
            reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
    }
    return hr;
}

// Hook: D3D8 CreateDevice
static HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3D8_PRESENT_PARAMETERS* pPresentationParameters,
                                                        IDirect3DDevice8** ppDevice) {
    if (g_IPC && pPresentationParameters) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off")
                pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            else if (mode == "fifo")
                pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "adaptive")
                pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox")
                pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            HookLog("DX8: CreateDevice VSync overridden to %08x",
                    pPresentationParameters->FullScreen_PresentationInterval);
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        ApplyDX8MSAAOverride(d3d, Adapter, DeviceType, pPresentationParameters);
    }

    HRESULT hr =
        oD3D8CreateDevice(d3d, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ce::legacy_d3d_sampler_state::RegisterDevice(ce::legacy_d3d_sampler_state::Api::D3D8, *ppDevice, true,
                                                     QueryD3D8MaxAnisotropy);
        InstallD3D8DeviceHooks(*ppDevice);
    }

    return hr;
}

void DX8Hook::Init() {
    HookLog("DX8Hook::Init()");

    // Check if d3d8.dll is loaded
    HMODULE d3d8Module = GetModuleHandleA("d3d8.dll");
    if (!d3d8Module) {
        return;
    }

    TryInstallDirect3DCreate8Hook(d3d8Module);
}

void DX8Hook::Shutdown() {
    HookLog("DX8Hook::Shutdown()");
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D8);

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DX8Capture.CleanupDX8(true);
}

void DX8Hook::OnHostDisconnect() {
    HookLog("DX8Hook::OnHostDisconnect()");
    g_DX8Capture.CleanupDX8(true);
}
