// Extracted from dx12_fsr_fg_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer);

static void PreloadAmdCompanionDlls() {
    const wchar_t* companionDlls[] = {L"amd_ags_x64.dll", L"amd_acs_x64.dll"};
    for (const wchar_t* dllName : companionDlls) {
        HMODULE companion = LoadLibraryW(dllName);
        if (companion) {
            testapp::Log("  Preloaded AMD companion: %S\n", dllName);
        } else {
            testapp::Log("  Failed to preload AMD companion %S (err=%lu)\n", dllName, GetLastError());
        }
    }
}

static FsrInitResult LoadFSRRuntime() {
    if (g_FfxModule)
        return kFsrOk;

    PreloadAmdCompanionDlls();

    // Prefer the per-effect FG DLL directly (the loader DLL's ffxCreateContext
    // delegates to this, but going direct avoids any loader-side issues).
    const wchar_t* dllNames[] = {
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_loader_dx12.dll",
        L"amd_fidelityfx_dx12.dll",
        L"ffx_framegeneration.dll",
    };
    for (auto dllName : dllNames) {
        g_FfxModule = LoadLibraryW(dllName);
        if (g_FfxModule) {
            testapp::Log("  Loaded FSR runtime: %S\n", dllName);
            break;
        }
    }
    if (!g_FfxModule)
        return kFsrNoDll;

    g_FfxCreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_FfxModule, "ffxCreateContext"));
    g_FfxConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_FfxModule, "ffxConfigure"));
    g_FfxDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_FfxModule, "ffxDispatch"));
    g_FfxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_FfxModule, "ffxDestroyContext"));
    if (!g_FfxCreateContext || !g_FfxConfigure || !g_FfxDispatch || !g_FfxDestroyContext) {
        testapp::Log("  FSR DLL missing ffxCreateContext/ffxConfigure/ffxDispatch/ffxDestroyContext exports\n");
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
        return kFsrNoExports;
    }
    return kFsrOk;
}

