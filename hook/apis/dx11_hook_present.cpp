#include "dx11_hook_internal.h"

namespace {

HRESULT CallOriginalPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    return dx11_hook_oPresent ? dx11_hook_oPresent(swapChain, syncInterval, flags) : DXGI_ERROR_INVALID_CALL;
}

HRESULT CallOriginalPresent1(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags,
                             const DXGI_PRESENT_PARAMETERS* parameters) {
    if (dx11_hook_oPresent1)
        return dx11_hook_oPresent1(swapChain, syncInterval, flags, parameters);
    return CallOriginalPresent(swapChain, syncInterval, flags);
}

}  // namespace

bool DX11Hook_ShouldPassThroughCurrentPresent() {
    return dx11_hook_g_UnsafeSwapChainObserved;
}

void ApplyDeferredSamplerOverrides11(IDXGISwapChain* pSwapChain) {
    if (HookIsShuttingDown() || !g_GraphicsOverridesActive.load(std::memory_order_acquire))
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
        int idx = dx11_hook_g_DiagDeferredAFBootstrapDisabled.fetch_add(1, std::memory_order_relaxed);
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
    if (s_LastRuntimeHookContext != ctx || !dx11_hook_oPSSetSamplers11 || !dx11_hook_oPSSetShaderResources11) {
        const int beforeHooks = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        InstallVTableHooks(dev, ctx, pSwapChain);
        const int installedDelta = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed) - beforeHooks;
        static std::atomic<int> s_runtimeEnsureLogs{0};
        int idx = s_runtimeEnsureLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant(
                "DX11: Runtime AF hook ensure from Present ctx=%p installedDelta=%d hooks(psShader=%p psSRV=%p "
                "psSamp=%p createPS=%p createDeferred=%p draw=%p executeCL=%p)",
                (void*)ctx, installedDelta, (void*)dx11_hook_oPSSetShader11, (void*)dx11_hook_oPSSetShaderResources11,
                (void*)dx11_hook_oPSSetSamplers11, (void*)dx11_hook_oCreatePixelShader11, (void*)dx11_hook_oCreateDeferredContext11, (void*)dx11_hook_oDraw11,
                (void*)dx11_hook_oExecuteCommandList11);
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
        int idx = dx11_hook_g_DiagDeferredAFBootstrapComplete.fetch_add(1, std::memory_order_relaxed);
        HookLogImportant(
            "DX11: Deferred AF bootstrap complete ctx=%p dev=%p sc=%p boundSamplers=%d "
            "drawReconcile=deferred (#%d)",
            (void*)ctx, (void*)dev, (void*)pSwapChain, totalBoundSamplers, idx + 1);
    } else {
        int idx = dx11_hook_g_DiagDeferredAFBootstrapRetry.fetch_add(1, std::memory_order_relaxed);
        if (idx < 16) {
            HookLogImportant("DX11: Deferred AF bootstrap found no samplers ctx=%p dev=%p sc=%p - will retry (#%d)",
                             (void*)ctx, (void*)dev, (void*)pSwapChain, idx + 1);
        }
    }
    ctx->Release();
    dev->Release();
}

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    // Dormant hooks remain installed so game-held and foreign-hook pointers stay
    // valid. Forward before diagnostics or config reads can touch retired host
    // state.
    if (HookIsShuttingDown()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX11", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = sharedMemory->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = sharedMemory->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (sharedMemory->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            sharedMemory->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
    perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
    perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
    perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
    perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
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
        return dx11_hook_oPresent(pSwapChain, SyncInterval, Flags);
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        return dx11_hook_oPresent(pSwapChain, SyncInterval, Flags);
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
            return dx11_hook_oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            EarlyLog("DX11: Window unavailable (hwnd=%p), passing through this Present", desc.OutputWindow);
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
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
    if (HookIsShuttingDown() || DX11Hook_ShouldPassThroughCurrentPresent()) {
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
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

    return dx11_hook_oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourDX11Present1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                             const DXGI_PRESENT_PARAMETERS* pPresentParameters) {
    // CRITICAL: Check for shutdown first - if app is closing, don't touch
    // anything
    if (HookIsShuttingDown()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
    }

    // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
    extern bool IsInWrapperPresent();
    if (IsInWrapperPresent()) {
        // Wrapper is handling everything, just call original
        if (dx11_hook_oPresent1)
            return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
    }

    // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
    const bool nvPresentLoaded = g_FGCompat.IsNvPresentLoaded();
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
        // NVIDIA Smooth Motion compatibility: pass through hidden/ephemeral windows
        if (nvPresentLoaded && ShouldSkipWindowForNvPresent(desc.OutputWindow)) {
            if (dx11_hook_oPresent1)
                return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
        }

        if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
            EarlyLog("DX11: Window unavailable (hwnd=%p), passing through this Present1", desc.OutputWindow);
            return CallOriginalPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        }
    }

    // Vulkan coordination: Skip DX11 overlay if Vulkan Layer is active AND
    // presenting.
    if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
        uint64_t lastVulkan = sharedMemory->runtimeState.vulkanPresentTick.load(std::memory_order_acquire);
        if (sharedMemory->runtimeState.vulkanLayerActive && (GetTickCount64() - lastVulkan < 200)) {
            if (dx11_hook_oPresent1)
                return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
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

            if (dx11_hook_oPresent1)
                return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
            return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
        }
    }

    // Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
    if (g_InPresentHook) {
        if (dx11_hook_oPresent1)
            return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
        return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
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
    if (HookIsShuttingDown() || DX11Hook_ShouldPassThroughCurrentPresent()) {
        return CallOriginalPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
    }

    // CPU Prerender Limit
    float prerenderLimit = ResolveDX11PrerenderLimit();
    if (prerenderLimit >= 0.0f) {
        ApplyPrerenderLimit(pSwapChain, prerenderLimit);
    }

    if (dx11_hook_oPresent1)
        return dx11_hook_oPresent1(pSwapChain, SyncInterval, PresentFlags, pPresentParameters);
    // Fallback to Present if Present1 not hooked (should not happen if vtable

    // hooked correctly)
    return dx11_hook_oPresent(pSwapChain, SyncInterval, PresentFlags);
}

