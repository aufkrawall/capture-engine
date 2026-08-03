    (void)pSwapChain;
}

// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)
static void HookSwapchainVTableViaTempSwapchain(bool presentOnly) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hDXGI || !hD3D12)
        return;

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pCreateFactory || !pD3D12CreateDevice)
        return;

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
        return;

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
        pFactory->Release();
        return;
    }

    // Hook CreateSampler on the device vtable
    // All D3D12 devices share the same vtable, so this hooks ALL devices
    DX12_HookDeviceVTable(pDevice);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
        pDevice->Release();
        pFactory->Release();
        return;
    }

    // Create a minimal hidden window
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CE_Temp";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

    // Create temp swapchain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = 2;
    scd.Height = 2;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Mark that we're creating a temp swapchain for hook installation.
    // This prevents the CreateSwapChainForHwnd hooks from capturing the temp
    // queue as g_SwapchainQueue or tracking the temp swapchain.
    g_CreatingTempSwapchain.store(true, std::memory_order_release);

    IDXGISwapChain1* pSwapChain = nullptr;
    HRESULT hr = E_FAIL;

    // CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
    // swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
    // wrapper If the original is not available, skip vtable hook installation
    if (oCreateSwapChainForHwndGlobal) {
        // Call original directly - bypasses our wrapper
        hr = oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            HookLog(
                "DX12: Created temp swapchain via original "
                "CreateSwapChainForHwnd (unwrapped)");
        }
    } else {
        HookLog(
            "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
            "Present vtable hooks");
    }

    g_CreatingTempSwapchain.store(false, std::memory_order_release);

    if (SUCCEEDED(hr) && pSwapChain) {
        HookLog("DX12: Installing Present inline hooks via temp swapchain");
        if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
            HookLog("DX12: Present inline hooks installed successfully");
        } else {
            HookLog("DX12: Failed to install Present inline hooks");
        }
        pSwapChain->Release();
    } else {
        HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
    }

    // Hook ExecuteCommandLists on the temp queue's vtable.
    // All DX12 command queues share the same vtable, so this hooks ALL queues
    // (including the game's pre-existing queue). When ECL fires, it calls
    // DX12_SetCommandQueue which captures the game's actual queue pointer.
    DX12_HookQueueVTable(pQueue);

    // Cleanup
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(L"CE_Temp", wc.hInstance);
    pQueue->Release();
    pDevice->Release();
    pFactory->Release();
}

