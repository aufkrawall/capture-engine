    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Vertex, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 31, &D3D11ContextVTableOriginals::gsSetShaderResources, oGSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Geometry, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 59, &D3D11ContextVTableOriginals::hsSetShaderResources, oHSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Hull, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 63, &D3D11ContextVTableOriginals::dsSetShaderResources, oDSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Domain, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 67, &D3D11ContextVTableOriginals::csSetShaderResources, oCSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Compute, startSlot, numViews, ppShaderResourceViews);
}

static void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, oPSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Pixel, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 26, &D3D11ContextVTableOriginals::vsSetSamplers, oVSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Vertex, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 32, &D3D11ContextVTableOriginals::gsSetSamplers, oGSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Geometry, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 61, &D3D11ContextVTableOriginals::hsSetSamplers, oHSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Hull, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 65, &D3D11ContextVTableOriginals::dsSetSamplers, oDSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Domain, startSlot, numSamplers, ppSamplers);
}

static void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                                                    ID3D11SamplerState* const* ppSamplers) {
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 70, &D3D11ContextVTableOriginals::csSetSamplers, oCSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Compute, startSlot, numSamplers, ppSamplers);
}

static void ReconcilePixelSamplersBeforeDraw11(ID3D11DeviceContext* context) {
    if (!context || g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    if (g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, oPSSetSamplers11);
    if (!original) {
        return;
    }
    const uint32_t dirtyMask = ConsumePixelSamplerDirtyMask11(context);
    if (dirtyMask == 0) {
        return;
    }
    const int rebound = ReconcileStageSamplers11(original, context, D3D11ShaderStage::Pixel, 0,
                                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
    int idx = g_DiagSamplerDrawReconcileCalls.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLog("DX11: AF draw reconcile ctx=%p dirtyMask=0x%04X rebound=%d (#%d)", (void*)context, dirtyMask, rebound,
                idx + 1);
    }
}

static void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context, UINT indexCount,
                                                  UINT startIndexLocation, INT baseVertexLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexed11_t original =
        ResolveContextOriginal11(context, 12, &D3D11ContextVTableOriginals::drawIndexed, oDrawIndexed11);
    if (original) {
        original(context, indexCount, startIndexLocation, baseVertexLocation);
    }
}

static void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    Draw11_t original = ResolveContextOriginal11(context, 13, &D3D11ContextVTableOriginals::draw, oDraw11);
    if (original) {
        original(context, vertexCount, startVertexLocation);
    }
}

static void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                           UINT instanceCount, UINT startIndexLocation,
                                                           INT baseVertexLocation, UINT startInstanceLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstanced11_t original = ResolveContextOriginal11(
        context, 20, &D3D11ContextVTableOriginals::drawIndexedInstanced, oDrawIndexedInstanced11);
    if (original) {
        original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation,
                 startInstanceLocation);
    }
}

static void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                    UINT instanceCount, UINT startVertexLocation,
                                                    UINT startInstanceLocation) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstanced11_t original =
        ResolveContextOriginal11(context, 21, &D3D11ContextVTableOriginals::drawInstanced, oDrawInstanced11);
    if (original) {
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }
}

static void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawAuto11_t original = ResolveContextOriginal11(context, 38, &D3D11ContextVTableOriginals::drawAuto, oDrawAuto11);
    if (original) {
        original(context);
    }
}

static void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context,
                                                                   ID3D11Buffer* bufferForArgs,
                                                                   UINT alignedByteOffsetForArgs) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 39, &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, oDrawIndexedInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }
}

static void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context, ID3D11Buffer* bufferForArgs,
                                                            UINT alignedByteOffsetForArgs) {
    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 40, &D3D11ContextVTableOriginals::drawInstancedIndirect, oDrawInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }
}

static void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                                         BOOL restoreContextState) {
    int idx = g_DiagExecuteCommandList11.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLogImportant(
            "DX11: ExecuteCommandList ctx=%p commandList=%p restore=%d deferredContexts=%d "
            "drawReconcile=%d (#%d)",
            (void*)context, (void*)commandList, restoreContextState ? 1 : 0,
            g_DiagCreateDeferredContext11.load(std::memory_order_relaxed),
            g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed), idx + 1);
    }

    ExecuteCommandList11_t original =
        ResolveContextOriginal11(context, 58, &D3D11ContextVTableOriginals::executeCommandList, oExecuteCommandList11);
    if (original) {
        original(context, commandList, restoreContextState);
    }
    if (!restoreContextState) {
        ClearTrackedContextState11(context);
    }
}

