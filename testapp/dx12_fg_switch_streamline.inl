// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.

static void SlLogCallback(sl::LogType type, const char* msg) {
    testapp::Log("[FG-DIAG] SL log type=%u %s\n", static_cast<unsigned>(type), msg ? msg : "");
}

static sl::float4x4 IdentityMatrix() {
    sl::float4x4 matrix = {};
    matrix[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
    matrix[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
    matrix[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
    matrix[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
    return matrix;
}

static bool LoadStreamlineAndInit() {
    g_SlModule = LoadLibraryW(L"sl.interposer.dll");
    if (!g_SlModule) {
        testapp::Log("[FG-DIAG] sl.interposer.dll not found\n");
        return false;
    }
    testapp::Log("  Loaded sl.interposer.dll\n");

    g_SlInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(g_SlModule, "slInit"));
    g_SlShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(g_SlModule, "slShutdown"));
    g_SlSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(g_SlModule, "slSetD3DDevice"));
    g_SlGetFeatureFunction =
        reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(g_SlModule, "slGetFeatureFunction"));
    g_SlGetNewFrameToken =
        reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(g_SlModule, "slGetNewFrameToken"));
    g_SlSetConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(g_SlModule, "slSetConstants"));
    g_SlSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(g_SlModule, "slSetTagForFrame"));
    g_SlCreateDXGIFactory1 =
        reinterpret_cast<PFun_CreateDXGIFactory1>(GetProcAddress(g_SlModule, "CreateDXGIFactory1"));
    g_SlD3D12CreateDevice =
        reinterpret_cast<PFun_D3D12CreateDevice>(GetProcAddress(g_SlModule, "D3D12CreateDevice"));

    if (!g_SlInit || !g_SlShutdown || !g_SlSetD3DDevice || !g_SlGetFeatureFunction || !g_SlGetNewFrameToken ||
        !g_SlSetConstants || !g_SlSetTagForFrame) {
        testapp::Log("[FG-DIAG] sl.interposer.dll missing required Streamline exports\n");
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
        return false;
    }
    testapp::Log("[FG-DIAG] Streamline proxy exports: CreateDXGIFactory1=%p D3D12CreateDevice=%p\n",
                 (void*)g_SlCreateDXGIFactory1, (void*)g_SlD3D12CreateDevice);

    std::wstring pluginPath = ExeDirectoryW();
    const wchar_t* pluginPaths[] = {pluginPath.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref = {};
    pref.logLevel = sl::LogLevel::eVerbose;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = pluginPath.c_str();
    pref.logMessageCallback = SlLogCallback;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eUseDXGIFactoryProxy;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = _countof(features);
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "CaptureProject DX12 FG switch test";
    pref.projectId = "7f1d0f20-2f9a-4f2d-9c64-5d1220e9d013";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    sl::Result initResult = g_SlInit(pref, sl::kSDKVersion);
    testapp::Log("[FG-DIAG] slInit result=%d (%s) pluginPath=%S sdk=0x%llx\n", static_cast<int>(initResult),
                 SlResultName(initResult), pluginPath.c_str(), static_cast<unsigned long long>(sl::kSDKVersion));
    if (initResult != sl::Result::eOk) {
        if (g_SlShutdown) {
            sl::Result shutdownResult = g_SlShutdown();
            testapp::Log("[FG-DIAG] slShutdown after failed slInit result=%d (%s)\n",
                         static_cast<int>(shutdownResult), SlResultName(shutdownResult));
        }
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
        g_SlInit = nullptr;
        g_SlShutdown = nullptr;
        g_SlSetD3DDevice = nullptr;
        g_SlGetFeatureFunction = nullptr;
        g_SlGetNewFrameToken = nullptr;
        g_SlSetConstants = nullptr;
        g_SlSetTagForFrame = nullptr;
        g_SlCreateDXGIFactory1 = nullptr;
        g_SlD3D12CreateDevice = nullptr;
        return false;
    }
    g_SlInitialized = true;
    return true;
}

