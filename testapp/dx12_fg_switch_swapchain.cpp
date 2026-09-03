#include "dx12_fg_switch_test_internal.h"

static const wchar_t* kBootstrapNativeSwapchainWindowClass = L"CaptureTestDX12FGSwitchBootstrap";

LRESULT CALLBACK BootstrapNativeSwapchainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

HWND CreateBootstrapNativeSwapchainWindow(int index) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = BootstrapNativeSwapchainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kBootstrapNativeSwapchainWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        testapp::Log("[FG-DIAG] Bootstrap native helper RegisterClassEx failed gle=%lu\n", GetLastError());
        return nullptr;
    }

    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kBootstrapNativeSwapchainWindowClass,
                                L"DX12 FG Switch Bootstrap", WS_POPUP, monitorRect.left + 24 + index * 12,
                                monitorRect.top + 24 + index * 12, 64, 64, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        testapp::Log("[FG-DIAG] Bootstrap native helper CreateWindowEx failed gle=%lu\n", GetLastError());
        return nullptr;
    }
    return hwnd;
}

void DestroyBootstrapNativeSwapchainWindow(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    DestroyWindow(hwnd);
    MSG msg = {};
    for (int i = 0; i < 16 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void RunBootstrapNativeSwapchainStress() {
    if (dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount <= 0) {
        return;
    }

    for (int i = 0; i < dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount; ++i) {
        testapp::Log("[FG-DIAG] Bootstrap native swapchain wrapper stress %d/%d begin\n", i + 1,
                     dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount);

        ComPtr<ID3D12Device> device;
        HRESULT deviceHr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        testapp::Log("[FG-DIAG] Bootstrap native D3D12CreateDevice hr=0x%08lx device=%p\n",
                     static_cast<unsigned long>(deviceHr), device.Get());
        if (FAILED(deviceHr) || !device) {
            continue;
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        HRESULT queueHr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
        testapp::Log("[FG-DIAG] Bootstrap native CreateCommandQueue hr=0x%08lx queue=%p\n",
                     static_cast<unsigned long>(queueHr), queue.Get());
        if (FAILED(queueHr) || !queue) {
            continue;
        }

        ComPtr<IDXGIFactory4> factory;
        HRESULT factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        testapp::Log("[FG-DIAG] Bootstrap native CreateDXGIFactory1 hr=0x%08lx factory=%p\n",
                     static_cast<unsigned long>(factoryHr), factory.Get());
        if (FAILED(factoryHr) || !factory) {
            continue;
        }

        HWND probeHwnd = CreateBootstrapNativeSwapchainWindow(i);
        if (!probeHwnd) {
            testapp::Log("[FG-DIAG] Bootstrap native helper window unavailable; skipping probe %d/%d\n", i + 1,
                         dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount);
            continue;
        }

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.BufferCount = dx12_fg_switch_test_kRequestedBackBuffers;
        desc.Width = dx12_fg_switch_test_g_WindowWidth;
        desc.Height = dx12_fg_switch_test_g_WindowHeight;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.SampleDesc.Count = 1;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (CheckPresentAllowTearingSupport(factory.Get())) {
            desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT swapHr = factory->CreateSwapChainForHwnd(queue.Get(), probeHwnd, &desc, nullptr, nullptr, &swapChain1);
        testapp::Log("[FG-DIAG] Bootstrap native CreateSwapChainForHwnd hr=0x%08lx swapChain=%p\n",
                     static_cast<unsigned long>(swapHr), swapChain1.Get());
        if (FAILED(swapHr) || !swapChain1) {
            DestroyBootstrapNativeSwapchainWindow(probeHwnd);
            continue;
        }

        factory->MakeWindowAssociation(probeHwnd, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(swapChain1.As(&swapChain2)) && swapChain2) {
            HRESULT latencyHr = swapChain2->SetMaximumFrameLatency(dx12_fg_switch_test_kRequestedBackBuffers);
            HANDLE waitable = swapChain2->GetFrameLatencyWaitableObject();
            testapp::Log("[FG-DIAG] Bootstrap native SetMaximumFrameLatency hr=0x%08lx waitable=%d\n",
                         static_cast<unsigned long>(latencyHr), waitable ? 1 : 0);
        }

        ComPtr<IDXGISwapChain3> swapChain3;
        HRESULT qiHr = swapChain1.As(&swapChain3);
        testapp::Log("[FG-DIAG] Bootstrap native QI IDXGISwapChain3 hr=0x%08lx sc3=%p\n",
                     static_cast<unsigned long>(qiHr), swapChain3.Get());

        HRESULT presentHr = swapChain1->Present(dx12_fg_switch_test_g_VSync, 0);
        testapp::Log("[FG-DIAG] Bootstrap native Present hr=0x%08lx\n", static_cast<unsigned long>(presentHr));

        swapChain3.Reset();
        swapChain2.Reset();
        swapChain1.Reset();
        factory.Reset();
        queue.Reset();
        device.Reset();
        DestroyBootstrapNativeSwapchainWindow(probeHwnd);
        testapp::Log("[FG-DIAG] Bootstrap native swapchain wrapper stress %d/%d released all local refs\n", i + 1,
                     dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount);
        testapp::LogFlush();
    }
}

void ReleaseSwapChainResources() {
    g_FrameLatencyWaitHandle = nullptr;
    dx12_fg_switch_test_g_SwapChainUsesStreamline = false;
    dx12_fg_switch_test_g_CurrentSwapChainAllowTearing = false;
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_SwapChainBufferCount = dx12_fg_switch_test_kRequestedBackBuffers;
}

bool CheckPresentAllowTearingSupport(IDXGIFactory4* factory) {
    if (!factory) {
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                sizeof(allowTearing)))) {
        return allowTearing != FALSE;
    }
    return false;
}

bool CreateSwapChainResources(HWND hwnd, bool useFfxSwapChain, const char* reason) {
    ComPtr<IDXGIFactory4> factory;
    PFun_CreateDXGIFactory1 createFactory =
        (!useFfxSwapChain && dx12_fg_switch_test_g_SlCreateDXGIFactory1) ? dx12_fg_switch_test_g_SlCreateDXGIFactory1 : CreateDXGIFactory1;
    const bool usingStreamlineFactory = createFactory == dx12_fg_switch_test_g_SlCreateDXGIFactory1 && dx12_fg_switch_test_g_SlCreateDXGIFactory1 != nullptr;
    HRESULT factoryHr = createFactory(IID_PPV_ARGS(&factory));
    testapp::Log("[FG-DIAG] %s CreateDXGIFactory1(%s) hr=0x%08lx factory=%p useFfx=%d\n",
                 usingStreamlineFactory ? "Streamline" : "Native", reason ? reason : "swapchain",
                 static_cast<unsigned long>(factoryHr), factory.Get(), useFfxSwapChain ? 1 : 0);
    if (FAILED(factoryHr) || !factory) {
        return false;
    }
    dx12_fg_switch_test_g_TearingSupported = CheckPresentAllowTearingSupport(factory.Get());

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = dx12_fg_switch_test_kRequestedBackBuffers;
    swapChainDesc.Width = dx12_fg_switch_test_g_WindowWidth;
    swapChainDesc.Height = dx12_fg_switch_test_g_WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (dx12_fg_switch_test_g_TearingSupported) {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    bool usingFfxSwapChain = false;
    if (useFfxSwapChain && dx12_fg_switch_test_g_FfxCreateContext) {
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

    dx12_fg_switch_test_g_SwapChainOwner = usingFfxSwapChain ? SwapChainOwner::FSR : SwapChainOwner::Native;
    dx12_fg_switch_test_g_SwapChainUsesStreamline = usingStreamlineFactory;
    dx12_fg_switch_test_g_CurrentSwapChainAllowTearing = (swapChainDesc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    DXGI_SWAP_CHAIN_DESC fullDesc = {};
    if (SUCCEEDED(g_SwapChain->GetDesc(&fullDesc)) && fullDesc.BufferCount > 0) {
        g_SwapChainBufferCount = fullDesc.BufferCount;
        if (g_SwapChainBufferCount > dx12_fg_switch_test_kMaxSwapChainBuffers) {
            testapp::Log("[FG-DIAG] WARN swapchain buffer count %u exceeds max %d; clamping\n", g_SwapChainBufferCount,
                         dx12_fg_switch_test_kMaxSwapChainBuffers);
            g_SwapChainBufferCount = dx12_fg_switch_test_kMaxSwapChainBuffers;
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
                     SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner), g_MaxFrameLatency, static_cast<unsigned long>(latencyHr),
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
        "format=RGBA8 vsync=%d fullscreen=%d streamline=%d tearing=%d flags=0x%x\n",
        reason ? reason : "swapchain", SwapChainOwnerName(dx12_fg_switch_test_g_SwapChainOwner), dx12_fg_switch_test_g_WindowWidth, dx12_fg_switch_test_g_WindowHeight,
        g_SwapChainBufferCount, dx12_fg_switch_test_kRequestedBackBuffers, g_MaxFrameLatency, g_FrameLatencyWaitHandle ? 1 : 0, dx12_fg_switch_test_g_VSync,
        dx12_fg_switch_test_g_Fullscreen, dx12_fg_switch_test_g_SwapChainUsesStreamline ? 1 : 0, dx12_fg_switch_test_g_CurrentSwapChainAllowTearing ? 1 : 0,
        swapChainDesc.Flags);
    testapp::LogFlush();
    return true;
}

bool RecreateSwapChain(bool useFfxSwapChain, const char* reason) {
    WaitForGpu();
    ReleaseSwapChainResources();
    bool ok = CreateSwapChainResources(dx12_fg_switch_test_g_Hwnd, useFfxSwapChain, reason);
    if (ok && g_Fence) {
        const UINT64 nextFenceValue = g_Fence->GetCompletedValue() + 1;
        for (UINT i = 0; i < dx12_fg_switch_test_kMaxSwapChainBuffers; ++i) {
            g_FenceValues[i] = nextFenceValue;
        }
        testapp::Log("[FG-DIAG] Reset swapchain fence timeline after recreation: nextFence=%llu frameIndex=%u\n",
                     static_cast<unsigned long long>(nextFenceValue), g_FrameIndex);
    }
    UpdateWindowTitle();
    return ok;
}

bool InitDX12(HWND hwnd, bool useFfxSwapChain , const char* reason ) {
    PFun_D3D12CreateDevice createDevice = dx12_fg_switch_test_g_SlD3D12CreateDevice ? dx12_fg_switch_test_g_SlD3D12CreateDevice : D3D12CreateDevice;
    HRESULT deviceHr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device));
    testapp::Log("[FG-DIAG] %s D3D12CreateDevice hr=0x%08lx device=%p\n",
                 dx12_fg_switch_test_g_SlD3D12CreateDevice ? "Streamline" : "Native", static_cast<unsigned long>(deviceHr), g_Device.Get());
    if (FAILED(deviceHr) || !g_Device) {
        return false;
    }
    InitDxgiVideoMemoryQueryStressAdapter("initial device");
    if (dx12_fg_switch_test_g_SlSetD3DDevice && dx12_fg_switch_test_g_SlInitialized) {
        sl::Result deviceResult = dx12_fg_switch_test_g_SlSetD3DDevice(g_Device.Get());
        dx12_fg_switch_test_g_SlDeviceSet = deviceResult == sl::Result::eOk;
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

    if (!CreateSwapChainResources(hwnd, useFfxSwapChain, reason)) {
        return false;
    }
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    g_FenceValues[g_FrameIndex]++;
    if (!g_GpuFrameTimer.Init(g_Device.Get(), g_CommandQueue.Get())) {
        testapp::Log("[FG-DIAG] WARN GPU frame timer unavailable; per-frame GPU duration will not be reported (%s)\n",
                     reason ? reason : "init");
    }
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    // The scene -> upscaler -> hudless chain runs in FP16 linear (band-free temporal accumulation);
    // the present blit dithers down to the 8-bit backbuffer.
    testapp::dx12fg::CreateAuxiliaryResources(g_Device.Get(), static_cast<UINT>(dx12_fg_switch_test_g_WindowWidth),
                                              static_cast<UINT>(dx12_fg_switch_test_g_WindowHeight), g_FgInputs,
                                              static_cast<UINT>(dx12_fg_switch_test_g_RenderWidth), static_cast<UINT>(dx12_fg_switch_test_g_RenderHeight),
                                              testapp::dx12fg::kHdrColorFormat);
    if (!g_Scene.Initialize(g_Device.Get(), testapp::dx12fg::kHdrColorFormat)) {
        testapp::Log("[FG-DIAG] WARN SceneRenderer init failed; scene will fall back to a flat clear (%s)\n",
                     reason ? reason : "init");
    }
    if (UpscalingActive() && !dx12_fg_switch_test_g_Taa.Initialize(g_Device.Get(), testapp::dx12fg::kHdrColorFormat)) {
        testapp::Log("[FG-DIAG] WARN TemporalUpscaler init failed; OFF-mode/fallback output would stay black (%s)\n",
                     reason ? reason : "init");
    }
    if (!dx12_fg_switch_test_g_PresentBlit.Initialize(g_Device.Get(), testapp::dx12fg::kHdrColorFormat)) {
        testapp::Log("[FG-DIAG] WARN PresentBlitPass init failed; backbuffer would stay black (%s)\n",
                     reason ? reason : "init");
    }
    return true;
}