// Use typedef from dx11_hook.h
// typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(...);
// Global original function pointer - set by IAT patching
PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = NULL;

// Local copy of the real original D3D11CreateDeviceAndSwapChain function address.
// HookExport calls PatchIATAllModules which overwrites the shared
// oD3D11CreateDeviceAndSwapChain (also used by wrapper_hooks.cpp) with the
// address of Wrapped_D3D11CreateDeviceAndSwapChain — causing the DX11 detour
// to call back into the wrapper instead of the real function, leading to
// infinite IAT recursion and stack overflow (0xC00000FD).
// Save the real GetProcAddress result separately so DetourD3D11CreateDeviceAndSwapChain
// can always reach the actual d3d11.dll code.
static PFN_D3D11CreateDeviceAndSwapChain s_oRealD3D11CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                           DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);
static PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDeviceAndSwapChain1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT,
                                                            D3D10_FEATURE_LEVEL1, UINT, DXGI_SWAP_CHAIN_DESC*,
                                                            IDXGISwapChain**, ID3D10Device1**);
static PFN_D3D10CreateDeviceAndSwapChain1 oD3D10CreateDeviceAndSwapChain1 = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device**);
static PFN_D3D10CreateDevice oD3D10CreateDevice = NULL;

typedef HRESULT(WINAPI* PFN_D3D10CreateDevice1)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1,
                                                UINT, ID3D10Device1**);
static PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
static PFN_CreateDXGIFactory oCreateDXGIFactory = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory1)(REFIID, void**);
static PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = NULL;

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
static PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = NULL;

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChain_t)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
static CreateSwapChain_t oCreateSwapChain = NULL;

typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainForHwnd_t)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
static CreateSwapChainForHwnd_t oCreateSwapChainForHwnd = NULL;

static ResizeBuffers_t oResizeBuffers = NULL;

// Forward Declarations (non-static for cross-file hook collision detection from
// dx12_hook.cpp) Helper to get VSync override settings (reduces duplication)
static VSyncOverride GetDX11VSyncOverride() {
    return GetVSyncOverride();  // Use the shared helper from hook_common.h
}

static bool ShouldSkipWindowForNvPresent(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return true;
    if (!IsWindowVisible(hwnd))
        return true;

    RECT clientRect = {};
    if (GetClientRect(hwnd, &clientRect) &&
        (clientRect.right <= clientRect.left || clientRect.bottom <= clientRect.top)) {
        return true;
    }

    return false;
}

// Forward Declarations
void CleanupDX11Resources(bool releaseDeviceContext = true);
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void DrawDX11Overlay(IDXGISwapChain* pSwapChain);
static void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);
static void InstallVTableHooks(ID3D11Device* pDevice, ID3D11DeviceContext* pContext, IDXGISwapChain* pSwapChain);