// Drives Reflex strictly from the DLSS-G active state. eLowLatency only while FG is actually
// generating frames; eOff otherwise. Idempotent so per-frame/per-transition callers stay cheap.
static void ApplyReflexMode(bool active, const char* reason) {
    if (!g_SlReflexSetOptions) {
        return;
    }
    if (active == g_ReflexLowLatencyActive) {
        return;
    }
    sl::ReflexOptions reflex = {};
    reflex.mode = active ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    sl::Result reflexResult = g_SlReflexSetOptions(reflex);
    testapp::Log("[FG-DIAG] slReflexSetOptions(mode=%s) reason=%s result=%d (%s)\n",
                 active ? "LowLatency" : "Off", reason ? reason : "unknown",
                 static_cast<int>(reflexResult), SlResultName(reflexResult));
    if (reflexResult == sl::Result::eOk) {
        g_ReflexLowLatencyActive = active;
    }
}

static bool TryInitDLSSFG() {
    if (!g_SlInitialized || !g_SlGetFeatureFunction) {
        return false;
    }
    if (!g_SlDeviceSet && g_SlSetD3DDevice) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = deviceResult == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slSetD3DDevice result=%d (%s)\n", static_cast<int>(deviceResult),
                     SlResultName(deviceResult));
    }

    void* fnPtr = nullptr;
    if (g_SlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slDLSSGSetOptions @ %p\n", (void*)g_SlDLSSGSetOptions);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slDLSSGGetState @ %p\n", (void*)g_SlDLSSGGetState);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slReflexSetOptions @ %p\n", (void*)g_SlReflexSetOptions);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slReflexSleep @ %p\n", (void*)g_SlReflexSleep);
    }
    if (g_SlGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slPCLSetMarker @ %p\n", (void*)g_SlPCLSetMarker);
    }
    // Do NOT force Reflex on at init: FG is not active yet. Reflex is driven by SetDLSSFGMode()
    // so the FG-aware Reflex frame limiter cannot linger while FG is off or suspended.
    ApplyReflexMode(false, "DLSS init");
    return g_SlDLSSGSetOptions && g_SlDLSSGGetState;
}

static bool SetDLSSFGMode(bool enable) {
    if (!g_SlDLSSGSetOptions) {
        return false;
    }

    sl::DLSSGOptions options = {};
    options.mode = enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    options.numBackBuffers = g_SwapChainBufferCount;
    options.colorWidth = static_cast<uint32_t>(g_WindowWidth);
    options.colorHeight = static_cast<uint32_t>(g_WindowHeight);
    options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    options.mvecDepthWidth = static_cast<uint32_t>(g_WindowWidth);
    options.mvecDepthHeight = static_cast<uint32_t>(g_WindowHeight);
    options.mvecBufferFormat = testapp::dx12fg::kMotionVectorFormat;
    options.depthBufferFormat = testapp::dx12fg::kDepthFormat;
    options.hudLessBufferFormat = testapp::dx12fg::kColorFormat;
    options.uiBufferFormat = testapp::dx12fg::kColorFormat;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.enableUserInterfaceRecomposition = sl::eTrue;

    sl::Result ret = g_SlDLSSGSetOptions(g_SlViewport, options);
    // frameToken makes an enable/disable pair landing on the SAME frame visible in the log --
    // Streamline treats that as a race with Present() and it leaves the pacer half-initialized.
    testapp::Log("[FG-DIAG] slDLSSGSetOptions(mode=%u) backBuffers=%u color=%dx%d frameToken=%u result=%d (%s)\n",
                 static_cast<uint32_t>(options.mode), options.numBackBuffers, options.colorWidth, options.colorHeight,
                 g_FrameTokenIndex, static_cast<int>(ret), SlResultName(ret));
    // INVARIANT (DRED-proven, 3 reproductions): Reflex must stay eLowLatency for as long as the
    // DLSS-G proxy swapchain presents -- INCLUDING the suspended state (slDLSSGSetOptions(eOff)
    // leaves the proxy and its present pacer alive, still submitting "pacer command list" GPU work
    // per present). Calling slReflexSetOptions(eOff) under that live pacer wedges the GPU within
    // ~100 frames (DXGI_ERROR_DEVICE_HUNG 0x887a0006) -- immediately, after a fence/present-gated
    // drain, and even after slFreeResources(kFeatureDLSS_G) (the pacer is owned by the proxy, not
    // the per-viewport feature resources). With Reflex kept on, the suspended hold is stable and
    // capped by the normal Reflex limiter, exactly like a real game with FG off in a menu. Reflex
    // genuinely turns off only with the proxy teardown (ShutdownStreamline) when leaving DLSS mode.
    if (enable && ret == sl::Result::eOk) {
        ApplyReflexMode(true, "DLSS FG enable");
    }
    testapp::LogFlush();
    return ret == sl::Result::eOk;
}