static bool CreateFSRSwapChainForHwndContext(IDXGIFactory4* factory, HWND hwnd, DXGI_SWAP_CHAIN_DESC1& swapChainDesc) {
    if (!g_FfxCreateContext || !factory || !hwnd || !g_CommandQueue) {
        return false;
    }

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 versionDesc = {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    versionDesc.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc = {};
    fullscreenDesc.Windowed = TRUE;

    IDXGISwapChain4* ffxSwapChain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    createDesc.header.pNext = &versionDesc.header;
    createDesc.swapchain = &ffxSwapChain;
    createDesc.hwnd = hwnd;
    createDesc.desc = &swapChainDesc;
    createDesc.fullscreenDesc = &fullscreenDesc;
    createDesc.dxgiFactory = factory;
    createDesc.gameQueue = g_CommandQueue.Get();

    ffxReturnCode_t ret = g_FfxCreateContext(&g_FfxSwapChainCtx, &createDesc.header, nullptr);
    if (ret != FFX_API_RETURN_OK || !g_FfxSwapChainCtx || !ffxSwapChain) {
        testapp::Log(
            "[FG-DIAG] ffxCreateContext(FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12) FAILED code=%u (%s) ctx=%p swap=%p "
            "version=0x%08x flags=0x%x\n",
            ret, FfxReturnName(ret), (void*)g_FfxSwapChainCtx, ffxSwapChain, FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION,
            swapChainDesc.Flags);
        g_FfxSwapChainCtx = nullptr;
        return false;
    }

    ComPtr<IDXGISwapChain3> swapChain3;
    HRESULT hr = ffxSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    ffxSwapChain->Release();
    if (FAILED(hr) || !swapChain3) {
        testapp::Log("[FG-DIAG] FSR FG swapchain QueryInterface(IDXGISwapChain3) FAILED hr=0x%08lx\n",
                     static_cast<unsigned long>(hr));
        g_FfxDestroyContext(&g_FfxSwapChainCtx, nullptr);
        g_FfxSwapChainCtx = nullptr;
        return false;
    }

    g_SwapChain = swapChain3;
    testapp::Log(
        "[FG-DIAG] ffxCreateContext(FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12) OK ctx=%p swapChain=%p version=0x%08x "
        "flags=0x%x\n",
        (void*)g_FfxSwapChainCtx, g_SwapChain.Get(), FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION, swapChainDesc.Flags);
    return true;
}

static void RegisterFSRUiResource() {
    if (!g_FfxConfigure || !g_FfxSwapChainCtx || !g_FgInputs.valid || !g_FgInputs.uiColor) {
        return;
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc = {};
    uiDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    uiDesc.uiResource = ffxApiGetResourceDX12(g_FgInputs.uiColor.Get(), testapp::dx12fg::kColorReadState,
                                              FFX_API_RESOURCE_USAGE_READ_ONLY);
    uiDesc.flags = 0;

    ffxReturnCode_t ret = g_FfxConfigure(&g_FfxSwapChainCtx, &uiDesc.header);
    if (ret != FFX_API_RETURN_OK || g_LastFsrUiRegisterLogFrame == kNoFsrUiRegisterLogFrame ||
        g_FrameIdCounter - g_LastFsrUiRegisterLogFrame >= 120) {
        g_LastFsrUiRegisterLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxConfigure(registerUI) frameID=%llu result=%u (%s) ui=%p swapchainCtx=%p\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, FfxReturnName(ret),
                     uiDesc.uiResource.resource, (void*)g_FfxSwapChainCtx);
    }
}

static FsrInitResult TryInitFSR() {
    FsrInitResult runtimeResult = LoadFSRRuntime();
    if (runtimeResult != kFsrOk) {
        return runtimeResult;
    }

    ffxCreateContextDescFrameGenerationVersion versionDesc = {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    versionDesc.version = FFX_FRAMEGENERATION_VERSION;

    ffxCreateContextDescFrameGenerationHudless hudlessDesc = {};
    hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    hudlessDesc.header.pNext = &versionDesc.header;
    hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(testapp::dx12fg::kColorFormat);

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = &hudlessDesc.header;
    backendDesc.device = g_Device.Get();

    ffxCreateContextDescFrameGeneration createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    createDesc.header.pNext = &backendDesc.header;
    createDesc.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    createDesc.displaySize = {static_cast<uint32_t>(g_WindowWidth), static_cast<uint32_t>(g_WindowHeight)};
    createDesc.maxRenderSize = createDesc.displaySize;
    createDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(DXGI_FORMAT_R8G8B8A8_UNORM);

    ffxReturnCode_t ret = g_FfxCreateContext(&g_FfxCtx, &createDesc.header, nullptr);
    if (ret != FFX_API_RETURN_OK || !g_FfxCtx) {
        testapp::Log("  ffxCreateContext(FRAMEGENERATION) FAILED code=%u (%s) ctx=%p display=%ux%u fmt=%u\n", ret,
                     FfxReturnName(ret), (void*)g_FfxCtx, createDesc.displaySize.width, createDesc.displaySize.height,
                     createDesc.backBufferFormat);
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
        return kFsrCreateFailed;
    }
    testapp::Log("  ffxCreateContext(FRAMEGENERATION) OK ctx=%p display=%ux%u fmt=%u hudlessFmt=%u swapchainCtx=%p\n",
                 (void*)g_FfxCtx, createDesc.displaySize.width, createDesc.displaySize.height,
                 createDesc.backBufferFormat, hudlessDesc.hudlessBackBufferFormat, (void*)g_FfxSwapChainCtx);
    testapp::Log("[FG-DIAG] Sending startup disabled FSR FG configure to mimic real-game boot/save-load arming\n");
    if (!ConfigureFSR(false, nullptr)) {
        testapp::Log("[FG-DIAG] WARN startup disabled FSR FG configure failed; later enable will still be attempted\n");
    }
    return kFsrOk;
}

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer) {
    if (!g_FfxConfigure || !g_FfxCtx)
        return false;

    ffxConfigureDescFrameGeneration cfgDesc = {};
    cfgDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfgDesc.header.pNext = nullptr;
    cfgDesc.swapChain = g_SwapChain.Get();
    cfgDesc.presentCallback = TestPresentCallback;
    cfgDesc.presentCallbackUserContext = nullptr;
    cfgDesc.frameGenerationCallback = TestFrameGenerationCallback;
    cfgDesc.frameGenerationCallbackUserContext = &g_FfxCtx;
    cfgDesc.frameGenerationEnabled = enable;
    cfgDesc.allowAsyncWorkloads = true;
    if (g_FgInputs.valid && g_FgInputs.hudlessColor) {
        cfgDesc.HUDLessColor = ffxApiGetResourceDX12(g_FgInputs.hudlessColor.Get(), testapp::dx12fg::kColorReadState,
                                                     FFX_API_RESOURCE_USAGE_READ_ONLY);
    } else if (backbuffer) {
        cfgDesc.HUDLessColor = ffxApiGetResourceDX12(backbuffer, FFX_API_RESOURCE_STATE_RENDER_TARGET,
                                                     FFX_API_RESOURCE_USAGE_RENDERTARGET);
    } else {
        cfgDesc.HUDLessColor.resource = nullptr;
        cfgDesc.HUDLessColor.state = FFX_API_RESOURCE_STATE_COMMON;
    }
    cfgDesc.flags = g_FfxSwapChainCtx ? 0 : FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfgDesc.onlyPresentGenerated = false;
    cfgDesc.generationRect = {0, 0, static_cast<int32_t>(g_WindowWidth), static_cast<int32_t>(g_WindowHeight)};
    cfgDesc.frameID = g_FrameIdCounter;

    ffxReturnCode_t ret = g_FfxConfigure(&g_FfxCtx, &cfgDesc.header);
    if (ret != FFX_API_RETURN_OK) {
        testapp::Log(
            "[FG-DIAG] ffxConfigure(%s) frameID=%llu FAILED code=%u (%s) swapChain=%p swapchainCtx=%p flags=0x%x "
            "hudless=%p\n",
            enable ? "enable" : "disable", (unsigned long long)g_FrameIdCounter, ret, FfxReturnName(ret),
            (void*)g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, cfgDesc.flags, cfgDesc.HUDLessColor.resource);
    } else {
        testapp::Log(
            "[FG-DIAG] ffxConfigure(%s) frameID=%llu enabled=%d OK swapChain=%p swapchainCtx=%p flags=0x%x "
            "hudless=%p\n",
            enable ? "enable" : "disable", (unsigned long long)g_FrameIdCounter, cfgDesc.frameGenerationEnabled,
            (void*)g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, cfgDesc.flags, cfgDesc.HUDLessColor.resource);
    }
    testapp::LogFlush();
    if (ret == FFX_API_RETURN_OK && enable) {
        RegisterFSRUiResource();
    }
    return (ret == FFX_API_RETURN_OK);
}

static void DispatchFSRPrepare(float elapsedSeconds) {
    if (!g_FsrEnabled || !g_FfxDispatch || !g_FfxCtx || !g_FgInputs.valid) {
        return;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = g_FrameIdCounter;
    prepare.commandList = g_CommandList.Get();
    prepare.renderSize = {static_cast<uint32_t>(g_WindowWidth), static_cast<uint32_t>(g_WindowHeight)};
    prepare.jitterOffset = {0.0f, 0.0f};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = elapsedSeconds * 1000.0f;
    prepare.reset = (g_FrameIdCounter < 4);
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.04719755f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(g_FgInputs.depth.Get(), testapp::dx12fg::kDepthReadState,
                                          FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    prepare.motionVectors = ffxApiGetResourceDX12(g_FgInputs.motionVectors.Get(), testapp::dx12fg::kColorReadState,
                                                  FFX_API_RESOURCE_USAGE_READ_ONLY);
    prepare.cameraPosition[2] = -2.0f;
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = 1.0f;

    ffxReturnCode_t ret = g_FfxDispatch(&g_FfxCtx, &prepare.header);
    if (ret != FFX_API_RETURN_OK || g_FrameIdCounter < 5 || g_FrameIdCounter - g_LastFsrPrepareLogFrame >= 120) {
        g_LastFsrPrepareLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxDispatch(prepareV2) frameID=%llu result=%u depth=%p mvec=%p\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, prepare.depth.resource,
                     prepare.motionVectors.resource);
    }
}

static void DestroyFSRContexts() {
    if (g_FfxConfigure && g_FfxCtx)
        ConfigureFSR(false, nullptr);
    testapp::Log("[FG-DIAG] FSR callback totals: present=%llu frameGeneration=%llu\n",
                 static_cast<unsigned long long>(g_FsrPresentCallbackCount.load()),
                 static_cast<unsigned long long>(g_FsrFrameGenerationCallbackCount.load()));
    if (g_FfxDestroyContext && g_FfxCtx) {
        g_FfxDestroyContext(&g_FfxCtx, nullptr);
        g_FfxCtx = nullptr;
    }
    if (g_FfxDestroyContext && g_FfxSwapChainCtx) {
        g_FfxDestroyContext(&g_FfxSwapChainCtx, nullptr);
        g_FfxSwapChainCtx = nullptr;
    }
}

static void UnloadFSRRuntime() {
    if (g_FfxModule) {
