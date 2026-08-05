#include "dx11_hook_internal.h"

// Called from DXGI SwapChain wrapper for frame capture (wrapper-only
// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    HandleDX11ProcessFrame(pSwapChain, true);
}

static void DrawDX10Overlay(IDXGISwapChain* pSwapChain, HWND currentHwnd, int frameCount) {
    ID3D10Device* device = nullptr;
    if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void**)&device))) {
        if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&device))) {
            static std::atomic<int> s_getDeviceFailureLogCount{0};
            const int logCount = s_getDeviceFailureLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 5) {
                HookLogImportant("DX10: DrawOverlay failed to get ID3D10Device/Device1 from swapchain %p", pSwapChain);
            }
            return;
        }
    }

    // Capture/Hook on the real device seen in Present
    static ID3D10Device* s_HookedDevice = nullptr;
    static IDXGISwapChain* s_LastDX10OverlaySwapChain = nullptr;
    static UINT s_LastDX10OverlayBufferIndex = 0xFFFFFFFFu;
    static HWND s_LastDX10OverlayHwnd = nullptr;
    if (s_HookedDevice != device) {
        if (dx11_hook_g_mainRenderTargetView10) {
            dx11_hook_g_mainRenderTargetView10->Release();
            dx11_hook_g_mainRenderTargetView10 = nullptr;
        }
        s_LastDX10OverlaySwapChain = nullptr;
        s_LastDX10OverlayBufferIndex = 0xFFFFFFFFu;
        s_LastDX10OverlayHwnd = currentHwnd;
        if (g_OverlayAdapter.IsInitialized()) {
            HookLogImportant("DX10: Device changed, reinitializing overlay backend");
            g_OverlayAdapter.Shutdown();
        }
        // Initialize System Metrics
        IDXGIDevice* dxgiDevice = nullptr;

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
    if (!dx11_hook_g_mainRenderTargetView10 || s_LastDX10OverlaySwapChain != pSwapChain ||
        s_LastDX10OverlayBufferIndex != bufferIndex) {
        if (dx11_hook_g_mainRenderTargetView10) {
            dx11_hook_g_mainRenderTargetView10->Release();
            dx11_hook_g_mainRenderTargetView10 = nullptr;
        }

        ID3D10Texture2D* backbuffer = nullptr;
        HRESULT hr = pSwapChain->GetBuffer(bufferIndex, IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr) && backbuffer) {
            hr = device->CreateRenderTargetView(backbuffer, nullptr, &dx11_hook_g_mainRenderTargetView10);
            backbuffer->Release();
        }
        if (FAILED(hr) || !dx11_hook_g_mainRenderTargetView10) {
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
                ID3D10RenderTargetView* overlayRTV = dx11_hook_g_mainRenderTargetView10;
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
        dx11_hook_g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);
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
    dx11_hook_g_DetectedAPI = GetDX11HookOverlayAPIName(swapChainApi);

    if (frameCount % 60 == 0) {
        EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", frameCount, pSwapChain, currentHwnd,
                 desc.BufferDesc.Width, desc.BufferDesc.Height);
    }

    // Acquire device/context — use cached (AddRef'd) pointers for subsequent
    // frames. Calling pSwapChain->GetDevice() every frame is unsafe during
    // shutdown (the swapchain's internal device ref can be freed before our
    // Present hook stops being called).
    if (!dx11_hook_g_pd3dDevice) {
        // First frame: cache the DX11 device with AddRef.
        ID3D11Device* device11 = NULL;
        HRESULT hrDevice = pSwapChain->GetDevice(IID_PPV_ARGS(&device11));
        if (FAILED(hrDevice) || !device11) {
            EarlyLog("%s: FAILED to get D3D11 device (hr=0x%08X)", dx11_hook_g_DetectedAPI, hrDevice);
            return;
        }
        EarlyLog("%s: Identified as D3D11 device %p", dx11_hook_g_DetectedAPI, device11);

        // Initialize System Metrics (one-time)
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    SystemMetricsCollector::Get().Initialize(adapterDesc.AdapterLuid.LowPart,
                                                             adapterDesc.AdapterLuid.HighPart);
                }
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Cache device — GetDevice already AddRef'd, so we keep that ref
        dx11_hook_g_pd3dDevice = device11;
        // Cache context with AddRef
        dx11_hook_g_pd3dDevice->GetImmediateContext(&dx11_hook_g_pd3dDeviceContext);
    }

    ID3D11Device* device = dx11_hook_g_pd3dDevice;
    ID3D11DeviceContext* context = dx11_hook_g_pd3dDeviceContext;
    if (dx11_hook_g_D3D11IdentitySwapChain != pSwapChain) {
        ID3D11Device* identityDevice = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&identityDevice))) && identityDevice) {
            if (dx11_hook_g_D3D11IdentityDevice)
                dx11_hook_g_D3D11IdentityDevice->Release();
            dx11_hook_g_D3D11IdentityDevice = identityDevice;
            dx11_hook_g_D3D11IdentitySwapChain = pSwapChain;
        }
    }
    ID3D11Device* activeIdentityDevice =
        (dx11_hook_g_D3D11IdentitySwapChain == pSwapChain && dx11_hook_g_D3D11IdentityDevice) ? dx11_hook_g_D3D11IdentityDevice : device;
    DX11Hook_RegisterDeviceIdentity(activeIdentityDevice, "active D3D11 swapchain device");
    const std::string apiLabel =
        ce::graphics_api_identity::D3D11Label(ResolveD3D11MinorUse(activeIdentityDevice), IsDXVKD3D10OrD3D11Loaded());

    if (g_OverlayAdapter.IsInitialized() && currentHwnd != dx11_hook_g_CachedHwnd) {
        HookLog("DX11: HWND changed, shutting down OverlayAdapter");
        g_OverlayAdapter.Shutdown();
    }

    if (!g_OverlayAdapter.IsInitialized() || currentHwnd != dx11_hook_g_CachedHwnd) {
        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }
        dx11_hook_g_CachedHwnd = currentHwnd;
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
    g_OverlayAdapter.SetDroppedFrames(dx11_hook_g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
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
        if (!dx11_hook_g_mainRenderTargetView || pSwapChain != lastSwapChain || resolvedBufferIndex != lastBufferIndex) {
            if (dx11_hook_g_mainRenderTargetView) {
                dx11_hook_g_mainRenderTargetView->Release();
                dx11_hook_g_mainRenderTargetView = nullptr;
            }

            EarlyLog("%s: Creating RTV for SwapChain %p (%ux%u) buffer=%u...", dx11_hook_g_DetectedAPI, pSwapChain,
                     desc.BufferDesc.Width, desc.BufferDesc.Height, resolvedBufferIndex);

            ID3D11Texture2D* backbuffer = nullptr;
            HRESULT hr = pSwapChain->GetBuffer(resolvedBufferIndex, IID_PPV_ARGS(&backbuffer));
            if (FAILED(hr) || !backbuffer) {
                EarlyLog("%s: GetBuffer(%u) FAILED hr=0x%08X", dx11_hook_g_DetectedAPI, resolvedBufferIndex, hr);
                return;
            }
            hr = device->CreateRenderTargetView(backbuffer, NULL, &dx11_hook_g_mainRenderTargetView);
            backbuffer->Release();
            if (FAILED(hr)) {
                EarlyLog("%s: CreateRTV FAILED hr=0x%08X", dx11_hook_g_DetectedAPI, hr);
                return;
            }
            lastSwapChain = pSwapChain;
            lastBufferIndex = resolvedBufferIndex;
            EarlyLog("%s: RTV created OK (buffer=%u)", dx11_hook_g_DetectedAPI, resolvedBufferIndex);
        }

        overlayRTV = dx11_hook_g_mainRenderTargetView;
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
    dx11_hook_g_InOverlayRender = true;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);
    dx11_hook_g_InOverlayRender = false;

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
