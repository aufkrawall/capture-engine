// Included by dx12_fg_switch_test.cpp; shares that file's static DX12/FG state.
//
// Super-resolution upscaling stage: fills the display-resolution g_FgInputs.hudlessColor from the
// render-resolution scene inputs (sceneColor/motionVectors/depth + masks), per run mode:
//   OFF  -> hand-rolled TAA/TAAU (testapp/dx12_fg_taa.h, pure DX12)
//   DLSS -> Streamline kFeatureDLSS (slEvaluateFeature on the app command list)
//   FSR  -> FFX API upscale context (amd_fidelityfx_upscaler_dx12.dll, loaded ISOLATED from the
//           validated FG runtime; every FFX kit DLL exports the full ffx* entry points)
// If a vendor upscaler is unavailable, the mode falls back to TAA/TAAU so the image stays correct
// and frame generation keeps working.

static bool UpscalingActive() {
    return g_UpscalingEnabled;
}

// ---- FSR upscaler runtime (isolated module; never reload-stressed like the FG runtime) ----

static bool LoadFSRUpscalerRuntime() {
    if (g_FfxUpscalerModule) {
        return true;
    }
    g_FfxUpscalerModule = LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
    if (!g_FfxUpscalerModule) {
        testapp::Log("[FG-DIAG] FSR upscaler DLL not found (amd_fidelityfx_upscaler_dx12.dll, err=%lu)\n",
                     GetLastError());
        return false;
    }
    g_FfxUpCreateContext =
        reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_FfxUpscalerModule, "ffxCreateContext"));
    g_FfxUpDispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_FfxUpscalerModule, "ffxDispatch"));
    g_FfxUpQuery = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_FfxUpscalerModule, "ffxQuery"));
    g_FfxUpDestroyContext =
        reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_FfxUpscalerModule, "ffxDestroyContext"));
    if (!g_FfxUpCreateContext || !g_FfxUpDispatch || !g_FfxUpQuery || !g_FfxUpDestroyContext) {
        testapp::Log("[FG-DIAG] FSR upscaler DLL missing ffx exports\n");
        FreeLibrary(g_FfxUpscalerModule);
        g_FfxUpscalerModule = nullptr;
        g_FfxUpCreateContext = nullptr;
        g_FfxUpDispatch = nullptr;
        g_FfxUpQuery = nullptr;
        g_FfxUpDestroyContext = nullptr;
        return false;
    }
    testapp::Log("[FG-DIAG] FSR upscaler runtime loaded base=%p create=%p dispatch=%p query=%p\n",
                 g_FfxUpscalerModule, reinterpret_cast<void*>(g_FfxUpCreateContext),
                 reinterpret_cast<void*>(g_FfxUpDispatch), reinterpret_cast<void*>(g_FfxUpQuery));
    return true;
}

static void UnloadFSRUpscalerRuntime(const char* reason) {
    if (g_FfxUpscaleCtx) {
        testapp::Log("[FG-DIAG] WARN unloading FSR upscaler with live context (%s)\n", reason ? reason : "unknown");
    }
    if (g_FfxUpscalerModule) {
        testapp::Log("[FG-DIAG] Unloading FSR upscaler runtime base=%p reason=%s\n", g_FfxUpscalerModule,
                     reason ? reason : "unknown");
        FreeLibrary(g_FfxUpscalerModule);
    }
    g_FfxUpscalerModule = nullptr;
    g_FfxUpCreateContext = nullptr;
    g_FfxUpDispatch = nullptr;
    g_FfxUpQuery = nullptr;
    g_FfxUpDestroyContext = nullptr;
}

static void FfxUpscaleMessageCallback(uint32_t type, const wchar_t* message) {
    testapp::Log("[FG-DIAG] FFX upscale message type=%u %S\n", type, message ? message : L"");
}

