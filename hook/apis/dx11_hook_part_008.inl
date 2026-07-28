        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        s_HookedDevice = device;
    }

    if (s_LastDX10OverlayHwnd && s_LastDX10OverlayHwnd != currentHwnd && g_OverlayAdapter.IsInitialized()) {
        HookLogImportant("DX10: HWND changed from %p to %p, reinitializing overlay backend", s_LastDX10OverlayHwnd,
                         currentHwnd);
        g_OverlayAdapter.Shutdown();
    }
    s_LastDX10OverlayHwnd = currentHwnd;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        static std::atomic<int> s_getDescFailureLogCount{0};
        const int logCount = s_getDescFailureLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5) {
            HookLogImportant("DX10: DrawOverlay failed to get swapchain desc for %p", pSwapChain);
        }
        device->Release();
        return;
    }

    const UINT bufferIndex = ResolveDX11BackBufferIndex(pSwapChain, &desc);
    if (!g_mainRenderTargetView10 || s_LastDX10OverlaySwapChain != pSwapChain ||
        s_LastDX10OverlayBufferIndex != bufferIndex) {
        if (g_mainRenderTargetView10) {
            g_mainRenderTargetView10->Release();
            g_mainRenderTargetView10 = nullptr;
        }

        ID3D10Texture2D* backbuffer = nullptr;
        HRESULT hr = pSwapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer) {
            hr = device->CreateRenderTargetView(backbuffer, nullptr, &g_mainRenderTargetView10);
            backbuffer->Release();
        }
        if (FAILED(hr) || !g_mainRenderTargetView10) {
            static std::atomic<int> s_rtvFailureLogCount{0};
            const int logCount = s_rtvFailureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant("DX10: Failed to create overlay RTV for swapchain %p buffer=%u hr=0x%08X", pSwapChain,
                                 bufferIndex, (unsigned)hr);
            }
            device->Release();
            return;
        }
        s_LastDX10OverlaySwapChain = pSwapChain;
        s_LastDX10OverlayBufferIndex = bufferIndex;
        HookLogImportant("DX10: Overlay RTV ready for swapchain %p buffer=%u", pSwapChain, bufferIndex);
    }

    // Render the overlay
    if (!g_OverlayAdapter.IsInitialized()) {
        HookLog("DX10: Initializing OverlayAdapter...");
        g_OverlayAdapter.SetHwnd(currentHwnd);
        g_OverlayAdapter.InitDX10(device);
    }

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        g_OverlayAdapter.SetIPCClient(g_IPC);
        const bool is10_1 = ResolveD3D10Is10_1(device, pSwapChain);
        const std::string apiLabel = ce::graphics_api_identity::D3D10Label(is10_1, IsDXVKD3D10OrD3D11Loaded());
        g_OverlayAdapter.SetGraphicsAPI(apiLabel.c_str(), "active D3D10 swapchain device");
        const auto presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format);
        g_OverlayAdapter.SetHDR(ce::presentation_color::IsHDR(presentationEncoding),
                                static_cast<int>(desc.BufferDesc.Format));

        RECT rect;
        if (GetClientRect(currentHwnd, &rect)) {
            int width = rect.right - rect.left;
            int height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                ID3D10RenderTargetView* previousRTV = nullptr;
                ID3D10DepthStencilView* previousDSV = nullptr;
                device->OMGetRenderTargets(1, &previousRTV, &previousDSV);
                ID3D10RenderTargetView* overlayRTV = g_mainRenderTargetView10;
                device->OMSetRenderTargets(1, &overlayRTV, nullptr);
                g_OverlayAdapter.RenderOverlay(width, height);
                device->OMSetRenderTargets(1, &previousRTV, previousDSV);
                if (previousRTV) {
                    previousRTV->Release();
                }
                if (previousDSV) {
                    previousDSV->Release();
                }
            }
        }
    }

    device->Release();
}