void ApplyDeferredSamplerOverrides11(IDXGISwapChain* pSwapChain) {
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return;

    ID3D11Device* dev = nullptr;
    HRESULT deviceHr = pSwapChain ? pSwapChain->GetDevice(IID_PPV_ARGS(&dev)) : E_POINTER;
    if (FAILED(deviceHr)) {
        static std::atomic<int> s_deviceFailLogs{0};
        int idx = s_deviceFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant("DX11: Deferred AF bootstrap skipped GetDevice failed hr=0x%08X sc=%p (#%d)", deviceHr,
                             (void*)pSwapChain, idx + 1);
        }
        return;
    }

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) {
        static std::atomic<int> s_noContextLogs{0};
        int idx = s_noContextLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant("DX11: Deferred AF bootstrap skipped no immediate context dev=%p sc=%p (#%d)", (void*)dev,
                             (void*)pSwapChain, idx + 1);
        }
        dev->Release();
        return;
    }

    if (IsDeferredAFBootstrapped11(ctx)) {
        ctx->Release();
        dev->Release();
        return;
    }

    const auto& gfx = GetActiveGraphicsConfig();
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
        int idx = g_DiagDeferredAFBootstrapDisabled.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant(
                "DX11: Deferred AF bootstrap postponed ctx=%p AF disabled/current='%s' configBackbuffers=%d (#%d)",
                (void*)ctx, gfx.anisotropicFiltering.c_str(), gfx.backbufferCount, idx + 1);
        }
        ctx->Release();
        dev->Release();
        return;
    }

    static ID3D11DeviceContext* s_LastRuntimeHookContext = nullptr;
    if (s_LastRuntimeHookContext != ctx || !oPSSetSamplers11 || !oPSSetShaderResources11) {
        const int beforeHooks = g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        InstallVTableHooks(dev, ctx, pSwapChain);
        const int installedDelta = g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed) - beforeHooks;
        static std::atomic<int> s_runtimeEnsureLogs{0};
        int idx = s_runtimeEnsureLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant(
                "DX11: Runtime AF hook ensure from Present ctx=%p installedDelta=%d hooks(psShader=%p psSRV=%p "
                "psSamp=%p createPS=%p createDeferred=%p draw=%p executeCL=%p)",
                (void*)ctx, installedDelta, (void*)oPSSetShader11, (void*)oPSSetShaderResources11,
                (void*)oPSSetSamplers11, (void*)oCreatePixelShader11, (void*)oCreateDeferredContext11, (void*)oDraw11,
                (void*)oExecuteCommandList11);
        }
        s_LastRuntimeHookContext = ctx;
    }

    RefreshPixelShaderFromContext11(ctx);

    struct StageSweep {
        D3D11ShaderStage stage;
    };

    StageSweep stages[] = {
        {D3D11ShaderStage::Pixel}, {D3D11ShaderStage::Vertex}, {D3D11ShaderStage::Geometry},
        {D3D11ShaderStage::Hull},  {D3D11ShaderStage::Domain}, {D3D11ShaderStage::Compute},
    };

    int totalBoundSamplers = 0;
    for (const auto& si : stages) {
        RefreshStageShaderResourcesFromContext11(ctx, si.stage, 0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        RefreshStageSamplersFromContext11(ctx, si.stage, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
            ID3D11SamplerState* sampler = GetTrackedSampler11(ctx, si.stage, slot);
            if (sampler) {
                ++totalBoundSamplers;
                sampler->Release();
            }
        }
    }

    if (totalBoundSamplers > 0) {
        MarkDeferredAFBootstrapped11(ctx);
        int idx = g_DiagDeferredAFBootstrapComplete.fetch_add(1, std::memory_order_relaxed);
        HookLogImportant(
            "DX11: Deferred AF bootstrap complete ctx=%p dev=%p sc=%p boundSamplers=%d "
            "drawReconcile=deferred (#%d)",
            (void*)ctx, (void*)dev, (void*)pSwapChain, totalBoundSamplers, idx + 1);
    } else {
        int idx = g_DiagDeferredAFBootstrapRetry.fetch_add(1, std::memory_order_relaxed);
        if (idx < 16) {
            HookLogImportant("DX11: Deferred AF bootstrap found no samplers ctx=%p dev=%p sc=%p - will retry (#%d)",
                             (void*)ctx, (void*)dev, (void*)pSwapChain, idx + 1);
        }
    }
    ctx->Release();
    dev->Release();
}

