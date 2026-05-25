// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.

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

static bool LoadFSRRuntime() {
    if (g_FfxModule) {
        g_FsrRuntimeLoaded = true;
        return true;
    }

    PreloadAmdCompanionDlls();
    const wchar_t* dllNames[] = {
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_loader_dx12.dll",
        L"amd_fidelityfx_dx12.dll",
        L"ffx_framegeneration.dll",
    };
    for (auto dllName : dllNames) {
        g_FfxModule = LoadLibraryW(dllName);
        if (g_FfxModule) {
            ++g_FsrRuntimeLoadGeneration;
            testapp::Log("  Loaded FSR runtime: %S base=%p generation=%llu\n", dllName, g_FfxModule,
                         static_cast<unsigned long long>(g_FsrRuntimeLoadGeneration));
            break;
        }
    }
    if (!g_FfxModule) {
        testapp::Log("[FG-DIAG] No FSR runtime DLL found\n");
        return false;
    }

    g_FfxCreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_FfxModule, "ffxCreateContext"));
    g_FfxConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_FfxModule, "ffxConfigure"));
    g_FfxDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_FfxModule, "ffxDispatch"));
    g_FfxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_FfxModule, "ffxDestroyContext"));
    if (!g_FfxCreateContext || !g_FfxConfigure || !g_FfxDispatch || !g_FfxDestroyContext) {
        testapp::Log("[FG-DIAG] FSR DLL missing ffxCreateContext/ffxConfigure/ffxDispatch/ffxDestroyContext exports\n");
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
        g_FsrRuntimeLoaded = false;
        return false;
    }
    g_FsrRuntimeLoaded = true;
    testapp::Log("[FG-DIAG] FSR exports: create=%p configure=%p dispatch=%p destroy=%p generation=%llu\n",
                 reinterpret_cast<void*>(g_FfxCreateContext), reinterpret_cast<void*>(g_FfxConfigure),
                 reinterpret_cast<void*>(g_FfxDispatch), reinterpret_cast<void*>(g_FfxDestroyContext),
                 static_cast<unsigned long long>(g_FsrRuntimeLoadGeneration));
    return true;
}

static void UnloadFSRRuntime(const char* reason) {
    if (!g_FfxModule) {
        g_FsrRuntimeLoaded = false;
        g_FfxCreateContext = nullptr;
        g_FfxConfigure = nullptr;
        g_FfxDispatch = nullptr;
        g_FfxDestroyContext = nullptr;
        return;
    }

    testapp::Log("[FG-DIAG] Unloading FSR runtime generation=%llu base=%p reason=%s\n",
                 static_cast<unsigned long long>(g_FsrRuntimeLoadGeneration), g_FfxModule,
                 reason ? reason : "unknown");
    FreeLibrary(g_FfxModule);
    g_FfxModule = nullptr;
    g_FfxCreateContext = nullptr;
    g_FfxConfigure = nullptr;
    g_FfxDispatch = nullptr;
    g_FfxDestroyContext = nullptr;
    g_FsrRuntimeLoaded = false;
    g_FsrInitialized = false;
    testapp::LogFlush();
}

static bool EnsureFSRRuntimeLoaded(const char* reason) {
    if (g_FsrRuntimeLoaded && g_FfxModule && g_FfxCreateContext && g_FfxConfigure && g_FfxDispatch &&
        g_FfxDestroyContext) {
        return true;
    }

    testapp::Log("[FG-DIAG] Loading FSR runtime on demand reason=%s\n", reason ? reason : "unknown");
    const bool loaded = LoadFSRRuntime();
    g_FsrRuntimeLoaded = loaded;
    if (!loaded) {
        testapp::Log("[FG-DIAG] FSR runtime on-demand load failed reason=%s\n", reason ? reason : "unknown");
        testapp::LogFlush();
    }
    return loaded;
}

static void MaybeUnloadFSRRuntimeAfterSwitch(const char* reason) {
    if (!g_FsrReloadRuntimeOnSwitch) {
        return;
    }

    if (g_FfxCtx || g_FfxSwapChainCtx || g_FsrEnabled) {
        testapp::Log("[FG-DIAG] WARN skipping FSR runtime unload because contexts are still live reason=%s "
                     "fgCtx=%p swapchainCtx=%p enabled=%d\n",
                     reason ? reason : "unknown", (void*)g_FfxCtx, (void*)g_FfxSwapChainCtx,
                     g_FsrEnabled ? 1 : 0);
        return;
    }
    UnloadFSRRuntimeSerialized(reason);
}