// Reentrancy guard for ResizeBuffers (Recursion Breaker)
thread_local int g_ResizeBuffersDepth = 0;

// Handle SwapChain resize - must release RTV and reinitialize ImGui
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    // Preserve the application's exact arguments while dormant. In particular,
    // do not add CE's waitable-object flag before forwarding.
    if (HookIsShuttingDown()) {
        return dx11_hook_oResizeBuffers
                   ? dx11_hook_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags)
                   : DXGI_ERROR_INVALID_CALL;
    }
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // RECURSION BREAKER: If we are calling ourselves recursively, bail out
    // immediately. This handles the "Hooked the Hook" scenario or infinite
    // unhook/rehook loops.
    if (g_ResizeBuffersDepth > 0) {
        // WrapperLog("DX11: ResizeBuffers recursion detected! Bailing to prevent
        // crash."); We must call original if possible, but if original points to
        // us, we can't. If oResizeBuffers == DetourResizeBuffers, we are stuck.
        // Safe bet: just return S_OK to stop the madness.
        return S_OK;
    }
    g_ResizeBuffersDepth++;

    // Guard for auto-resetting depth
    auto depthGuard = ce::make_scope_guard([&] { g_ResizeBuffersDepth--; });

    HookLog("DX11: ResizeBuffers called (%dx%d)", Width, Height);

    // Safety: Check if oResizeBuffers is valid
    if (!dx11_hook_oResizeBuffers) {
        HookLog("DX11: ResizeBuffers - Original function pointer is NULL! Bailing.");
        return S_OK;
    }

    // Safety: Check if oResizeBuffers points to US (Cycle Detection)
    if ((void*)dx11_hook_oResizeBuffers == (void*)DetourResizeBuffers) {
        HookLog(
            "DX11: ResizeBuffers - Original function points to DETOUR! Cycle "
            "detected. Bailing.");
        return S_OK;
    }

    {
        ID3D12Device* d12Dev = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&d12Dev))) && d12Dev) {
            d12Dev->Release();
            DX12_OnSwapchainResizeBegin();

            // SELF-DESTRUCT: We are a DX11 hook on a DX12 swapchain.
            // Unhook ourselves to prevent infinite loops.
            HookLog(
                "DX11: DetourResizeBuffers - DX12 detected. Unhooking DX11 "
                "ResizeBuffers from this SwapChain.");

            void** vtable = *(void***)pSwapChain;
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(&vtable[13]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                void* replaced = InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(&vtable[13]), reinterpret_cast<void*>(dx11_hook_oResizeBuffers),
                    reinterpret_cast<void*>(DetourResizeBuffers));
                DWORD ignoredProtect = 0;
                VirtualProtect(reinterpret_cast<void*>(&vtable[13]), sizeof(void*), oldProtect, &ignoredProtect);
                if (replaced == reinterpret_cast<void*>(DetourResizeBuffers)) {
                    HookLog("DX11: DetourResizeBuffers - VTable[13] restored to original.");
                } else if (replaced != reinterpret_cast<void*>(dx11_hook_oResizeBuffers)) {
                    HookLogImportant(
                        "DX11: DetourResizeBuffers - preserving foreign VTable[13] follower %p during DX12 handoff",
                        replaced);
                }
            } else {
                HookLog("DX11: DetourResizeBuffers - FAILED to restore VTable[13]!");
            }

            // Call original immediately
            HRESULT hr = dx11_hook_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
            // CRITICAL FIX: Reset the resize cleanup flag since we called Begin but
            // never called End
            DX12_OnSwapchainResizeEnd();
            return hr;
        }
    }

    // Release render target view before resize
    if (dx11_hook_g_mainRenderTargetView) {
        dx11_hook_g_mainRenderTargetView->Release();
        dx11_hook_g_mainRenderTargetView = nullptr;
    }
    if (dx11_hook_g_mainRenderTargetView10) {
        dx11_hook_g_mainRenderTargetView10->Release();
        dx11_hook_g_mainRenderTargetView10 = nullptr;
    }

    // Invalidate D3D11 resources for OverlayAdapter if needed
    // Typically OverlayAdapter Release/resize handling is done in Render logic or
    // internally But we can force a shutdown if we want fresh resources on resize
    if (g_OverlayAdapter.IsInitialized()) {
        // g_OverlayAdapter.Shutdown(); // Optional: Shutdown on resize?
        // Usually not needed for DX11 as backend handles it or uses swapchain
        // backbuffer which changes? Capture project uses OMSetRenderTargets. For
        // safety, let's just let it be.
    }

    // Check for Waitable Swapchain
    if (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        HookLog("DX11: ResizeBuffers: Waitable Swapchain detected");
    }

    // Apply backbuffer count override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
        // Check swap effect — don't reduce buffer count for flip model swapchains
        bool isFlip = false;
        DXGI_SWAP_CHAIN_DESC scDesc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
            isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                      scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        }
        UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
        if (isFlip && (UINT)count < gameCount) {
            HookLog("DX11: ResizeBuffers: Skipping BufferCount override %d < game's %u (flip model)", count, gameCount);
        } else {
            BufferCount = (UINT)count;
            HookLog("DX11: ResizeBuffers: Overriding BufferCount to %d", count);
        }
    }

    // Call original ResizeBuffers
    HRESULT hr = dx11_hook_oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (FAILED(hr)) {
        HookLog("DX11: ResizeBuffers FAILED hr=0x%08X", hr);
    } else {
        dx11_hook_g_DX11Capture.RequestGenerationReset(pSwapChain);
        HookLog("DX11: ResizeBuffers SUCCESS");
    }

    return hr;
}

