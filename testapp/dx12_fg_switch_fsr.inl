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

static bool ShouldUseFSRPresentCallbackForConfigure(bool enable) {
    if (!g_FsrPresentCallbackStress) {
        return true;
    }
    if (!enable) {
        return g_FsrLastConfigureUsedPresentCallback;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsed = std::chrono::duration<float>(now - g_FsrPresentCallbackStressStartTime).count();
    const int interval = g_FsrPresentCallbackToggleIntervalSeconds > 0 ? g_FsrPresentCallbackToggleIntervalSeconds : 1;
    const int phase = static_cast<int>(elapsed / static_cast<float>(interval));
    return (phase % 2) != 0;
}

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer, const char* reason = "switch",
                         bool forceLog = true) {
    if (!g_FfxConfigure || !g_FfxCtx) {
        return false;
    }

    const bool usePresentCallback = ShouldUseFSRPresentCallbackForConfigure(enable);
    const bool presentCallbackModeChanged = usePresentCallback != g_FsrLastConfigureUsedPresentCallback;
    ffxConfigureDescFrameGeneration cfgDesc = {};
    cfgDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfgDesc.swapChain = g_SwapChain.Get();
    cfgDesc.presentCallback = usePresentCallback ? TestPresentCallback : nullptr;
    cfgDesc.presentCallbackUserContext = nullptr;
    cfgDesc.frameGenerationCallback = TestFrameGenerationCallback;
    cfgDesc.frameGenerationCallbackUserContext = &g_FfxCtx;
    cfgDesc.frameGenerationEnabled = enable;
    cfgDesc.allowAsyncWorkloads = true;
    if (g_FgInputs.valid && g_FgInputs.hudlessColor) {
        // EMPIRICAL CONTRACT (do not "fix" to FFX_API_RESOURCE_STATE_* flags): resources consumed
        // by the frame-interpolation swapchain (HUDLessColor here, the registered UI resource
        // below) interpret FfxApiResource.state as NATIVE D3D12 states. Declaring the documented
        // FFX flag (PIXEL_COMPUTE_READ = 0xC) makes AMD's async FG worker record a barrier with
        // before-state RENDER_TARGET|UNORDERED_ACCESS (D3D12 0xC) and the NV driver AVs in
        // CCommandList::Close within ~1 s of FG enable (cdb-verified, deterministic). The
        // dispatch-time prepare inputs follow the documented FFX flags instead (see
        // DispatchFSRPrepare).
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
    if (ret == FFX_API_RETURN_OK) {
        g_FsrLastConfigureUsedPresentCallback = usePresentCallback;
    }
    if (forceLog || ret != FFX_API_RETURN_OK || presentCallbackModeChanged ||
        g_FrameIdCounter - g_LastFsrConfigureLogFrame >= 120) {
        g_LastFsrConfigureLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxConfigure(%s) reason=%s frameID=%llu enabled=%d result=%u (%s) "
                     "swapChain=%p swapchainCtx=%p flags=0x%x hudless=%p everyFrame=%d "
                     "presentCallbackRoute=%s callbackModeChanged=%d\n",
                     enable ? "enable" : "disable", reason ? reason : "unknown",
                     static_cast<unsigned long long>(g_FrameIdCounter), cfgDesc.frameGenerationEnabled, ret,
                     FfxReturnName(ret), g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, cfgDesc.flags,
                     cfgDesc.HUDLessColor.resource, g_FsrConfigureEveryFrame ? 1 : 0,
                     usePresentCallback ? "app-callback" : "amd-internal",
                     presentCallbackModeChanged ? 1 : 0);
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

// Resolve which texture to register as the FSR FG UI resource. Default: the full-size UI texture (the
// universal path — composited HUD, exercised on every normal run). Opt-in degenerate mode
// (g_FsrDegenerateUiResource): a 1x1 placeholder that mimics GTA V Enhanced's degenerate UI registration, so
// an INJECTED run exercises CE's substitution path (CE swaps in its own backbuffer-sized texture). The
// placeholder is lazily created once in the same format as the real UI texture and is never rendered to (CE
// substitutes it; standalone runs just composite an empty 1x1, which is the point of the test).
static ID3D12Resource* AcquireFsrUiRegistrationTexture() {
    if (!g_FsrDegenerateUiResource) {
        return g_FgInputs.uiColor.Get();
    }
    if (!g_FsrDegenerateUiTexture && g_Device && g_FgInputs.uiColor) {
        const D3D12_RESOURCE_DESC src = g_FgInputs.uiColor->GetDesc();
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1;
        td.Height = 1;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = src.Format;
        td.SampleDesc.Count = 1;
        if (SUCCEEDED(g_Device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
                                                        testapp::dx12fg::kColorReadState, nullptr,
                                                        IID_PPV_ARGS(&g_FsrDegenerateUiTexture)))) {
            g_FsrDegenerateUiTexture->SetName(L"FsrDegenerateUiPlaceholder1x1");
            testapp::Log(
                "[FG-DIAG] Degenerate-UI mode: registering 1x1 UI placeholder %p (fmt=%d) to exercise CE's "
                "no-callback FSR FG substitution path\n",
                (void*)g_FsrDegenerateUiTexture.Get(), static_cast<int>(src.Format));
        }
    }
    return g_FsrDegenerateUiTexture ? g_FsrDegenerateUiTexture.Get() : g_FgInputs.uiColor.Get();
}

static void RegisterFSRUiResource() {
    if (!g_FfxConfigure || !g_FfxSwapChainCtx || !g_FgInputs.valid || !g_FgInputs.uiColor) {
        return;
    }

    ID3D12Resource* uiTex = AcquireFsrUiRegistrationTexture();
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiDesc = {};
    uiDesc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    // NATIVE D3D12 state by the same frame-interpolation-swapchain contract as HUDLessColor in
    // ConfigureFSR (see the comment there).
    uiDesc.uiResource =
        ffxApiGetResourceDX12(uiTex, testapp::dx12fg::kColorReadState, FFX_API_RESOURCE_USAGE_READ_ONLY);
    // We re-render the UI texture every frame without synchronizing against the FG swapchain's
    // interpolation present. Ask the swapchain to double-buffer (snapshot) the UI resource so a
    // next-frame UI rewrite can't race compositing onto an in-flight generated frame (HUD tearing).
    uiDesc.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
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
    hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(testapp::dx12fg::kHdrColorFormat);

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

static void DispatchFSRPrepare(float frameDeltaMs) {
    if (!g_FsrEnabled || g_FsrSuspended || !g_FfxDispatch || !g_FfxCtx || !g_FgInputs.valid) {
        return;
    }
    const testapp::dx12fg::SceneCamera& camera = g_Scene.Camera();

    ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = g_FrameIdCounter;
    prepare.commandList = g_CommandList.Get();
    // Depth + motion vectors are produced at RENDER resolution when upscaling is active.
    prepare.renderSize = {static_cast<uint32_t>(g_RenderWidth), static_cast<uint32_t>(g_RenderHeight)};
    // Same jitter the camera rendered with (FSR convention: the value whose projection offset is
    // (2*jx/renderW, -2*jy/renderH) -- exactly how Mat4ApplyJitter applies it).
    prepare.jitterOffset = {g_CurrentJitter.x, g_CurrentJitter.y};
    // Scene motion vectors are emitted in UV space (prevUV - curUV); FFX expects pixel-space
    // motion, so scale by renderSize (per ffx_framegeneration.h motionVectorScale docs).
    prepare.motionVectorScale = {static_cast<float>(g_RenderWidth), static_cast<float>(g_RenderHeight)};
    prepare.frameTimeDelta = frameDeltaMs;
    prepare.reset = g_FrameIdCounter < 4;
    prepare.cameraNear = camera.valid ? camera.nearZ : 0.1f;
    prepare.cameraFar = camera.valid ? camera.farZ : 1000.0f;
    prepare.cameraFovAngleVertical = camera.valid ? camera.fovY : 1.04719755f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    // Dispatch-time inputs use the documented FFX state flags (AMD's FidelityFX_FSR sample passes
    // exactly FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ here); both textures sit in the
    // PIXEL|NON_PIXEL shader-read D3D12 state, which this FFX flag maps to.
    prepare.depth = ffxApiGetResourceDX12(g_FgInputs.depth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                          FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    prepare.motionVectors = ffxApiGetResourceDX12(g_FgInputs.motionVectors.Get(),
                                                  FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
                                                  FFX_API_RESOURCE_USAGE_READ_ONLY);
    if (camera.valid) {
        for (int axis = 0; axis < 3; ++axis) {
            prepare.cameraPosition[axis] = camera.basis.eye[axis];
            prepare.cameraRight[axis] = camera.basis.right[axis];
            prepare.cameraUp[axis] = camera.basis.up[axis];
            prepare.cameraForward[axis] = camera.basis.forward[axis];
        }
    } else {
        prepare.cameraPosition[2] = -2.0f;
        prepare.cameraUp[1] = 1.0f;
        prepare.cameraRight[0] = 1.0f;
        prepare.cameraForward[2] = 1.0f;
    }

    ffxReturnCode_t ret = g_FfxDispatch(&g_FfxCtx, &prepare.header);
    if (ret != FFX_API_RETURN_OK || g_FrameIdCounter < 5 || g_FrameIdCounter - g_LastFsrPrepareLogFrame >= 120) {
        g_LastFsrPrepareLogFrame = g_FrameIdCounter;
        testapp::Log("[FG-DIAG] ffxDispatch(prepareV2) frameID=%llu result=%u (%s) depth=%p mvec=%p\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, FfxReturnName(ret),
                     prepare.depth.resource, prepare.motionVectors.resource);
    }
}

static void DestroyFSRContexts() {
    DestroyFSRUpscaleContext();
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