static ffxReturnCode_t TestPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*) {
    uint64_t callbackIndex = ++g_FsrPresentCallbackCount;
    if (params) {
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        testapp::dx12fg::CopyFfxPresentSourceToOutput(cmdList, params);
        if (callbackIndex <= 5 || (callbackIndex % 120) == 0) {
            testapp::Log("[FG-DIAG] FSR present callback #%llu frameID=%llu generated=%d backbuffer=%p output=%p\n",
                         static_cast<unsigned long long>(callbackIndex),
                         static_cast<unsigned long long>(params->frameID), params->isGeneratedFrame ? 1 : 0,
                         params->currentBackBuffer.resource, params->outputSwapChainBuffer.resource);
        }
    }
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t TestFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx) {
    if (!params || !pUserCtx || !g_FfxDispatch) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ffxContext* context = reinterpret_cast<ffxContext*>(pUserCtx);
    ffxReturnCode_t ret = g_FfxDispatch(context, &params->header);
    uint64_t callbackIndex = ++g_FsrFrameGenerationCallbackCount;
    if (ret != FFX_API_RETURN_OK || callbackIndex <= 5 || (callbackIndex % 120) == 0) {
        testapp::Log("[FG-DIAG] FSR frame-generation callback #%llu frameID=%llu result=%u present=%p output0=%p\n",
                     static_cast<unsigned long long>(callbackIndex),
                     static_cast<unsigned long long>(params->frameID), ret, params->presentColor.resource,
                     params->outputs[0].resource);
    }
    return ret;
}

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer, const char* reason = "switch",
                         bool forceLog = true) {
    if (!g_FfxConfigure || !g_FfxCtx) {
        return false;
    }

    ffxConfigureDescFrameGeneration cfgDesc = {};
    cfgDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfgDesc.swapChain = g_SwapChain.Get();
    cfgDesc.presentCallback = TestPresentCallback;
    cfgDesc.frameGenerationCallback = TestFrameGenerationCallback;
    cfgDesc.frameGenerationCallbackUserContext = &g_FfxCtx;
    cfgDesc.frameGenerationEnabled = enable;
    cfgDesc.allowAsyncWorkloads = true;
    if (g_FgInputs.valid && g_FgInputs.hudlessColor) {
        cfgDesc.HUDLessColor =
            ffxApiGetResourceDX12(g_FgInputs.hudlessColor.Get(), testapp::dx12fg::kColorReadState,
                                  FFX_API_RESOURCE_USAGE_READ_ONLY);
    } else if (backbuffer) {
        cfgDesc.HUDLessColor = ffxApiGetResourceDX12(backbuffer, FFX_API_RESOURCE_STATE_RENDER_TARGET,
                                                     FFX_API_RESOURCE_USAGE_RENDERTARGET);
    }
    cfgDesc.flags = g_FfxSwapChainCtx ? 0 : FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfgDesc.onlyPresentGenerated = false;
    cfgDesc.generationRect = {0, 0, static_cast<int32_t>(g_WindowWidth), static_cast<int32_t>(g_WindowHeight)};
    cfgDesc.frameID = g_FrameIdCounter;

    ffxReturnCode_t ret = g_FfxConfigure(&g_FfxCtx, &cfgDesc.header);
    if (forceLog || ret != FFX_API_RETURN_OK || g_FrameIdCounter - g_LastFsrConfigureLogFrame >= 120) {
        g_LastFsrConfigureLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxConfigure(%s) reason=%s frameID=%llu enabled=%d result=%u (%s) "
                     "swapChain=%p swapchainCtx=%p flags=0x%x hudless=%p everyFrame=%d\n",
                     enable ? "enable" : "disable", reason ? reason : "unknown",
                     static_cast<unsigned long long>(g_FrameIdCounter), cfgDesc.frameGenerationEnabled, ret,
                     FfxReturnName(ret), g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, cfgDesc.flags,
                     cfgDesc.HUDLessColor.resource, g_FsrConfigureEveryFrame ? 1 : 0);
        testapp::LogFlush();
    }
    return ret == FFX_API_RETURN_OK;
}

static bool WaitForFSRSwapChainPresents(const char* reason) {
    if (!g_FfxDispatch || !g_FfxSwapChainCtx) {
        return true;
    }

    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsDX12 waitDesc = {};
    waitDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12;
    ffxReturnCode_t ret = g_FfxDispatch(&g_FfxSwapChainCtx, &waitDesc.header);
    testapp::Log("[FG-DIAG] ffxDispatch(waitForPresents) reason=%s frameID=%llu result=%u (%s) swapchainCtx=%p\n",
                 reason ? reason : "unknown", static_cast<unsigned long long>(g_FrameIdCounter), ret,
                 FfxReturnName(ret), (void*)g_FfxSwapChainCtx);
    testapp::LogFlush();
    return ret == FFX_API_RETURN_OK;
}