void ShutdownImGui() {
    if (!g_State.overlayInit)
        return;

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
        g_OverlayAdapterBackendDevice.store(nullptr, std::memory_order_release);
        g_OverlayAdapterBackendQueue.store(nullptr, std::memory_order_release);
        g_OverlayAdapterBackendFormat.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_release);
        g_PreserveOverlayAdapterAcrossResize.store(false, std::memory_order_release);
    }

    if (g_State.srvDescHeap) {
        g_State.srvDescHeap->Release();
        g_State.srvDescHeap = nullptr;
    }
    g_State.overlayInit = false;
}

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    if (g_State.overlayInit) {
        HookLog("InitImGui: Already initialized, returning early");
        return true;
    }

    HookLog(
        "InitImGui: Proceeding with initialization - buffers=%d, format=%d, "
        "hwnd=%p",
        buffers, format, hwnd);

    g_State.format = format;

    // Use OverlayAdapter instead of ImGui
    // Rendering always goes through the game queue since GPU drivers require
    // swapchain writes from the owning queue. The overlay queue handles fence
    // management independently.
    // CRITICAL: Use same queue preference as ProcessFrame.
    // SL FG: use origGame (SL manages cross-queue internally).
    // FSR FG: use g_SwapchainQueue (FSR's swapchain uses FSR's queue;
    //   submitting on origGame causes cross-queue DEVICE_REMOVED).
    ID3D12CommandQueue* queueForBackend = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool postFSRInactiveRecoveryPending =
            g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);

        const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
            g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
            g_OriginalGameQueue != nullptr, g_PostSLLastWorkingQueue != nullptr, postFSRInactiveRecoveryPending,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
            g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
            g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire));

        if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
            // After FSR→DLSS: use scQueue (swapchain creation queue)
            queueForBackend = g_SwapchainQueue ? g_SwapchainQueue : g_OriginalGameQueue;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
            // During SL FG, prefer scQueue when it differs from origGame.
            // SL may recreate the swapchain on its own internal queue.
            if (g_SwapchainQueue && g_SwapchainQueue != g_OriginalGameQueue) {
                queueForBackend = g_SwapchainQueue;
            } else if (g_OriginalGameQueue) {
                queueForBackend = g_OriginalGameQueue;
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
            queueForBackend = g_PostSLLastWorkingQueue;
            static std::atomic<int> s_postFSRBackendLastWorkingRouteLogCount{0};
            int logCount = s_postFSRBackendLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: InitImGui — post-FSR inactive recovery epoch using preserved PostSL lastWorking queue %p "
                    "(cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                    g_PostSLLastWorkingQueue, currentCommandQueue, g_OriginalGameQueue, currentPrimaryQueue,
                    lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
            const auto queueSource =
                ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource(g_OriginalGameQueue != nullptr);
            if (queueSource == ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue) {
                queueForBackend = g_OriginalGameQueue;
                const bool explicitNativeFSROffPending =
                    g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
                static std::atomic<int> s_postFSRBackendOrigRouteLogCount{0};
                int logCount = s_postFSRBackendOrigRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — post-FSR normal/recovery routing using original present queue %p "
                        "(cmdQ=%p primaryQ=%p recoveryPending=%d explicitNativeOff=%d)",
                        queueForBackend, currentCommandQueue, currentPrimaryQueue,
                        postFSRInactiveRecoveryPending ? 1 : 0, explicitNativeFSROffPending ? 1 : 0);
                }
            } else {
                queueForBackend = currentCommandQueue ? currentCommandQueue : currentPrimaryQueue;
                static std::atomic<int> s_postFSRBackendFallbackRouteLogCount{0};
                int logCount = s_postFSRBackendFallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: InitImGui — post-FSR inactive recovery missing origGame, falling back to current "
                        "command queue %p (primaryQ=%p)",
                        queueForBackend, currentPrimaryQueue);
                }
            }
        } else if (routingDecision == ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue ||
                   routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
            queueForBackend = g_SwapchainQueue;
        } else if (fsrFGNow && g_OriginalGameQueue) {
            queueForBackend = g_OriginalGameQueue;  // fallback
        } else {
            queueForBackend = g_SwapchainQueue;
            if (!queueForBackend)
                queueForBackend = g_CommandQueue.load();
        }
    }
    HookLogImportant(
        "[Overlay] DX12: InitImGui backend queue=%p (origQ=%p, primaryQ=%p, scQueue=%p, cmdQueue=%p, "
        "lastWorkingQ=%p, slFG=%d, fgCooldown=%d)",
        queueForBackend, g_OriginalGameQueue, g_PrimaryGameQueue.load(std::memory_order_acquire), g_SwapchainQueue,
        (void*)g_CommandQueue.load(), g_PostSLLastWorkingQueue,
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0,
        g_FGTransitionCooldown.load(std::memory_order_acquire));

    const bool preserveAcrossResize = g_PreserveOverlayAdapterAcrossResize.exchange(false, std::memory_order_acq_rel);
    // Warm reuse is device+format scoped: the backend never uses its bound
    // queue (see CanReuseWarmDX12OverlayBackend), so FG transitions that move
    // the swapchain to a different queue still reuse the warm PSOs/font atlas.
    const bool canReuseWarmResizeBackend = ce::dx12_overlay_policy::CanReuseWarmDX12OverlayBackend(
        preserveAcrossResize, g_OverlayAdapter.IsInitialized(),
        g_OverlayAdapterBackendDevice.load(std::memory_order_acquire) == device,
        g_OverlayAdapterBackendFormat.load(std::memory_order_acquire) == static_cast<int>(format));
    if (g_OverlayAdapter.IsInitialized() && !canReuseWarmResizeBackend) {
        HookLogImportant(
            "InitImGui: OverlayAdapter already initialized, shutting down for re-init "
            "(preserveResize=%d device old=%p new=%p queue old=%p new=%p fmt old=%d new=%d)",
            preserveAcrossResize ? 1 : 0, g_OverlayAdapterBackendDevice.load(std::memory_order_acquire), device,
            g_OverlayAdapterBackendQueue.load(std::memory_order_acquire), queueForBackend,
            g_OverlayAdapterBackendFormat.load(std::memory_order_acquire), static_cast<int>(format));
        g_OverlayAdapter.Shutdown();
        g_OverlayAdapterBackendDevice.store(nullptr, std::memory_order_release);
        g_OverlayAdapterBackendQueue.store(nullptr, std::memory_order_release);
        g_OverlayAdapterBackendFormat.store(static_cast<int>(DXGI_FORMAT_UNKNOWN), std::memory_order_release);
    } else if (canReuseWarmResizeBackend) {
        HookLogImportant(
            "InitImGui: Reusing warm DX12 overlay backend across swapchain/queue transition "
            "(device=%p queue old=%p new=%p fmt=%d)",
            device, g_OverlayAdapterBackendQueue.load(std::memory_order_acquire), queueForBackend,
            static_cast<int>(format));
    }

    g_OverlayAdapter.SetHwnd(hwnd);
    if (!g_OverlayAdapter.InitDX12(device, queueForBackend, format)) {
        HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 FAILED (device=%p, queue=%p, fmt=%d)", device,
                queueForBackend, format);
        return false;
    }

    // OverlayAdapter handles its own initialization
    HookLog("[Overlay] DX12: OverlayAdapter::InitDX12 succeeded (hwnd=%p)", hwnd);
    g_OverlayAdapterBackendDevice.store(device, std::memory_order_release);
    g_OverlayAdapterBackendQueue.store(queueForBackend, std::memory_order_release);
    g_OverlayAdapterBackendFormat.store(static_cast<int>(format), std::memory_order_release);

    InputManager::Get().HookWindow(hwnd);

    // We don't need SRV heap for ImGui anymore, OverlayAdapter manages its own
    // resources. But we might need it if we keep ImGui for menus? For now
    // assuming full replacement for overlay.

    g_State.overlayInit = true;

    // Reset frame delay counter on reinitialization
    extern void DX12_ResetOverlayFrameDelay();
    DX12_ResetOverlayFrameDelay();

    return true;
}

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx,
                 D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride) {
    // CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
    // shutdown/reinit
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    if (!g_State.overlayInit || !cmdList)
        return;

    static std::atomic<int> s_drawOverlayLogCount{0};
    const bool logThisDraw = s_drawOverlayLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
    if (logThisDraw) {
        HookLogImportant(
            "DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)", cmdList,
            bufferIdx, isRealFrame ? 1 : 0, g_State.overlayInit ? 1 : 0, g_State.syncInit ? 1 : 0);
    }

    // CRITICAL FIX: Always set IPC client regardless of frame type.
    // RenderOverlay() guards on ipc being non-null, so if this was only set
    // on real frames, overlay would never render when isRealFrame is false.
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }

    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_OverlayAdapter.SetGraphicsAPI(api);
        // HDR state is set during overlay init (ProcessFrame) by querying the
        // display output's actual color space — not here, to avoid the false
        // positive of R10G10B10A2_UNORM being treated as HDR in SDR mode.
    }

    // Set Render Target for Custom Overlay
    // When rtvOverride is set (offscreen compositing path), use it instead of the backbuffer RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    if (rtvOverride) {
        rtvHandle = *rtvOverride;
    } else {
        // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
        if (!g_State.rtvDescHeap) {
            HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
            return;
        }
        rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += bufferIdx * g_State.rtvDescriptorSize;
    }

    g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);
    g_OverlayAdapter.SetDX12UploadSlotFence(
        g_State.fence, ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                           DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) || g_FGCompat.IsFGActive(),
                           g_State.fence != nullptr, g_State.currentFenceValue));

    // Render overlay content
    g_OverlayAdapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
    if (logThisDraw) {
        HookLogImportant("DX12: DrawOverlay end (bufferIdx=%u)", bufferIdx);
    }
}