static void PollDLSSFGState() {
    if (!g_SlDLSSGGetState) {
        return;
    }
    sl::DLSSGState state = {};
    sl::Result ret = g_SlDLSSGGetState(g_SlViewport, state, nullptr);
    testapp::Log("[FG-DIAG] slDLSSGGetState ret=%d (%s) status=%u genFrames=%u maxGen=%u dynamicMFG=%d\n",
                 static_cast<int>(ret), SlResultName(ret), static_cast<uint32_t>(state.status),
                 state.numFramesActuallyPresented, state.numFramesToGenerateMax,
                 static_cast<int>(state.bIsDynamicMFGSupported));
}

static void SetPCLMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name) {
    if (!token || !g_SlPCLSetMarker) {
        return;
    }
    sl::Result ret = g_SlPCLSetMarker(marker, *token);
    if (ret != sl::Result::eOk && g_FrameTokenIndex < 8) {
        testapp::Log("[FG-DIAG] slPCLSetMarker(%s) frame=%u result=%d (%s)\n", name,
                     g_FrameTokenIndex ? g_FrameTokenIndex - 1 : 0, static_cast<int>(ret), SlResultName(ret));
    }
}

static sl::FrameToken* BeginStreamlineFrame() {
    if (!g_DlssInitialized || !g_SlGetNewFrameToken) {
        return nullptr;
    }
    uint32_t frameIndex = g_FrameTokenIndex++;
    sl::FrameToken* token = nullptr;
    sl::Result ret = g_SlGetNewFrameToken(token, &frameIndex);
    if (ret != sl::Result::eOk || !token) {
        testapp::Log("[FG-DIAG] slGetNewFrameToken frame=%u failed result=%d (%s)\n", frameIndex,
                     static_cast<int>(ret), SlResultName(ret));
        return nullptr;
    }
    // Call slReflexSleep once per frame whenever the Reflex frame token exists, per Streamline's
    // integration contract. We still emit PCL markers (SimulationStart/PresentStart/...) every
    // frame; skipping the matching sleep on those tokens left Streamline's present/PCL pipeline
    // waiting on an un-slept token and stalled the present queue after suspending DLSS FG (a GPU
    // fence that never signals, observed in a freeze dump). While DLSS FG is suspended this sleep
    // keeps running with Reflex eLowLatency (required by the live proxy pacer, see SetDLSSFGMode);
    // the Reflex frame cap disappears with the proxy teardown when DLSS mode is left, NOT by
    // skipping this call.
    if (g_SlReflexSleep) {
        sl::Result sleepResult = g_SlReflexSleep(*token);
        if (sleepResult != sl::Result::eOk && frameIndex < 8) {
            testapp::Log("[FG-DIAG] slReflexSleep frame=%u result=%d (%s)\n", frameIndex,
                         static_cast<int>(sleepResult), SlResultName(sleepResult));
        }
    }
    return token;
}

