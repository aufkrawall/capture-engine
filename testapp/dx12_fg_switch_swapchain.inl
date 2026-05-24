// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.

static void ReleaseSwapChainResources() {
    g_FrameLatencyWaitHandle = nullptr;
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_SwapChainBufferCount = kRequestedBackBuffers;
}

static bool CreateSwapChainResources(HWND hwnd, bool useFfxSwapChain, const char* reason) {
    ComPtr<IDXGIFactory4> factory;
    PFun_CreateDXGIFactory1 createFactory = g_SlCreateDXGIFactory1 ? g_SlCreateDXGIFactory1 : CreateDXGIFactory1;
    HRESULT factoryHr = createFactory(IID_PPV_ARGS(&factory));
    testapp::Log("[FG-DIAG] %s CreateDXGIFactory1(%s) hr=0x%08lx factory=%p\n",
                 g_SlCreateDXGIFactory1 ? "Streamline" : "Native", reason ? reason : "swapchain",
                 static_cast<unsigned long>(factoryHr), factory.Get());
    if (FAILED(factoryHr) || !factory) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kRequestedBackBuffers;
    swapChainDesc.Width = g_WindowWidth;
    swapChainDesc.Height = g_WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    bool usingFfxSwapChain = false;
    if (useFfxSwapChain && g_FfxCreateContext) {
        usingFfxSwapChain = CreateFSRSwapChainForHwndContext(factory.Get(), hwnd, swapChainDesc);
    }
    if (!usingFfxSwapChain) {
        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr =
            factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(hr) || !swapChain1 || FAILED(swapChain1.As(&g_SwapChain))) {
            testapp::Log("[FG-DIAG] Native CreateSwapChainForHwnd(%s) failed hr=0x%08lx\n",
                         reason ? reason : "swapchain", static_cast<unsigned long>(hr));
            return false;
        }
        testapp::Log("[FG-DIAG] Native DXGI swapchain created for %s\n", reason ? reason : "swapchain");
    }

    g_SwapChainOwner = usingFfxSwapChain ? SwapChainOwner::FSR : SwapChainOwner::Native;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    DXGI_SWAP_CHAIN_DESC fullDesc = {};
    if (SUCCEEDED(g_SwapChain->GetDesc(&fullDesc)) && fullDesc.BufferCount > 0) {
        g_SwapChainBufferCount = fullDesc.BufferCount;
        if (g_SwapChainBufferCount > kMaxSwapChainBuffers) {
            testapp::Log("[FG-DIAG] WARN swapchain buffer count %u exceeds max %d; clamping\n", g_SwapChainBufferCount,
                         kMaxSwapChainBuffers);
            g_SwapChainBufferCount = kMaxSwapChainBuffers;
        }
    }

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(g_SwapChain.As(&swapChain2)) && swapChain2) {
        g_MaxFrameLatency = g_SwapChainBufferCount;
        if (g_MaxFrameLatency > 3) {
            g_MaxFrameLatency = 3;
        }
        if (g_MaxFrameLatency < 1) {
            g_MaxFrameLatency = 1;
        }
        HRESULT latencyHr = swapChain2->SetMaximumFrameLatency(g_MaxFrameLatency);
        g_FrameLatencyWaitHandle = swapChain2->GetFrameLatencyWaitableObject();
        testapp::Log("[FG-DIAG] SetMaximumFrameLatency owner=%s value=%u hr=0x%08lx waitable=%d\n",
                     SwapChainOwnerName(g_SwapChainOwner), g_MaxFrameLatency, static_cast<unsigned long>(latencyHr),
                     g_FrameLatencyWaitHandle ? 1 : 0);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = g_SwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HRESULT heapHr = g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    if (FAILED(heapHr) || !g_RtvHeap) {
        testapp::Log("[FG-DIAG] CreateDescriptorHeap for swapchain RTVs failed hr=0x%08lx\n",
                     static_cast<unsigned long>(heapHr));
        return false;
    }

    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_SwapChainBufferCount; i++) {
        HRESULT bufferHr = g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        if (FAILED(bufferHr) || !g_RenderTargets[i]) {
            testapp::Log("[FG-DIAG] GetBuffer(%u) failed hr=0x%08lx\n", i, static_cast<unsigned long>(bufferHr));
            return false;
        }
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_RtvDescriptorSize;
        if (!g_CommandAllocators[i]) {
            HRESULT allocHr =
                g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
            if (FAILED(allocHr) || !g_CommandAllocators[i]) {
                testapp::Log("[FG-DIAG] CreateCommandAllocator(%u) failed hr=0x%08lx\n", i,
                             static_cast<unsigned long>(allocHr));
                return false;
            }
            g_FenceValues[i] = 0;
        }
    }

    testapp::Log(
        "[FG-DIAG] Swapchain ready: reason=%s owner=%s %dx%d buffers=%u requested=%d maxLatency=%u waitable=%d "
        "format=RGBA8 vsync=%d fullscreen=%d\n",
        reason ? reason : "swapchain", SwapChainOwnerName(g_SwapChainOwner), g_WindowWidth, g_WindowHeight,
        g_SwapChainBufferCount, kRequestedBackBuffers, g_MaxFrameLatency, g_FrameLatencyWaitHandle ? 1 : 0, g_VSync,
        g_Fullscreen);
    testapp::LogFlush();
    return true;
}

static bool RecreateSwapChain(bool useFfxSwapChain, const char* reason) {
    WaitForGpu();
    ReleaseSwapChainResources();
    bool ok = CreateSwapChainResources(g_Hwnd, useFfxSwapChain, reason);
    if (ok && g_Fence) {
        const UINT64 nextFenceValue = g_Fence->GetCompletedValue() + 1;
        for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
            g_FenceValues[i] = nextFenceValue;
        }
        testapp::Log("[FG-DIAG] Reset swapchain fence timeline after recreation: nextFence=%llu frameIndex=%u\n",
                     static_cast<unsigned long long>(nextFenceValue), g_FrameIndex);
    }
    UpdateWindowTitle();
    return ok;
}

static bool InitDX12(HWND hwnd) {
    PFun_D3D12CreateDevice createDevice = g_SlD3D12CreateDevice ? g_SlD3D12CreateDevice : D3D12CreateDevice;
    HRESULT deviceHr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device));
    testapp::Log("[FG-DIAG] %s D3D12CreateDevice hr=0x%08lx device=%p\n",
                 g_SlD3D12CreateDevice ? "Streamline" : "Native", static_cast<unsigned long>(deviceHr), g_Device.Get());
    if (FAILED(deviceHr) || !g_Device) {
        return false;
    }
    InitDxgiVideoMemoryQueryStressAdapter("initial device");
    if (g_SlSetD3DDevice && g_SlInitialized) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = deviceResult == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slSetD3DDevice(before swapchain) result=%d (%s)\n", static_cast<int>(deviceResult),
                     SlResultName(deviceResult));
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT queueHr = g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue));
    if (FAILED(queueHr) || !g_CommandQueue) {
        testapp::Log("[FG-DIAG] CreateCommandQueue failed hr=0x%08lx\n", static_cast<unsigned long>(queueHr));
        return false;
    }

    if (!CreateSwapChainResources(hwnd, false, "initial native")) {
        return false;
    }
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    g_FenceValues[g_FrameIndex]++;
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    testapp::dx12fg::CreateAuxiliaryResources(g_Device.Get(), static_cast<UINT>(g_WindowWidth),
                                              static_cast<UINT>(g_WindowHeight), g_FgInputs);
    return true;
}