static bool IsReadableMemoryDX11(const void* ptr, size_t size) {
    if (!ptr)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return (mbi.Protect &
            (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

void DrawDX11Overlay(IDXGISwapChain* pSwapChain) {
    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    // when D3D device is destroyed while we're trying to use it
    if (HookIsShuttingDown()) {
        return;
    }

    // CRITICAL: Null pointer check
    if (!pSwapChain) {
        return;
    }

    static IDXGISwapChain* lastSwapChain = nullptr;
    static HWND lastHwnd = NULL;
    static int frameCount = 0;
    static IDXGISwapChain* s_lastNvPresentOverlaySwapChain = nullptr;
    static int64_t s_lastNvPresentOverlayUs = 0;
    static UINT s_lastNvPresentOverlayBufferIndex = 0xFFFFFFFFu;

    frameCount++;

    // SAFETY: Verify the swapchain pointer is valid before accessing it
    if (!IsReadableMemoryDX11(pSwapChain, sizeof(void*))) {
        EarlyLog("DX11: Swapchain memory not readable at frame %d — shutting down", frameCount);
        RequestHookShutdown();
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        EarlyLog("DX11: GetDesc failed at frame %d — bailing", frameCount);
        return;
    }
    HWND currentHwnd = desc.OutputWindow;
    const bool isFlipSwapchain =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    const UINT resolvedBufferIndex = ResolveDX11BackBufferIndex(pSwapChain, &desc);

    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    if (nvPresentLoaded && ShouldSkipWindowForNvPresent(currentHwnd)) {
        return;
    }

    // NVIDIA Smooth Motion can trigger paired Present callbacks for the same
    // frame in quick succession. Only suppress near-immediate duplicates for the
    // exact same backbuffer to avoid dropping legitimate output frames.
    if (nvPresentLoaded) {
        static constexpr int64_t kNvPresentExactDuplicateUs = 500;
        int64_t nowUs = PerfLogger::GetQpcUs();
        bool hasPreviousSample = (s_lastNvPresentOverlayUs > 0 && nowUs > s_lastNvPresentOverlayUs);
        int64_t deltaUs = hasPreviousSample ? (nowUs - s_lastNvPresentOverlayUs) : 0;
        bool sameSwapChain = (s_lastNvPresentOverlaySwapChain == pSwapChain);
        bool sameBackBuffer = (!isFlipSwapchain || resolvedBufferIndex == s_lastNvPresentOverlayBufferIndex);

        bool skipExactDuplicate =
            sameSwapChain && sameBackBuffer && hasPreviousSample && deltaUs < kNvPresentExactDuplicateUs;

        if (skipExactDuplicate) {
            return;
        }
        s_lastNvPresentOverlaySwapChain = pSwapChain;
        s_lastNvPresentOverlayUs = nowUs;
        s_lastNvPresentOverlayBufferIndex = isFlipSwapchain ? resolvedBufferIndex : 0xFFFFFFFFu;
    }

    // Skip overlay if the window is being destroyed — the D3D device may already
    // be partially torn down, making GetDevice/GetBuffer unsafe.
    if (!currentHwnd || !IsWindow(currentHwnd)) {
        EarlyLog(
            "DX11: Window invalid at frame %d (hwnd=%p, IsWindow=%d) — "
            "shutting down",
            frameCount, currentHwnd, currentHwnd ? IsWindow(currentHwnd) : 0);
        // CRITICAL: Set shutdown flag and tell overlay adapter to skip cleanup
        // The app is tearing down and any Release() call can crash. Let OS clean
        // up.
        RequestHookShutdown();
        g_OverlayAdapter.SetShutdownMode(true);  // Tell adapter to skip destructor cleanup
        return;
    }

    const DXGIShared::APIType swapChainApi = DetectSwapChainAPITypeForDX11Hook(pSwapChain);
    if (swapChainApi == DXGIShared::APIType::D3D10) {
        g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);
        DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
        return;
    }
    if (swapChainApi != DXGIShared::APIType::D3D11) {
        static std::atomic<int> s_unexpectedApiLogCount{0};
        if (s_unexpectedApiLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: DrawOverlay skipping swapchain classified as %s", GetDX11HookBaseAPIName(swapChainApi));
        }
        return;
    }
    g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);

    if (frameCount % 60 == 0) {
        EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", frameCount, pSwapChain, currentHwnd,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    }

    // Acquire device/context — use cached (AddRef'd) pointers for subsequent
    // frames. Calling pSwapChain->GetDevice() every frame is unsafe during
    // shutdown (the swapchain's internal device ref can be freed before our
    // Present hook stops being called).
    if (!g_pd3dDevice) {
        // First frame: cache the DX11 device with AddRef.
        ID3D11Device* device11 = NULL;
        HRESULT hrDevice = pSwapChain->GetDevice(IID_PPV_ARGS(&device11));
        if (FAILED(hrDevice) || !device11) {
            EarlyLog("%s: FAILED to get D3D11 device (hr=0x%08X)", g_DetectedAPI, hrDevice);
            return;
        }
        EarlyLog("%s: Identified as D3D11 device %p", g_DetectedAPI, device11);

        // Initialize System Metrics (one-time)
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Cache device — GetDevice already AddRef'd, so we keep that ref
        g_pd3dDevice = device11;
        // Cache context with AddRef
        g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
    }

    ID3D11Device* device = g_pd3dDevice;
    ID3D11DeviceContext* context = g_pd3dDeviceContext;
    if (g_D3D11IdentitySwapChain != pSwapChain) {
        ID3D11Device* identityDevice = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&identityDevice))) && identityDevice) {
            if (g_D3D11IdentityDevice)
                g_D3D11IdentityDevice->Release();
            g_D3D11IdentityDevice = identityDevice;
            g_D3D11IdentitySwapChain = pSwapChain;
        }
    }
    ID3D11Device* activeIdentityDevice =
        (g_D3D11IdentitySwapChain == pSwapChain && g_D3D11IdentityDevice) ? g_D3D11IdentityDevice : device;
    DX11Hook_RegisterDeviceIdentity(activeIdentityDevice, "active D3D11 swapchain device");
    const std::string apiLabel =
        ce::graphics_api_identity::D3D11Label(ResolveD3D11MinorUse(activeIdentityDevice), IsDXVKD3D10OrD3D11Loaded());

    if (g_OverlayAdapter.IsInitialized() && currentHwnd != g_CachedHwnd) {
        HookLog("DX11: HWND changed, shutting down OverlayAdapter");
        g_OverlayAdapter.Shutdown();
    }

    if (!g_OverlayAdapter.IsInitialized() || currentHwnd != g_CachedHwnd) {
        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }
        g_CachedHwnd = currentHwnd;
        lastHwnd = currentHwnd;

        InputManager::Get().HookWindow(currentHwnd);
        g_OverlayAdapter.SetHwnd(currentHwnd);

        if (g_OverlayAdapter.InitDX11(device, context)) {
            g_OverlayAdapter.SetHwnd(currentHwnd);
            EarlyLog("DX11: OverlayAdapter initialized for HWND %p", currentHwnd);
        } else {
            EarlyLog("DX11: OverlayAdapter::InitDX11 FAILED for HWND %p", currentHwnd);
        }
    }

    // Detect HWND change (multi-window apps)
    if (currentHwnd != lastHwnd) {
        EarlyLog(
            "DX11: HWND changed from %p to %p. Overlay might not be visible "
            "on new window without re-init.",
            lastHwnd, currentHwnd);
        lastHwnd = currentHwnd;
    }

    const auto presentationEncoding =
        DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, desc.BufferDesc.Format);
    const bool isHDR = ce::presentation_color::IsHDR(presentationEncoding);
    g_OverlayAdapter.SetHDR(isHDR, (int)desc.BufferDesc.Format);

    // Propagate HDR state to media engine via shared memory
    if (g_pSharedMem) {
        g_pSharedMem->SetIsHDR(isHDR);
    }

    g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI(apiLabel.c_str(), "active D3D11 swapchain device");

    ID3D11RenderTargetView* overlayRTV = nullptr;
    bool usingBoundRTV = false;

    // When Smooth Motion is active, prefer the RTV currently bound by the game.
    // This better matches the actual frame target and reduces overlay flicker.
    if (nvPresentLoaded && context) {
        context->OMGetRenderTargets(1, &overlayRTV, NULL);
        if (overlayRTV) {
            usingBoundRTV = true;
        }
    }

    if (!usingBoundRTV) {
        // For FLIP swap chains, the back buffer rotates each Present.
        // Overlay must draw to the same buffer that CaptureFrame will read,
        // otherwise the captured frame will not contain the overlay (flicker).
        static UINT lastBufferIndex = 0xFFFFFFFF;

        // Create/recreate RTV if swapchain changed or FLIP buffer index rotated
        if (!g_mainRenderTargetView || pSwapChain != lastSwapChain || resolvedBufferIndex != lastBufferIndex) {
            if (g_mainRenderTargetView) {
                g_mainRenderTargetView->Release();
                g_mainRenderTargetView = nullptr;
            }

            EarlyLog("%s: Creating RTV for SwapChain %p (%ux%u) buffer=%u...", g_DetectedAPI, pSwapChain,
                     desc.BufferDesc.Width, desc.BufferDesc.Height, resolvedBufferIndex);

            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = pSwapChain->GetBuffer(resolvedBufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                EarlyLog("%s: GetBuffer(%u) FAILED hr=0x%08X", g_DetectedAPI, resolvedBufferIndex, hr);
                return;
            }
            hr = device->CreateRenderTargetView(backbuffer, NULL, &g_mainRenderTargetView);
            backbuffer->Release();
            if (FAILED(hr)) {
                EarlyLog("%s: CreateRTV FAILED hr=0x%08X", g_DetectedAPI, hr);
                return;
            }
            lastSwapChain = pSwapChain;
            lastBufferIndex = resolvedBufferIndex;
            EarlyLog("%s: RTV created OK (buffer=%u)", g_DetectedAPI, resolvedBufferIndex);
        }

        overlayRTV = g_mainRenderTargetView;
    }

    EarlyLog("DX11: [frame %d] pre-render device=%p context=%p rtv=%p", frameCount, device, context, overlayRTV);

    // Preserve game OM/viewport state around overlay target binding.
    ID3D11RenderTargetView* previousRTV = nullptr;
    ID3D11DepthStencilView* previousDSV = nullptr;
    context->OMGetRenderTargets(1, &previousRTV, &previousDSV);

    D3D11_VIEWPORT previousViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT previousViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&previousViewportCount, previousViewports);

    context->OMSetRenderTargets(1, &overlayRTV, NULL);

    // Explicitly set viewport
    D3D11_VIEWPORT vp;
    vp.Width = (float)desc.BufferDesc.Width;
    vp.Height = (float)desc.BufferDesc.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    context->RSSetViewports(1, &vp);

    EarlyLog("DX11: [frame %d] calling RenderOverlay", frameCount);

    // Render Custom Overlay
    g_InOverlayRender = true;
    g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);
    g_InOverlayRender = false;

    if (previousViewportCount > 0) {
        context->RSSetViewports(previousViewportCount, previousViewports);
    }
    context->OMSetRenderTargets(1, &previousRTV, previousDSV);

    if (previousRTV) {
        previousRTV->Release();
    }
    if (previousDSV) {
        previousDSV->Release();
    }

    if (usingBoundRTV && overlayRTV) {
        overlayRTV->Release();
    }
}