// Picks the provider version: FSR4 needs WMMA-capable hardware (RDNA4); on other GPUs the runtime
// auto-selects FSR 3.1.x. Config fsr_version=3|4 forces a provider whose name carries that major
// version; if no match exists the runtime default is used with a warning (graceful FSR4->FSR3
// fallback on unsupported hardware).
static uint64_t ChooseFSRUpscaleVersionOverride() {
    if (!g_FfxUpQuery || !g_Device) {
        return 0;
    }
    ffxQueryDescGetVersions versionsQuery = {};
    versionsQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionsQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versionsQuery.device = g_Device.Get();
    uint64_t count = 0;
    versionsQuery.outputCount = &count;
    ffxReturnCode_t ret = g_FfxUpQuery(nullptr, &versionsQuery.header);
    if (ret != FFX_API_RETURN_OK || count == 0 || count > 16) {
        testapp::Log("[FG-DIAG] FSR upscale version enumeration result=%u (%s) count=%llu\n", ret, FfxReturnName(ret),
                     static_cast<unsigned long long>(count));
        return 0;
    }
    uint64_t ids[16] = {};
    const char* names[16] = {};
    versionsQuery.versionIds = ids;
    versionsQuery.versionNames = names;
    ret = g_FfxUpQuery(nullptr, &versionsQuery.header);
    if (ret != FFX_API_RETURN_OK) {
        return 0;
    }
    uint64_t override = 0;
    for (uint64_t i = 0; i < count; ++i) {
        int major = 0;
        if (names[i]) {
            const char* p = names[i];
            while (*p && (*p < '0' || *p > '9')) {
                ++p;
            }
            major = atoi(p);
        }
        testapp::Log("[FG-DIAG] FSR upscale provider[%llu] id=0x%llx name='%s' major=%d\n",
                     static_cast<unsigned long long>(i), static_cast<unsigned long long>(ids[i]),
                     names[i] ? names[i] : "?", major);
        if (g_FsrUpscaleVersionConfig != 0 && major == g_FsrUpscaleVersionConfig && override == 0) {
            override = ids[i];
        }
    }
    if (g_FsrUpscaleVersionConfig != 0 && override == 0) {
        testapp::Log("[FG-DIAG] WARN requested FSR upscale major version %d not available; using runtime default\n",
                     g_FsrUpscaleVersionConfig);
    }
    return override;
}

static void DestroyFSRUpscaleContext() {
    if (g_FfxUpDestroyContext && g_FfxUpscaleCtx) {
        g_FfxUpDestroyContext(&g_FfxUpscaleCtx, nullptr);
        testapp::Log("[FG-DIAG] FSR upscale context destroyed\n");
    }
    g_FfxUpscaleCtx = nullptr;
}

static bool TryInitFSRUpscaleContext() {
    if (g_FfxUpscaleCtx) {
        return true;
    }
    if (!g_Device || !LoadFSRUpscalerRuntime()) {
        return false;
    }

    const uint64_t versionOverride = ChooseFSRUpscaleVersionOverride();

    ffxCreateContextDescUpscaleVersion versionDesc = {};
    versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    versionDesc.version = FFX_UPSCALER_VERSION;

    ffxCreateBackendDX12Desc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.header.pNext = &versionDesc.header;
    backendDesc.device = g_Device.Get();

    ffxOverrideVersion overrideDesc = {};
    if (versionOverride != 0) {
        overrideDesc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        overrideDesc.versionId = versionOverride;
        versionDesc.header.pNext = &overrideDesc.header;
    }

    ffxCreateContextDescUpscale createDesc = {};
    createDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    createDesc.header.pNext = &backendDesc.header;
    // The scene color input is FP16 LINEAR (kHdrColorFormat) -> HIGH_DYNAMIC_RANGE; exposure is
    // derived automatically (no exposure texture provided).
    createDesc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    createDesc.maxRenderSize = {static_cast<uint32_t>(g_RenderWidth), static_cast<uint32_t>(g_RenderHeight)};
    createDesc.maxUpscaleSize = {static_cast<uint32_t>(g_WindowWidth), static_cast<uint32_t>(g_WindowHeight)};
    createDesc.fpMessage = FfxUpscaleMessageCallback;

    ffxReturnCode_t ret = g_FfxUpCreateContext(&g_FfxUpscaleCtx, &createDesc.header, nullptr);
    if (ret != FFX_API_RETURN_OK || !g_FfxUpscaleCtx) {
        testapp::Log("[FG-DIAG] ffxCreateContext(UPSCALE) FAILED code=%u (%s) render=%dx%d display=%dx%d\n", ret,
                     FfxReturnName(ret), g_RenderWidth, g_RenderHeight, g_WindowWidth, g_WindowHeight);
        g_FfxUpscaleCtx = nullptr;
        return false;
    }

    ffxQueryGetProviderVersion providerQuery = {};
    providerQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (g_FfxUpQuery(&g_FfxUpscaleCtx, &providerQuery.header) == FFX_API_RETURN_OK) {
        testapp::Log("[FG-DIAG] ffxCreateContext(UPSCALE) OK ctx=%p provider id=0x%llx name='%s' "
                     "render=%dx%d display=%dx%d quality=%s\n",
                     (void*)g_FfxUpscaleCtx, static_cast<unsigned long long>(providerQuery.versionId),
                     providerQuery.versionName ? providerQuery.versionName : "?", g_RenderWidth, g_RenderHeight,
                     g_WindowWidth, g_WindowHeight, testapp::fg::UpscaleQualityName(g_UpscaleQuality));
    } else {
        testapp::Log("[FG-DIAG] ffxCreateContext(UPSCALE) OK ctx=%p (provider version query failed)\n",
                     (void*)g_FfxUpscaleCtx);
    }
    testapp::LogFlush();
    return true;
}