static float ResolveDX11PrerenderLimit() {
    return GetActivePrerenderLimit();
}

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    if (g_pSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
        perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
    }

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL ULTIMATE FIX: If shutdown flag is set, return immediately WITHOUT
    // touching ANYTHING - no wrapper checks, no GetDesc, nothing. Just return.
    // The device may already be destroyed and any D3D call can crash.
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
    // The wrapper sets a thread-local flag before calling the real Present
    extern bool IsInWrapperPresent();
    bool inWrapper = IsInWrapperPresent();
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount.fetch_add(1) < 10) {
        HookLog("DetourDX11Present: IsInWrapperPresent=%d", inWrapper);
    }
    if (inWrapper) {
        // Wrapper is handling everything, just call original
        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        return oPresent(pSwapChain, SyncInterval, Flags);
    }
    g_InPresentHook = true;
    auto hookGuard = ce::make_scope_guard([&] { g_InPresentHook = false; });

    // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
    // This prevents crashes when the app is shutting down and destroying its
    // window
    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        // NVIDIA Smooth Motion compatibility: NvPresent64 can present through
        // hidden/ephemeral windows that must be passed through untouched.
        if (nvPresentLoaded && ShouldSkipWindowForNvPresent(desc.OutputWindow)) {
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            // Window is being destroyed - app is shutting down
            // Set shutdown flag and bail immediately without touching any D3D objects
            RequestHookShutdown();
            EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode", desc.OutputWindow);
            return S_OK;
        }
    }

    // Process VSync Override
    VSyncOverride override = GetDX11VSyncOverride();
    if (override.shouldOverride) {
        // Skip forcing VSync when the FPS limiter is actively pacing frames.
        // The limiter's sleep-before-Present controls frame timing; SyncInterval=1
        // would add a vblank wait after and override our target FPS.
        if (!g_SharedFpsLimiter.IsActivelyLimiting()) {
            SyncInterval = override.presentInterval;
        }
        if (override.useMailbox) {
            Flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    // Non-wrapper path: Draw overlay via vtable hook
    // Also bootstrap forced-AF state for games that bound samplers before hooks.
    ApplyDeferredSamplerOverrides11(pSwapChain);
    int64_t overlayStartUs = PerfLogger::GetQpcUs();
    HandleDX11ProcessFrame(pSwapChain, true);
    perfMetrics.overlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);

    // If window was invalid during overlay rendering, skip Present to avoid crash
    // The app is already tearing down its D3D resources
    if (HookIsShuttingDown()) {
        return S_OK;  // Return success to avoid cascading errors
    }

    // CPU Prerender Limit
    float prerenderLimit = ResolveDX11PrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        int64_t prerenderStartUs = PerfLogger::GetQpcUs();
        ApplyPrerenderLimit(pSwapChain, prerenderLimit);
        perfMetrics.prerenderWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - prerenderStartUs);
    }

    // FPS Limiter
    int64_t fpsLimitStartUs = PerfLogger::GetQpcUs();
    g_SharedFpsLimiter.Apply();
    perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - fpsLimitStartUs);

    return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                             const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for shutdown first - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
    extern bool IsInWrapperPresent();
    if (IsInWrapperPresent()) {
        // Wrapper is handling everything, just call original
        if (oPresent1)
            return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }

    // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        // NVIDIA Smooth Motion compatibility: pass through hidden/ephemeral windows
        if (nvPresentLoaded && ShouldSkipWindowForNvPresent(desc.OutputWindow)) {
            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            RequestHookShutdown();
            EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode", desc.OutputWindow);
            return S_OK;
        }
    }

    // Vulkan coordination: Skip DX11 overlay if Vulkan Layer is active AND
    // presenting.
    if (g_pSharedMem) {
        uint64_t lastVulkan = g_pSharedMem->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
        if (g_pSharedMem->runtimeState.vulkanLayerActive && (GetTickCount64() - lastVulkan < 200)) {
            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }
    }

    // SAFETY CHECK: DX12 Detection
    // If this swapchain is actually DX12, we must NOT draw the DX11 overlay.
    {
        ID3D12Device* d12Dev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d12Dev))) {
            d12Dev->Release();

            // DX12 detected. Skip DX11 overlay but delegate to DX12 for frame
            // processing.
            static bool s_LoggedDX12Mismatch = false;
            if (!s_LoggedDX12Mismatch) {
                HookLog(
                    "DX11: DetourDX11Present1 - DX12 Device detected! Delegating "
                    "to DX12_ProcessFrameExternal.");
                s_LoggedDX12Mismatch = true;
            }

            // Delegate to DX12 hook for overlay/capture processing
            extern void DX12_ProcessFrameExternal(IDXGISwapChain * pSwapChain);
            DX12_ProcessFrameExternal(pSwapChain);

            if (oPresent1)
                return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return oPresent(pSwapChain, SyncInterval, PresentFlags);
        }
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        if (oPresent1)
            return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }
    g_InPresentHook = true;
    auto hookGuard = ce::make_scope_guard([&] { g_InPresentHook = false; });

    // Process VSync Override
    VSyncOverride override = GetDX11VSyncOverride();
    if (override.shouldOverride) {
        SyncInterval = override.presentInterval;
        if (override.useMailbox) {
            PresentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    // Non-wrapper path: Process overlay, capture, and screenshots via the shared ordering helper
    HandleDX11ProcessFrame(pSwapChain, true);

    // If window was invalid during overlay rendering, skip Present to avoid crash
    if (HookIsShuttingDown()) {
        return S_OK;
    }

    // CPU Prerender Limit
    float prerenderLimit = ResolveDX11PrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        ApplyPrerenderLimit(pSwapChain, prerenderLimit);
    }

    if (oPresent1)
        return oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
    // Fallback to Present if Present1 not hooked (should not happen if vtable