// Handle SwapChain resize - must release RTV and reinitialize ImGui
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // CRITICAL: Check for shutdown first - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        if (oResizeBuffers) {
            return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        return S_OK;
    }

    // RECURSION BREAKER: If we are calling ourselves recursively, bail out
    // immediately. This handles the "Hooked the Hook" scenario or infinite
    // unhook/rehook loops.
    if (g_ResizeBuffersDepth > 0) {
        // WrapperLog("DX11: ResizeBuffers recursion detected! Bailing to prevent
        // crash."); We must call original if possible, but if original points to
        // us, we can't. If oResizeBuffers == DetourResizeBuffers, we are stuck.
        // Safe bet: just return S_OK to stop the madness.
        return S_OK;
    }
    g_ResizeBuffersDepth++;

    // Guard for auto-resetting depth
    auto depthGuard = ce::make_scope_guard([&] { g_ResizeBuffersDepth--; });

    HookLog("DX11: ResizeBuffers called (%dx%d)", Width, Height);

    // Safety: Check if oResizeBuffers is valid
    if (!oResizeBuffers) {
        HookLog("DX11: ResizeBuffers - Original function pointer is NULL! Bailing.");
        return S_OK;
    }

    // Safety: Check if oResizeBuffers points to US (Cycle Detection)
    if ((void*)oResizeBuffers == (void*)DetourResizeBuffers) {
        HookLog(
            "DX11: ResizeBuffers - Original function points to DETOUR! Cycle "
            "detected. Bailing.");
        return S_OK;
    }

    {
        ID3D12Device* d12Dev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&d12Dev))) && d12Dev) {
            d12Dev->Release();
            DX12_OnSwapchainResizeBegin();

            // SELF-DESTRUCT: We are a DX11 hook on a DX12 swapchain.
            // Unhook ourselves to prevent infinite loops.
            HookLog(
                "DX11: DetourResizeBuffers - DX12 detected. Unhooking DX11 "
                "ResizeBuffers from this SwapChain.");

            void** vtable = *(void***)pSwapChain;
            DWORD oldProtect;
            if (VirtualProtect(&vtable[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Double check we are overwriting OURSELVES (or a hook), not something
                // random But actually we just want to restore 'oResizeBuffers' (the
                // Real Original).
                vtable[13] = (void*)oResizeBuffers;
                VirtualProtect(&vtable[13], sizeof(void*), oldProtect, &oldProtect);
                HookLog("DX11: DetourResizeBuffers - VTable[13] restored to original.");
            } else {
                HookLog("DX11: DetourResizeBuffers - FAILED to restore VTable[13]!");
            }

            // Call original immediately
            HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
            // CRITICAL FIX: Reset the resize cleanup flag since we called Begin but
            // never called End
            DX12_OnSwapchainResizeEnd();
            return hr;
        }
    }

    // Release render target view before resize
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
    if (g_mainRenderTargetView10) {
        g_mainRenderTargetView10->Release();
        g_mainRenderTargetView10 = nullptr;
    }

    // Invalidate D3D11 resources for OverlayAdapter if needed
    // Typically OverlayAdapter Release/resize handling is done in Render logic or
    // internally But we can force a shutdown if we want fresh resources on resize
    if (g_OverlayAdapter.IsInitialized()) {
        // g_OverlayAdapter.Shutdown(); // Optional: Shutdown on resize?
        // Usually not needed for DX11 as backend handles it or uses swapchain
        // backbuffer which changes? Capture project uses OMSetRenderTargets. For
        // safety, let's just let it be.
    }

    // Check for Waitable Swapchain
    if (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        HookLog("DX11: ResizeBuffers: Waitable Swapchain detected");
    }

    // Apply backbuffer count override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
        // Check swap effect — don't reduce buffer count for flip model swapchains
        bool isFlip = false;
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
            isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                      scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        }
        UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
        if (isFlip && (UINT)count < gameCount) {
            HookLog("DX11: ResizeBuffers: Skipping BufferCount override %d < game's %u (flip model)", count, gameCount);
        } else {
            BufferCount = (UINT)count;
            HookLog("DX11: ResizeBuffers: Overriding BufferCount to %d", count);
        }
    }

    // Call original ResizeBuffers
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (FAILED(hr)) {
        HookLog("DX11: ResizeBuffers FAILED hr=0x%08X", hr);
    } else {
        g_DX11Capture.RequestGenerationReset(pSwapChain);
        HookLog("DX11: ResizeBuffers SUCCESS");
    }

    return hr;
}