static void DispatchFSRUpscale(float frameDeltaMs, bool reset) {
    if (!g_FfxUpDispatch || !g_FfxUpscaleCtx || !g_FgInputs.valid) {
        return;
    }
    const testapp::dx12fg::SceneCamera& camera = g_Scene.Camera();

    // The upscaler writes the display-res hudless color as a UAV.
    testapp::dx12fg::Transition(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), g_FgInputs.hudlessState,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ffxDispatchDescUpscale upscale = {};
    upscale.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    upscale.commandList = g_CommandList.Get();
    // NOTE: FFX_API_RESOURCE_STATE_* values are FFX flags, NOT D3D12 states. The render-res inputs
    // sit in PIXEL|NON_PIXEL shader-resource D3D12 state == FFX PIXEL_COMPUTE_READ.
    upscale.color = ffxApiGetResourceDX12(g_FgInputs.sceneColor.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    upscale.depth = ffxApiGetResourceDX12(g_FgInputs.depth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    upscale.motionVectors =
        ffxApiGetResourceDX12(g_FgInputs.motionVectors.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    // Exposure omitted: FFX_UPSCALE_ENABLE_AUTO_EXPOSURE is set on the context.
    upscale.reactive = ffxApiGetResourceDX12(g_FgInputs.reactiveMask.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    upscale.transparencyAndComposition =
        ffxApiGetResourceDX12(g_FgInputs.transparencyMask.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    upscale.output = ffxApiGetResourceDX12(g_FgInputs.hudlessColor.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    upscale.jitterOffset = {g_CurrentJitter.x, g_CurrentJitter.y};
    // Scene motion is UV-space prevUV - curUV; FFX expects pixel-space motion at render res.
    upscale.motionVectorScale = {static_cast<float>(g_RenderWidth), static_cast<float>(g_RenderHeight)};
    upscale.renderSize = {static_cast<uint32_t>(g_RenderWidth), static_cast<uint32_t>(g_RenderHeight)};
    upscale.upscaleSize = {static_cast<uint32_t>(g_WindowWidth), static_cast<uint32_t>(g_WindowHeight)};
    upscale.enableSharpening = g_FsrSharpeningEnabled;
    upscale.sharpness = static_cast<float>(g_FsrSharpnessPercent) / 100.0f;
    upscale.frameTimeDelta = frameDeltaMs;
    upscale.preExposure = 1.0f;
    upscale.reset = reset;
    upscale.cameraNear = camera.valid ? camera.nearZ : 0.1f;
    upscale.cameraFar = camera.valid ? camera.farZ : 1000.0f;
    upscale.cameraFovAngleVertical = camera.valid ? camera.fovY : 1.04719755f;
    upscale.viewSpaceToMetersFactor = 1.0f;
    upscale.flags = 0;

    ffxReturnCode_t ret = g_FfxUpDispatch(&g_FfxUpscaleCtx, &upscale.header);
    if (ret != FFX_API_RETURN_OK || g_FrameIdCounter < 5 || (g_FrameIdCounter % 240) == 0) {
        testapp::Log("[FG-DIAG] ffxDispatch(upscale) frameID=%llu result=%u (%s) render=%dx%d jitter=(%.3f,%.3f) "
                     "reset=%d deltaMs=%.2f\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ret, FfxReturnName(ret), g_RenderWidth,
                     g_RenderHeight, g_CurrentJitter.x, g_CurrentJitter.y, reset ? 1 : 0, frameDeltaMs);
    }

    testapp::dx12fg::Transition(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), g_FgInputs.hudlessState,
                                testapp::dx12fg::kColorReadState);
}

// ---- DLSS Super Resolution (Streamline kFeatureDLSS) ----

static sl::DLSSMode MapDLSSSRMode() {
    // With a custom scale override pick the DLSS quality mode whose ratio is closest; DLSS accepts
    // any render size within the optimal-settings min/max for the chosen mode.
    if (g_UpscaleScalePercent > 0) {
        const double ratio = 100.0 / static_cast<double>(g_UpscaleScalePercent);
        if (ratio < 1.05) {
            return sl::DLSSMode::eDLAA;
        }
        if (ratio < 1.6) {
            return sl::DLSSMode::eMaxQuality;
        }
        if (ratio < 1.85) {
            return sl::DLSSMode::eBalanced;
        }
        if (ratio < 2.5) {
            return sl::DLSSMode::eMaxPerformance;
        }
        return sl::DLSSMode::eUltraPerformance;
    }
    switch (g_UpscaleQuality) {
        case testapp::fg::UpscaleQuality::Quality:
            return sl::DLSSMode::eMaxQuality;
        case testapp::fg::UpscaleQuality::Balanced:
            return sl::DLSSMode::eBalanced;
        case testapp::fg::UpscaleQuality::Performance:
            return sl::DLSSMode::eMaxPerformance;
        case testapp::fg::UpscaleQuality::UltraPerformance:
            return sl::DLSSMode::eUltraPerformance;
        case testapp::fg::UpscaleQuality::NativeAA:
        default:
            return sl::DLSSMode::eDLAA;
    }
}

static sl::DLSSPreset MapDLSSPreset() {
    switch (g_DlssPresetConfig) {
        case 'j':
            return sl::DLSSPreset::ePresetJ;
        case 'k':
            return sl::DLSSPreset::ePresetK;
        case 'l':
            return sl::DLSSPreset::ePresetL;
        case 'm':
            return sl::DLSSPreset::ePresetM;
        default:
            return sl::DLSSPreset::eDefault;
    }
}

// Configures DLSS SR for the current quality/preset; logs the optimal settings so a mismatch
// between the app-chosen render size and the DLSS-supported range is visible in the log.
static bool SetDLSSSROptions(bool enable) {
    g_DlssSrActive = false;
    if (!g_SlDLSSSetOptions) {
        if (enable) {
            testapp::Log("[FG-DIAG] DLSS SR unavailable (slDLSSSetOptions unresolved); TAA fallback active\n");
        }
        return false;
    }

    sl::DLSSOptions options = {};
    options.mode = enable ? MapDLSSSRMode() : sl::DLSSMode::eOff;
    options.outputWidth = static_cast<uint32_t>(g_WindowWidth);
    options.outputHeight = static_cast<uint32_t>(g_WindowHeight);
    // The scene/upscale chain is FP16 LINEAR (kHdrColorFormat): tell DLSS the color buffers are
    // HDR/linear and let it derive exposure (no exposure texture is tagged).
    options.colorBuffersHDR = sl::Boolean::eTrue;
    options.useAutoExposure = sl::Boolean::eTrue;
    const sl::DLSSPreset preset = MapDLSSPreset();
    options.dlaaPreset = preset;
    options.qualityPreset = preset;
    options.balancedPreset = preset;
    options.performancePreset = preset;
    options.ultraPerformancePreset = preset;
    options.ultraQualityPreset = preset;

    if (enable && g_SlDLSSGetOptimalSettings) {
        sl::DLSSOptimalSettings optimal = {};
        if (g_SlDLSSGetOptimalSettings(options, optimal) == sl::Result::eOk) {
            testapp::Log("[FG-DIAG] DLSS SR optimal: render=%ux%u min=%ux%u max=%ux%u (app render=%dx%d)%s\n",
                         optimal.optimalRenderWidth, optimal.optimalRenderHeight, optimal.renderWidthMin,
                         optimal.renderHeightMin, optimal.renderWidthMax, optimal.renderHeightMax, g_RenderWidth,
                         g_RenderHeight,
                         (static_cast<uint32_t>(g_RenderWidth) < optimal.renderWidthMin ||
                          static_cast<uint32_t>(g_RenderWidth) > optimal.renderWidthMax)
                             ? " WARN app render width outside DLSS range"
                             : "");
        }
    }

    sl::Result ret = g_SlDLSSSetOptions(g_SlViewport, options);
    testapp::Log("[FG-DIAG] slDLSSSetOptions(mode=%u preset=%u) output=%ux%u result=%d (%s)\n",
                 static_cast<uint32_t>(options.mode), static_cast<uint32_t>(preset), options.outputWidth,
                 options.outputHeight, static_cast<int>(ret), SlResultName(ret));
    testapp::LogFlush();
    g_DlssSrActive = enable && ret == sl::Result::eOk;
    return g_DlssSrActive;
}

// Runs the DLSS SR pass on the app command list. Requires the frame's constants + resource tags
// (incl. kBufferTypeScalingInputColor/kBufferTypeScalingOutputColor) to be set for this frame
// token BEFORE the call -- SubmitStreamlineFrameInputs runs earlier in the frame.
static void EvaluateDLSSSR(sl::FrameToken* frameToken) {
    if (!frameToken || !g_SlEvaluateFeature || !g_DlssSrActive) {
        return;
    }
    const sl::BaseStructure* inputs[] = {&g_SlViewport};
    sl::Result ret = g_SlEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), g_CommandList.Get());
    if (ret != sl::Result::eOk || g_FrameIdCounter < 5 || (g_FrameIdCounter % 240) == 0) {
        testapp::Log("[FG-DIAG] slEvaluateFeature(DLSS) frameID=%llu result=%d (%s) render=%dx%d jitter=(%.3f,%.3f)\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), static_cast<int>(ret), SlResultName(ret),
                     g_RenderWidth, g_RenderHeight, g_CurrentJitter.x, g_CurrentJitter.y);
    }
}

// Heartbeat/diagnostics: which upscaler actually serves the current mode this frame.
static const char* ActiveUpscalerName() {
    if (!UpscalingActive()) {
        return "off";
    }
    if (g_CurrentMode == FGMode::DLSS) {
        return g_DlssSrActive ? "dlss-sr" : "taa-fallback";
    }
    if (g_CurrentMode == FGMode::FSR) {
        return g_FfxUpscaleCtx ? "fsr" : "taa-fallback";
    }
    return "taa";
}

// ---- Per-frame upscale stage dispatcher ----

static void RunUpscaleStage(sl::FrameToken* frameToken, UINT frameIndex, float frameDeltaMs) {
    if (!UpscalingActive() || !g_FgInputs.valid) {
        return;
    }
    const bool reset =
        g_FrameIdCounter < 4 || (g_LastModeSwitchFrameId != 0 && g_FrameIdCounter - g_LastModeSwitchFrameId <= 1);

    if (g_CurrentMode == FGMode::DLSS && g_DlssSrActive) {
        EvaluateDLSSSR(frameToken);
        return;
    }
    if (g_CurrentMode == FGMode::FSR && g_FfxUpscaleCtx) {
        DispatchFSRUpscale(frameDeltaMs, reset);
        return;
    }

    // OFF mode, and the graceful fallback when a vendor upscaler is unavailable.
    static FGMode s_lastFallbackLogMode = FGMode::Off;
    static bool s_loggedFallback = false;
    if (g_CurrentMode != FGMode::Off && (!s_loggedFallback || s_lastFallbackLogMode != g_CurrentMode)) {
        s_loggedFallback = true;
        s_lastFallbackLogMode = g_CurrentMode;
        testapp::Log("[FG-DIAG] Upscale stage: %s has no vendor upscaler context; using TAA/TAAU fallback\n",
                     ModeName(g_CurrentMode));
    }
    if (reset) {
        g_Taa.Reset();
    }
    g_Taa.Render(g_CommandList.Get(), g_FgInputs, frameIndex, g_CurrentJitter.x, g_CurrentJitter.y);
}