// Ensure offscreen render target exists and matches backbuffer dimensions/format.
// Used for the copy-render-copy overlay compositing path that avoids
// OMSetRenderTargets(swapchain) + SetDescriptorHeaps GPU pipeline stalls.
static bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (g_State.offscreenRT && g_State.offscreenWidth == width && g_State.offscreenHeight == height &&
        g_State.offscreenFormat == format) {
        return true;
    }

    // Release old resources if dimensions/format changed
    if (g_State.offscreenRT) {
        g_State.offscreenRT->Release();
        g_State.offscreenRT = nullptr;
    }
    if (g_State.offscreenRtvHeap) {
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
    }

    // Create RTV descriptor heap for offscreen target
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_State.offscreenRtvHeap));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RTV heap hr=0x%08X", hr);
        return false;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = width;
    resDesc.Height = height;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = format;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                         &clearVal, IID_PPV_ARGS(&g_State.offscreenRT));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RT %ux%u fmt=%d hr=0x%08X", width, height, format, hr);
        g_State.offscreenRtvHeap->Release();
        g_State.offscreenRtvHeap = nullptr;
        return false;
    }

    device->CreateRenderTargetView(g_State.offscreenRT, nullptr,
                                   g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());

    g_State.offscreenRT->SetName(L"CE_OverlayOffscreenRT");

    g_State.offscreenWidth = width;
    g_State.offscreenHeight = height;
    g_State.offscreenFormat = format;

    HookLogImportant("DX12: Created offscreen RT %ux%u fmt=%d for overlay compositing", width, height, format);
    return true;
}

