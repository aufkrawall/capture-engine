#include "dx11_hook_internal.h"
#include "common/logging/log_meter.h"
#include "hook/overrides/sampler_override/translation_scope.h"
#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/present/swapchain_flag_policy.h"

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

    const auto& gfx = GetActiveGraphicsConfigCached();
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
    if (s_LastRuntimeHookContext != ctx || !ContextVTableHooksActive11(ctx)) {
        const int beforeHooks = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        InstallVTableHooks(dev, ctx, pSwapChain);
        const int installedDelta = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed) - beforeHooks;
        static std::atomic<int> s_runtimeEnsureLogs{0};
        int idx = s_runtimeEnsureLogs.fetch_add(1, std::memory_order_relaxed);
        if (idx < 8) {
            HookLogImportant(
                "DX11: Runtime AF hook ensure from Present ctx=%p dispatch=%s installedDelta=%d hooks(psShader=%p "
                "psSRV=%p psSamp=%p createPS=%p createDeferred=%p draw=%p executeCL=%p)",
                (void*)ctx, ce::d3d11_dispatch::RuntimeTable(ctx) ? "ce-table" : "slot-hooks", installedDelta,
                (void*)dx11_hook_oPSSetShader11, (void*)dx11_hook_oPSSetShaderResources11,
                (void*)dx11_hook_oPSSetSamplers11, (void*)dx11_hook_oCreatePixelShader11, (void*)dx11_hook_oCreateDeferredContext11, (void*)dx11_hook_oDraw11,
                (void*)dx11_hook_oExecuteCommandList11);
        }
        s_LastRuntimeHookContext = ctx;
    }

    // Only pixel-stage bindings are tracked (forced AF replaces nothing else);
    // the other stages are counted straight from the context so a game that
    // binds samplers there first still completes the bootstrap as before.
    int totalBoundSamplers = RawAF_Bootstrap(ctx);
    const D3D11ShaderStage untrackedStages[] = {D3D11ShaderStage::Vertex, D3D11ShaderStage::Geometry,
                                                D3D11ShaderStage::Hull, D3D11ShaderStage::Domain,
                                                D3D11ShaderStage::Compute};
    for (const D3D11ShaderStage stage : untrackedStages) {
        ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        GetStageSamplers11(ctx, stage, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
        for (ID3D11SamplerState* sampler : samplers) {
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
    // The waitable-object bit CE may have added at creation has to agree with
    // the real chain: DXGI fails the resize with E_INVALIDARG on any
    // disagreement there, and the live descriptor is the only authority for it.
    {
        DXGI_SWAP_CHAIN_DESC resizeDesc = {};
        if (pSwapChain && SUCCEEDED(pSwapChain->GetDesc(&resizeDesc))) {
            SwapChainFlags =
                ce::swapchain_flag_policy::ReconcileApplicationResizeFlags(SwapChainFlags, resizeDesc.Flags);
        }
    }

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
    // CE's wrapper device applies the same policy before it forwards here.
    if (DX11Hook_IsWrapperSamplerForwarding()) {
        return dx11_hook_oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    }
    const auto create = [](void* device, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler) {
        return dx11_hook_oCreateSamplerState(static_cast<ID3D11Device*>(device), desc, sampler);
    };
    return ce::d3d11_af::CreateSamplerWithCreationPolicy(pSamplerDesc, ppSamplerState, create, pDevice, "raw");
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