static void SubmitStreamlineFrameInputs(sl::FrameToken* token, UINT frameIndex) {
    if (!token || !g_SlSetConstants || !g_SlSetTagForFrame || !g_FgInputs.valid ||
        frameIndex >= g_SwapChainBufferCount || !g_RenderTargets[frameIndex]) {
        return;
    }

    sl::Constants constants = {};
    constants.cameraViewToClip = IdentityMatrix();
    constants.clipToCameraView = IdentityMatrix();
    constants.clipToLensClip = IdentityMatrix();
    constants.clipToPrevClip = IdentityMatrix();
    constants.prevClipToClip = IdentityMatrix();
    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    // Scene motion vectors are emitted in UV space (prevUV - curUV). Streamline expects motion
    // normalized to [-1,1] (NDC): UV delta * 2 = NDC delta in x, and -2 in y (UV y is flipped
    // relative to NDC y).
    constants.mvecScale = sl::float2(2.0f, -2.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, -2.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.04719755f;
    constants.cameraAspectRatio = static_cast<float>(g_WindowWidth) / static_cast<float>(g_WindowHeight);
    constants.motionVectorsInvalidValue = 65504.0f;
    constants.depthInverted = sl::eFalse;
    constants.cameraMotionIncluded = sl::eFalse;
    constants.motionVectors3D = sl::eFalse;
    constants.reset = g_FrameTokenIndex < 4 ? sl::eTrue : sl::eFalse;
    sl::Result constantsResult = g_SlSetConstants(constants, *token, g_SlViewport);
    if (constantsResult != sl::Result::eOk && g_FrameTokenIndex < 8) {
        testapp::Log("[FG-DIAG] slSetConstants result=%d (%s)\n", static_cast<int>(constantsResult),
                     SlResultName(constantsResult));
    }

    sl::Extent extent = {};
    extent.width = static_cast<uint32_t>(g_WindowWidth);
    extent.height = static_cast<uint32_t>(g_WindowHeight);
    sl::Resource depth(sl::ResourceType::eTex2d, g_FgInputs.depth.Get(), testapp::dx12fg::kDepthReadState);
    sl::Resource motion(sl::ResourceType::eTex2d, g_FgInputs.motionVectors.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource hudless(sl::ResourceType::eTex2d, g_FgInputs.hudlessColor.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource ui(sl::ResourceType::eTex2d, g_FgInputs.uiColor.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource backbuffer(sl::ResourceType::eTex2d, g_RenderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT);
    depth.width = motion.width = hudless.width = ui.width = backbuffer.width = extent.width;
    depth.height = motion.height = hudless.height = ui.height = backbuffer.height = extent.height;
    depth.nativeFormat = testapp::dx12fg::kDepthFormat;
    motion.nativeFormat = testapp::dx12fg::kMotionVectorFormat;
    hudless.nativeFormat = testapp::dx12fg::kColorFormat;
    ui.nativeFormat = testapp::dx12fg::kColorFormat;
    backbuffer.nativeFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&backbuffer, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent, &extent),
    };
    sl::Result tagResult = g_SlSetTagForFrame(*token, g_SlViewport, tags, _countof(tags), g_CommandList.Get());
    if (tagResult != sl::Result::eOk || g_FrameTokenIndex < 5 || (g_FrameTokenIndex % 120) == 0) {
        testapp::Log("[FG-DIAG] slSetTagForFrame frame=%u result=%d (%s) depth=%p mvec=%p hudless=%p ui=%p backbuffer=%p\n",
                     g_FrameTokenIndex - 1, static_cast<int>(tagResult), SlResultName(tagResult),
                     g_FgInputs.depth.Get(), g_FgInputs.motionVectors.Get(), g_FgInputs.hudlessColor.Get(),
                     g_FgInputs.uiColor.Get(), g_RenderTargets[frameIndex].Get());
    }
}

static void ShutdownStreamline() {
    if (g_DlssEnabled && g_SlDLSSGSetOptions) {
        SetDLSSFGMode(false);
        g_DlssEnabled = false;
    }
    g_DlssSuspended = false;
    // All shutdown paths destroy the proxy swapchain and idle the GPU before reaching here
    // (ReleaseDX12RendererResourcesForSwitch / Cleanup), so the pacer is gone and a synchronous
    // Reflex eOff is safe. This is the ONLY place Reflex turns off (see SetDLSSFGMode invariant),
    // and it removes the lingering Reflex frame cap once DLSS mode is fully left.
    ApplyReflexMode(false, "Streamline shutdown");
    if (g_SlShutdown && g_SlInitialized) {
        sl::Result shutdownResult = g_SlShutdown();
        testapp::Log("[FG-DIAG] slShutdown result=%d (%s)\n", static_cast<int>(shutdownResult),
                     SlResultName(shutdownResult));
        g_SlInitialized = false;
    }
    if (g_SlModule) {
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
    }
    g_SlInit = nullptr;
    g_SlShutdown = nullptr;
    g_SlSetD3DDevice = nullptr;
    g_SlGetFeatureFunction = nullptr;
    g_SlGetNewFrameToken = nullptr;
    g_SlSetConstants = nullptr;
    g_SlSetTagForFrame = nullptr;
    g_SlDLSSGSetOptions = nullptr;
    g_SlDLSSGGetState = nullptr;
    g_SlReflexSetOptions = nullptr;
    g_SlReflexSleep = nullptr;
    g_SlPCLSetMarker = nullptr;
    g_SlCreateDXGIFactory1 = nullptr;
    g_SlD3D12CreateDevice = nullptr;
    g_SlInitialized = false;
    g_SlDeviceSet = false;
    g_DlssInitialized = false;
    g_ReflexLowLatencyActive = false;
    g_FrameTokenIndex = 0;
}