// --- Prerender Limit Support ---

namespace DXGIShared {
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    if (HookIsShuttingDown())
        return;
    // Deferred AF bootstrap: capture already-bound samplers/SRVs once so later
    // SRV changes can reconcile forced AF with resource context.
    ApplyDeferredSamplerOverrides11(pSwapChain);

    ::HandleDX11ProcessFrame(pSwapChain, isRealFrame);
}
}

namespace DXGIShared {
void HandleDX11ResizeBegin() {
    CleanupDX11Resources();
}
}

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                   ID3D11SamplerState** ppSamplerState) {
    if (!pSamplerDesc)
        return dx11_hook_oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperSamplerForwarding())
        return dx11_hook_oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return dx11_hook_oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

    bool debug = false;
    D3D11_SAMPLER_DESC desc = *pSamplerDesc;

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        debug = true;
    }

    const auto& gfx = GetActiveGraphicsConfig();
    // AF enablement needs SRV/resource context on Blackwell. Create-time still
    // handles AF-off, mip mapping, and mip-bias changes, but forced AF-on is
    // applied later by the bind-state reconciler.
    const bool allowAF = false;
    const bool modified = DX11Hook_ApplySamplerOverrides(desc, gfx, allowAF);

    {
        static std::atomic<int> s_createAFLog{0};
        int idx = s_createAFLog.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            if (modified) {
                HookLogImportant(
                    "DX11: CreateSamplerState override origFilter=0x%X newFilter=0x%X Aniso=%u Bias=%.2f Addr=%d/%d/%d "
                    "Comp=%d MinLOD=%.1f MaxLOD=%.1f (#%d)",
                    pSamplerDesc->Filter, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, pSamplerDesc->AddressU,
                    pSamplerDesc->AddressV, pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD,
                    pSamplerDesc->MaxLOD, idx + 1);
            } else if (ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
                HookLogImportant(
                    "DX11: CreateSamplerState deferred runtime AF Filter=0x%X Aniso=%u Addr=%d/%d/%d Comp=%d "
                    "MinLOD=%.1f MaxLOD=%.1f (#%d)",
                    pSamplerDesc->Filter, pSamplerDesc->MaxAnisotropy, pSamplerDesc->AddressU, pSamplerDesc->AddressV,
                    pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD, pSamplerDesc->MaxLOD,
                    idx + 1);
            } else if (idx < 8) {
                HookLogImportant(
                    "DX11: CreateSamplerState passthrough AF disabled Filter=0x%X Aniso=%u Addr=%d/%d/%d Comp=%d "
                    "MinLOD=%.1f MaxLOD=%.1f af=%s (#%d)",
                    pSamplerDesc->Filter, pSamplerDesc->MaxAnisotropy, pSamplerDesc->AddressU, pSamplerDesc->AddressV,
                    pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD, pSamplerDesc->MaxLOD,
                    gfx.anisotropicFiltering.c_str(), idx + 1);
            }
        }
    }

    HRESULT hr;
    if (modified) {
        hr = dx11_hook_oCreateSamplerState(pDevice, &desc, ppSamplerState);
        if (FAILED(hr)) {
            if (debug) {
                EarlyLog(
                    "DX11: CreateSamplerState FAILED with modified desc "
                    "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
                    hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
            }
        }
    } else {
        hr = dx11_hook_oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    }
    return hr;
}