// --- Prerender Limit Support ---
void ApplyPrerenderLimit(IDXGISwapChain* pSwapChain, float limit) {
    if (limit < 0.0f)
        return;

    ID3D11Device* dev = nullptr;
    if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev)))) {
        // D3D10 limits 1-6 use IDXGIDevice1::SetMaximumFrameLatency. DXGI has
        // no zero-depth value, so serialize limit 0 with a native event query.
        ID3D10Device* dev10 = nullptr;
        if (limit == 0.0f && SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev10))) && dev10) {
            std::lock_guard<std::mutex> prerenderLock(g_PrerenderMutex);
            if (g_PrerenderQueryDevice10 != dev10) {
                if (g_PrerenderSerialQuery10) {
                    g_PrerenderSerialQuery10->Release();
                    g_PrerenderSerialQuery10 = nullptr;
                }
                if (g_PrerenderQueryDevice10)
                    g_PrerenderQueryDevice10->Release();
                g_PrerenderQueryDevice10 = dev10;
                g_PrerenderQueryDevice10->AddRef();
                HookLogImportant("D3D10: Serial prerender query rebound to device=%p", dev10);
            }
            if (!g_PrerenderSerialQuery10) {
                D3D10_QUERY_DESC queryDesc = {};
                queryDesc.Query = D3D10_QUERY_EVENT;
                dev10->CreateQuery(&queryDesc, &g_PrerenderSerialQuery10);
            }
            if (g_PrerenderSerialQuery10) {
                g_PrerenderSerialQuery10->End();
                dev10->Flush();
                const int64_t waitStart = PerfLogger::GetQpcUs();
                while (g_PrerenderSerialQuery10->GetData(nullptr, 0, 0) == S_FALSE)
                    SwitchToThread();
                const int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
                const int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12)
                    HookLogImportant("D3D10: Prerender serial wait=%lldus (#%d)", (long long)waitUs, idx + 1);
                g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
            }
            dev10->Release();
            return;
        }
        if (dev10)
            dev10->Release();
        static std::atomic<int> s_nonD3D11LogCount{0};
        if (limit == 0.0f && s_nonD3D11LogCount.fetch_add(1, std::memory_order_relaxed) < 5)
            HookLogImportant("D3D10/11: Manual prerender query path unavailable for swapchain=%p", pSwapChain);
        return;
    }

    std::lock_guard<std::mutex> prerenderLock(g_PrerenderMutex);
    if (g_PrerenderQueryDevice != dev) {
        for (auto* query : g_PrerenderQueries) {
            if (query)
                query->Release();
        }
        g_PrerenderQueries.clear();
        g_PrerenderFrameIndex = 0;
        if (g_PrerenderQueryDevice)
            g_PrerenderQueryDevice->Release();
        g_PrerenderQueryDevice = dev;
        g_PrerenderQueryDevice->AddRef();
        HookLogImportant("DX11: Prerender query stream rebound to device=%p", dev);
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);

    if (g_PrerenderQueries.empty() || g_PrerenderQueries[0] == nullptr) {
        g_PrerenderQueries.clear();
        for (int i = 0; i < 16; i++) {
            D3D11_QUERY_DESC qd = {};
            qd.Query = D3D11_QUERY_EVENT;
            ID3D11Query* q = nullptr;
            if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
                g_PrerenderQueries.push_back(q);
            }
        }
        HookLogImportant("DX11: Created manual prerender query ring buffer (size: %d, limit=%.2f)",
                         (int)g_PrerenderQueries.size(), limit);
    }

    if (!g_PrerenderQueries.empty()) {
        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            ID3D11Query* q = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(q);
            int64_t waitStart = PerfLogger::GetQpcUs();
            while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {
                SwitchToThread();
            }
            int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
            int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12) {
                HookLogImportant("DX11: Prerender serial wait frame=%llu wait=%lldus (#%d)",
                                 (unsigned long long)g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
            }
        } else {
            const int lookback = std::clamp(static_cast<int>(limit), 1, 6);