// ---------------------------------------------------------------------------
// D3D11On12 overlay bridge: renders overlay via D3D11 wrapping the D3D12
// backbuffer.  D3D11 doesn't use descriptor heaps, so the NVIDIA driver
// stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain) in
// the same D3D12 ECL is completely avoided.  The D3D11on12 layer handles
// all resource state transitions internally.
// ---------------------------------------------------------------------------

static bool InitD3D11On12(ID3D12Device* d3d12Dev, ID3D12CommandQueue* queue, IDXGISwapChain* swapChain,
                          UINT bufferCount) {
    if (g_State.d3d11on12Init)
        return true;

    HookLogImportant("DX12 D3D11On12: Initializing bridge (dev=%p queue=%p bufCnt=%u)", d3d12Dev, queue, bufferCount);

    if (!d3d12Dev || !queue || !swapChain || bufferCount == 0)
        return false;

    // Dynamically load D3D11On12CreateDevice
    HMODULE d3d11Lib = GetModuleHandleA("d3d11.dll");
    if (!d3d11Lib)
        d3d11Lib = ce::security::LoadSystemLibrary(L"d3d11.dll");
    if (!d3d11Lib) {
        HookLogImportant("DX12 D3D11On12: d3d11.dll not available");
        return false;
    }

    using PFN_D3D11On12 = decltype(&D3D11On12CreateDevice);
    auto pfnCreate = (PFN_D3D11On12)GetProcAddress(d3d11Lib, "D3D11On12CreateDevice");
    if (!pfnCreate) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice not found");
        return false;
    }

    // Create D3D11on12 device wrapping the game's D3D12 device + queue
    IUnknown* queues[] = {queue};
    ID3D11Device* d3d11Dev = nullptr;
    ID3D11DeviceContext* d3d11Ctx = nullptr;
    HRESULT hr =
        pfnCreate(d3d12Dev, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &d3d11Dev, &d3d11Ctx, nullptr);
    if (FAILED(hr) || !d3d11Dev) {
        HookLogImportant("DX12 D3D11On12: D3D11On12CreateDevice failed hr=0x%08X", hr);
        return false;
    }

    // Get the ID3D11On12Device interface for wrapping resources
    ID3D11On12Device* d3d11on12 = nullptr;
    hr = d3d11Dev->QueryInterface(IID_PPV_ARGS(&d3d11on12));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for ID3D11On12Device failed hr=0x%08X", hr);
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    HookLogImportant("DX12 D3D11On12: Bridge device created successfully");

    // Wrap each D3D12 backbuffer for D3D11 use
    IDXGISwapChain3* sc3 = nullptr;
    hr = swapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    if (FAILED(hr)) {
        HookLogImportant("DX12 D3D11On12: QI for IDXGISwapChain3 failed hr=0x%08X", hr);
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    std::vector<ID3D11Resource*> wrappedBBs;
    std::vector<ID3D11RenderTargetView*> rtvs;
    bool wrapOk = true;

    for (UINT i = 0; i < bufferCount && wrapOk; i++) {
        ID3D12Resource* d3d12BB = nullptr;
        hr = sc3->GetBuffer(i, IID_PPV_ARGS(&d3d12BB));
        if (FAILED(hr) || !d3d12BB) {
            HookLogImportant("DX12 D3D11On12: GetBuffer(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        D3D11_RESOURCE_FLAGS flags = {};
        flags.BindFlags = D3D11_BIND_RENDER_TARGET;

        ID3D11Resource* wrapped = nullptr;
        hr = d3d11on12->CreateWrappedResource(d3d12BB, &flags, D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_PRESENT, IID_PPV_ARGS(&wrapped));
        d3d12BB->Release();
        if (FAILED(hr) || !wrapped) {
            HookLogImportant("DX12 D3D11On12: CreateWrappedResource(%u) failed hr=0x%08X", i, hr);
            wrapOk = false;
            break;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        hr = d3d11Dev->CreateRenderTargetView(wrapped, nullptr, &rtv);
        if (FAILED(hr)) {
            HookLogImportant("DX12 D3D11On12: CreateRenderTargetView(%u) failed hr=0x%08X", i, hr);
            wrapped->Release();
            wrapOk = false;
            break;
        }

        wrappedBBs.push_back(wrapped);
        rtvs.push_back(rtv);
    }
    sc3->Release();

    if (!wrapOk) {
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Initialize the SL FG overlay adapter with the D3D11on12 device.
    // This creates the DX11Backend (shaders, font texture, blend states).
    if (!g_SLFGAdapter.InitDX11(d3d11Dev, d3d11Ctx)) {
        HookLogImportant("DX12 D3D11On12: OverlayAdapter.InitDX11 failed");
        for (auto* r : rtvs)
            if (r)
                r->Release();
        for (auto* w : wrappedBBs)
            if (w)
                w->Release();
        d3d11on12->Release();
        d3d11Ctx->Release();
        d3d11Dev->Release();
        return false;
    }

    // Store everything
    g_State.d3d11on12Device = d3d11Dev;
    g_State.d3d11on12Context = d3d11Ctx;
    g_State.d3d11on12 = d3d11on12;
    g_State.d3d11WrappedBBs = std::move(wrappedBBs);
    g_State.d3d11RTVs = std::move(rtvs);
    g_State.d3d11on12Init = true;

    HookLogImportant("DX12 D3D11On12: Initialized (%u buffers wrapped)", bufferCount);
    return true;
}

static bool RenderOverlayViaD3D11On12(int bufferIdx, bool isRealFrame) {
    if (!g_State.d3d11on12Init || !g_State.d3d11on12 || !g_State.d3d11on12Context)
        return false;

    if (bufferIdx < 0 || bufferIdx >= (int)g_State.d3d11WrappedBBs.size())
        return false;

    auto* wrapped = g_State.d3d11WrappedBBs[bufferIdx];
    auto* rtv = g_State.d3d11RTVs[bufferIdx];
    if (!wrapped || !rtv)
        return false;

    static uint64_t s_d3d11on12FrameCount = 0;
    s_d3d11on12FrameCount++;
    if (s_d3d11on12FrameCount <= 5 || (s_d3d11on12FrameCount % 300) == 0) {
        HookLogImportant("DX12 D3D11On12: RenderOverlay frame #%llu (bufIdx=%d)",
                         (unsigned long long)s_d3d11on12FrameCount, bufferIdx);
    }

    // Acquire: internally transitions backbuffer to RENDER_TARGET
    g_State.d3d11on12->AcquireWrappedResources(&wrapped, 1);

    // Set render target on D3D11 context
    g_State.d3d11on12Context->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)g_State.cachedWidth, (float)g_State.cachedHeight, 0.0f, 1.0f};
    g_State.d3d11on12Context->RSSetViewports(1, &vp);

    // Feed data to the SL FG overlay adapter
    g_SLFGAdapter.SetIPCClient(g_IPC);
    g_SLFGAdapter.SetReserveInactiveFGSpace(false);
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        g_SLFGAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }
    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_SLFGAdapter.SetGraphicsAPI(api);
    }

    // Render overlay via D3D11 backend (no descriptor heaps!)
    g_SLFGAdapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);

    // Release: internally transitions backbuffer back to PRESENT
    g_State.d3d11on12->ReleaseWrappedResources(&wrapped, 1);

    // Flush submits all D3D11 commands to the D3D12 queue
    g_State.d3d11on12Context->Flush();

    return true;
}

static void CleanupD3D11On12() {
    // Warm-backend: the x64 descriptor-free adapter is DEVICE-scoped (PSOs,
    // font buffer, vb/ib pool; backbuffer fetched per frame) and survives
    // swapchain teardown so the first present of the next swapchain can draw
    // without a backend rebuild. It is rebuilt only on device/format change