bool DX10Hook_ApplySamplerOverrides(D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!ce::sampler_override::IsD3D10SamplerOverrideEligible(desc, gfx)) {
        return false;
    }

    bool modified = false;
    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
                desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
            }
        } else if (ce::sampler_override::D3D10SamplerAllowsCreationTimeForcedAF(desc, gfx)) {
            const D3D10_FILTER forcedFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
            const UINT forcedAnisotropy = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
            if (desc.Filter != forcedFilter || desc.MaxAnisotropy != forcedAnisotropy) {
                desc.Filter = forcedFilter;
                desc.MaxAnisotropy = forcedAnisotropy;
                modified = true;
            }
        }
    }

    if (!ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
        D3D10_FILTER forcedFilter = desc.Filter;
        if (gfx.mipMapping == "trilinear") {
            forcedFilter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        } else if (gfx.mipMapping == "bilinear") {
            forcedFilter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
        if (desc.Filter != forcedFilter) {
            desc.Filter = forcedFilter;
            modified = true;
        }
    }

    float userBiasValue = 0.0f;
    const bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasValue);
    const float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);

    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
            desc.MipLODBias += sgssaaBias;
        }
    }
    if (userBiasActive && userBiasValue < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp &&
        desc.MipLODBias < -0.5f) {
        desc.MipLODBias = -0.5f;
    }
    desc.MipLODBias = FinalizeMipBias(gfx, desc.MipLODBias);
    modified = modified || desc.MipLODBias != originalBias;
    return modified;
}

// D3D10 sampler descriptors are immutable. Apply the conservative policy once
// at creation so there is no sampler-bind or draw-time override work.
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice, const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                     ID3D10SamplerState** ppSamplerState) {
    if (!pSamplerDesc)
        return dx11_hook_oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperSamplerForwarding())
        return dx11_hook_oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);

    D3D10_SAMPLER_DESC desc = *pSamplerDesc;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const bool modified = DX10Hook_ApplySamplerOverrides(desc, gfx);

    HRESULT hr = dx11_hook_oCreateSamplerState10(pDevice, modified ? &desc : pSamplerDesc, ppSamplerState);
    if (modified && FAILED(hr)) {
        if (ppSamplerState && *ppSamplerState) {
            (*ppSamplerState)->Release();
            *ppSamplerState = nullptr;
        }
        static std::atomic<int> s_fallbackLogCount{0};
        const int logIndex = s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8) {
            HookLogImportant(
                "DX10: Modified sampler rejected; retrying original descriptor hr=0x%08X filter=0x%X aniso=%u "
                "bias=%.2f (#%d)",
                hr, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, logIndex + 1);
        }
        hr = dx11_hook_oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    } else if (modified) {
        static std::atomic<int> s_overrideLogCount{0};
        const int logIndex = s_overrideLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 32) {
            HookLogImportant(
                "DX10: Creation-time sampler override filter=0x%X->0x%X aniso=%u->%u bias=%.2f->%.2f "
                "policy=%s (#%d)",
                pSamplerDesc->Filter, desc.Filter, pSamplerDesc->MaxAnisotropy, desc.MaxAnisotropy,
                pSamplerDesc->MipLODBias, desc.MipLODBias, gfx.samplerOverrideMode.c_str(), logIndex + 1);
        }
    }
    return hr;
}

void DX11Hook::ProcessDeferredReleases() {
    g_DeferredRelease.Process();
}

// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    if (HookIsShuttingDown())
        return;
    HandleDX11ProcessFrame(pSwapChain, true);
}