static void RegisterFSRUiResource() {
    if (!g_FfxConfigure || !g_FfxSwapChainCtx || !g_FgInputs.valid || !g_FgInputs.uiColor) {
        return;
    }

    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc = {};
    uiDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    uiDesc.uiResource = ffxApiGetResourceDX12(g_FgInputs.uiColor.Get(), testapp::dx12fg::kColorReadState,
                                              FFX_API_RESOURCE_USAGE_READ_ONLY);
    ffxReturnCode_t ret = g_FfxConfigure(&g_FfxSwapChainCtx, &uiDesc.header);
    if (ret != FFX_API_RETURN_OK || g_LastFsrUiRegisterLogFrame == kNoFsrUiRegisterLogFrame ||
        g_FrameIdCounter - g_LastFsrUiRegisterLogFrame >= 120) {
        g_LastFsrUiRegisterLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxConfigure(registerUI) frameID=%llu result=%u (%s) ui=%p swapchainCtx=%p\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, FfxReturnName(ret),
                     uiDesc.uiResource.resource, (void*)g_FfxSwapChainCtx);
    }
}

static bool CreateFSRSwapChainForHwndContext(IDXGIFactory4* factory, HWND hwnd, DXGI_SWAP_CHAIN_DESC1& desc) {
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
    createDesc.desc = &desc;
    createDesc.fullscreenDesc = &fullscreenDesc;
    createDesc.dxgiFactory = factory;
    createDesc.gameQueue = g_CommandQueue.Get();

    ffxReturnCode_t ret = g_FfxCreateContext(&g_FfxSwapChainCtx, &createDesc.header, nullptr);
    if (ret != FFX_API_RETURN_OK || !g_FfxSwapChainCtx || !ffxSwapChain) {
        testapp::Log("[FG-DIAG] ffxCreateContext(FG_SWAPCHAIN_HWND_DX12) FAILED code=%u (%s) ctx=%p swap=%p flags=0x%x\n",
                     ret, FfxReturnName(ret), (void*)g_FfxSwapChainCtx, ffxSwapChain, desc.Flags);
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
    testapp::Log("[FG-DIAG] ffxCreateContext(FG_SWAPCHAIN_HWND_DX12) OK ctx=%p swapChain=%p version=0x%08x flags=0x%x\n",
                 (void*)g_FfxSwapChainCtx, g_SwapChain.Get(), FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION, desc.Flags);
    return true;
}

static bool TryInitFSR() {
    if (!g_FfxCreateContext || !g_Device) {
        return false;
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
        testapp::Log("[FG-DIAG] ffxCreateContext(FRAMEGENERATION) FAILED code=%u (%s) ctx=%p display=%ux%u\n",
                     ret, FfxReturnName(ret), (void*)g_FfxCtx, createDesc.displaySize.width,
                     createDesc.displaySize.height);
        return false;
    }
    testapp::Log("[FG-DIAG] ffxCreateContext(FRAMEGENERATION) OK ctx=%p display=%ux%u swapchainCtx=%p\n",
                 (void*)g_FfxCtx, createDesc.displaySize.width, createDesc.displaySize.height,
                 (void*)g_FfxSwapChainCtx);
    testapp::Log("[FG-DIAG] Sending startup disabled FSR configure before auto/user enable\n");
    ConfigureFSR(false, nullptr, "startup disabled", true);
    return true;
}

static void DispatchFSRPrepare(float elapsedSeconds) {
    if (!g_FsrEnabled || g_FsrSuspended || !g_FfxDispatch || !g_FfxCtx || !g_FgInputs.valid) {
        return;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = g_FrameIdCounter;
    prepare.commandList = g_CommandList.Get();
    prepare.renderSize = {static_cast<uint32_t>(g_WindowWidth), static_cast<uint32_t>(g_WindowHeight)};
    prepare.motionVectorScale = {1.0f, 1.0f};
    prepare.frameTimeDelta = elapsedSeconds * 1000.0f;
    prepare.reset = g_FrameIdCounter < 4;
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
        testapp::Log("[FG-DIAG] ffxDispatch(prepareV2) frameID=%llu result=%u (%s) depth=%p mvec=%p\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, FfxReturnName(ret),
                     prepare.depth.resource, prepare.motionVectors.resource);
    }
}

static void DestroyFSRContexts() {
    if (g_FfxConfigure && g_FfxCtx) {
        ConfigureFSR(false, nullptr, "destroy FSR contexts", true);
        WaitForFSRSwapChainPresents("destroy FSR contexts");
    }
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
    g_FsrEnabled = false;
    g_FsrSuspended = false;
    g_FsrInitialized = false;
}
