#include "dx11_hook_internal.h"


HRESULT STDMETHODCALLTYPE DetourCreatePixelShader11(ID3D11Device* device,  const void* shaderBytecode, 
                                                           SIZE_T bytecodeLength,  ID3D11ClassLinkage* classLinkage, 
                                                           ID3D11PixelShader** pixelShader) {


    const HRESULT hr = dx11_hook_oCreatePixelShader11(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && pixelShader && *pixelShader) {
        RegisterWrapperPixelShaderAFMetadata(*pixelShader, shaderBytecode, bytecodeLength);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourCreateDeferredContext11(ID3D11Device* device,  UINT contextFlags, 
                                                               ID3D11DeviceContext** deferredContext) {


    if (!dx11_hook_oCreateDeferredContext11) {
        return E_FAIL;
    }
    const HRESULT hr = dx11_hook_oCreateDeferredContext11(device, contextFlags, deferredContext);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && deferredContext && *deferredContext) {
        InstallContextVTableHooks11(*deferredContext, "CreateDeferredContext");
        int idx = dx11_hook_g_DiagCreateDeferredContext11.fetch_add(1, std::memory_order_relaxed);
        if (idx < 16) {
            void** vtable = *reinterpret_cast<void***>(*deferredContext);
            HookLogImportant("DX11: CreateDeferredContext returned ctx=%p flags=0x%X vtable=%p (#%d)",
                             (void*)*deferredContext, contextFlags, (void*)vtable, idx + 1);
        }
    }
    return hr;

}

void STDMETHODCALLTYPE DetourPSSetShader11(ID3D11DeviceContext* context,  ID3D11PixelShader* pixelShader, 
                                                  ID3D11ClassInstance* const* classInstances,  UINT numClassInstances) {


    PSSetShader11_t original =
        ResolveContextOriginal11(context, 9, &D3D11ContextVTableOriginals::psSetShader, dx11_hook_oPSSetShader11);
    if (!original) {
        return;
    }
    original(context, pixelShader, classInstances, numClassInstances);
    if (HookIsShuttingDown() || dx11_hook_g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateTrackedPixelShader11(context, pixelShader);

}

void STDMETHODCALLTYPE DetourPSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 8, &D3D11ContextVTableOriginals::psSetShaderResources, dx11_hook_oPSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Pixel, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourVSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 25, &D3D11ContextVTableOriginals::vsSetShaderResources, dx11_hook_oVSSetShaderResources11);
    if (!original) {
        return;
    }

    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Vertex, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourGSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 31, &D3D11ContextVTableOriginals::gsSetShaderResources, dx11_hook_oGSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Geometry, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourHSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 59, &D3D11ContextVTableOriginals::hsSetShaderResources, dx11_hook_oHSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Hull, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourDSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 63, &D3D11ContextVTableOriginals::dsSetShaderResources, dx11_hook_oDSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Domain, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourCSSetShaderResources11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numViews, 
                                                           ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    SetShaderResources11_t original = ResolveContextOriginal11(
        context, 67, &D3D11ContextVTableOriginals::csSetShaderResources, dx11_hook_oCSSetShaderResources11);
    if (!original) {
        return;
    }
    original(context, startSlot, numViews, ppShaderResourceViews);
    if (HookIsShuttingDown() || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    UpdateStageShaderResources(context, D3D11ShaderStage::Compute, startSlot, numViews, ppShaderResourceViews);

}

void STDMETHODCALLTYPE DetourPSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, dx11_hook_oPSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Pixel, startSlot, numSamplers, ppSamplers);

}

void STDMETHODCALLTYPE DetourVSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 26, &D3D11ContextVTableOriginals::vsSetSamplers, dx11_hook_oVSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Vertex, startSlot, numSamplers, ppSamplers);

}

void STDMETHODCALLTYPE DetourGSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 32, &D3D11ContextVTableOriginals::gsSetSamplers, dx11_hook_oGSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Geometry, startSlot, numSamplers, ppSamplers);

}

void STDMETHODCALLTYPE DetourHSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 61, &D3D11ContextVTableOriginals::hsSetSamplers, dx11_hook_oHSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Hull, startSlot, numSamplers, ppSamplers);

}

void STDMETHODCALLTYPE DetourDSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 65, &D3D11ContextVTableOriginals::dsSetSamplers, dx11_hook_oDSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Domain, startSlot, numSamplers, ppSamplers);

}

void STDMETHODCALLTYPE DetourCSSetSamplers11(ID3D11DeviceContext* context,  UINT startSlot,  UINT numSamplers, 
                                                    ID3D11SamplerState* const* ppSamplers) {


    SetSamplers11_t original =
        ResolveContextOriginal11(context, 70, &D3D11ContextVTableOriginals::csSetSamplers, dx11_hook_oCSSetSamplers11);
    SetSamplersWithOverrides11(original, context, D3D11ShaderStage::Compute, startSlot, numSamplers, ppSamplers);

}

void ReconcilePixelSamplersBeforeDraw11(ID3D11DeviceContext* context) {


    if (HookIsShuttingDown() || !context || dx11_hook_g_InOverlayRender || DX11Hook_IsWrapperContextForwarding()) {
        return;
    }
    if (dx11_hook_g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    SetSamplers11_t original =
        ResolveContextOriginal11(context, 10, &D3D11ContextVTableOriginals::psSetSamplers, dx11_hook_oPSSetSamplers11);
    if (!original) {
        return;
    }
    const uint32_t dirtyMask = ConsumePixelSamplerDirtyMask11(context);
    if (dirtyMask == 0) {
        return;
    }
    const int rebound = ReconcileStageSamplers11(original, context, D3D11ShaderStage::Pixel, 0,
                                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
    int idx = dx11_hook_g_DiagSamplerDrawReconcileCalls.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLog("DX11: AF draw reconcile ctx=%p dirtyMask=0x%04X rebound=%d (#%d)", (void*)context, dirtyMask, rebound,
                idx + 1);
    }

}

void STDMETHODCALLTYPE DetourDrawIndexed11(ID3D11DeviceContext* context,  UINT indexCount, 
                                                  UINT startIndexLocation,  INT baseVertexLocation) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexed11_t original =
        ResolveContextOriginal11(context, 12, &D3D11ContextVTableOriginals::drawIndexed, dx11_hook_oDrawIndexed11);
    if (original) {
        original(context, indexCount, startIndexLocation, baseVertexLocation);
    }

}

void STDMETHODCALLTYPE DetourDraw11(ID3D11DeviceContext* context,  UINT vertexCount,  UINT startVertexLocation) {


    ReconcilePixelSamplersBeforeDraw11(context);
    Draw11_t original = ResolveContextOriginal11(context, 13, &D3D11ContextVTableOriginals::draw, dx11_hook_oDraw11);
    if (original) {
        original(context, vertexCount, startVertexLocation);
    }

}

void STDMETHODCALLTYPE DetourDrawIndexedInstanced11(ID3D11DeviceContext* context,  UINT indexCountPerInstance, 
                                                           UINT instanceCount,  UINT startIndexLocation, 
                                                           INT baseVertexLocation,  UINT startInstanceLocation) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstanced11_t original = ResolveContextOriginal11(
        context, 20, &D3D11ContextVTableOriginals::drawIndexedInstanced, dx11_hook_oDrawIndexedInstanced11);
    if (original) {
        original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation,
                 startInstanceLocation);
    }

}

void STDMETHODCALLTYPE DetourDrawInstanced11(ID3D11DeviceContext* context,  UINT vertexCountPerInstance, 
                                                    UINT instanceCount,  UINT startVertexLocation, 
                                                    UINT startInstanceLocation) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstanced11_t original =
        ResolveContextOriginal11(context, 21, &D3D11ContextVTableOriginals::drawInstanced, dx11_hook_oDrawInstanced11);
    if (original) {
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }

}

void STDMETHODCALLTYPE DetourDrawAuto11(ID3D11DeviceContext* context) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawAuto11_t original = ResolveContextOriginal11(context, 38, &D3D11ContextVTableOriginals::drawAuto, dx11_hook_oDrawAuto11);
    if (original) {
        original(context);
    }

}

void STDMETHODCALLTYPE DetourDrawIndexedInstancedIndirect11(ID3D11DeviceContext* context, 
                                                                   ID3D11Buffer* bufferForArgs, 
                                                                   UINT alignedByteOffsetForArgs) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawIndexedInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 39, &D3D11ContextVTableOriginals::drawIndexedInstancedIndirect, dx11_hook_oDrawIndexedInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }

}

void STDMETHODCALLTYPE DetourDrawInstancedIndirect11(ID3D11DeviceContext* context,  ID3D11Buffer* bufferForArgs, 
                                                            UINT alignedByteOffsetForArgs) {


    ReconcilePixelSamplersBeforeDraw11(context);
    DrawInstancedIndirect11_t original = ResolveContextOriginal11(
        context, 40, &D3D11ContextVTableOriginals::drawInstancedIndirect, dx11_hook_oDrawInstancedIndirect11);
    if (original) {
        original(context, bufferForArgs, alignedByteOffsetForArgs);
    }

}

void STDMETHODCALLTYPE DetourExecuteCommandList11(ID3D11DeviceContext* context,  ID3D11CommandList* commandList, 
                                                         BOOL restoreContextState) {


    int idx = dx11_hook_g_DiagExecuteCommandList11.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        HookLogImportant(
            "DX11: ExecuteCommandList ctx=%p commandList=%p restore=%d deferredContexts=%d "
            "drawReconcile=%d (#%d)",
            (void*)context, (void*)commandList, restoreContextState ? 1 : 0,
            dx11_hook_g_DiagCreateDeferredContext11.load(std::memory_order_relaxed),
            dx11_hook_g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed), idx + 1);
    }

    ExecuteCommandList11_t original =
        ResolveContextOriginal11(context, 58, &D3D11ContextVTableOriginals::executeCommandList, dx11_hook_oExecuteCommandList11);
    if (original) {
        original(context, commandList, restoreContextState);
    }
    if (!HookIsShuttingDown() && !restoreContextState) {
        ClearTrackedContextState11(context);
    }

}

VSyncOverride GetDX11VSyncOverride() {


    return GetVSyncOverride();  // Use the shared helper from hook_common.h

}

bool ShouldSkipWindowForNvPresent(HWND hwnd) {


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

HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter,  D3D_DRIVER_TYPE DriverType, 
                                                          HMODULE Software,  UINT Flags, 
                                                          const D3D_FEATURE_LEVEL* pFeatureLevels,  UINT FeatureLevels, 
                                                          UINT SDKVersion,  const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, 
                                                          IDXGISwapChain** ppSwapChain,  ID3D11Device** ppDevice, 
                                                          D3D_FEATURE_LEVEL* pFeatureLevel, 
                                                          ID3D11DeviceContext** ppImmediateContext) {


    PFN_D3D11CreateDeviceAndSwapChain realOriginal =
        dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain ? dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain
                                                       : oD3D11CreateDeviceAndSwapChain;
    if (!realOriginal)
        return E_FAIL;
    if (HookIsShuttingDown()) {
        return realOriginal(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                            pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    }
    // Unconditional log to verify hook is being called
    HookLog("DetourD3D11CreateDeviceAndSwapChain: ENTER");

    if (pSwapChainDesc) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called. Width=%u Height=%u", pSwapChainDesc->BufferDesc.Width,
                     pSwapChainDesc->BufferDesc.Height);
        }
    } else {
        EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called (pSwapChainDesc=NULL)");
    }

    DXGI_SWAP_CHAIN_DESC desc;
    const DXGI_SWAP_CHAIN_DESC* pFinalDesc = pSwapChainDesc;

    if (pSwapChainDesc) {
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        desc = *pSwapChainDesc;
        bool modified = false;

        // Backbuffer Count
        modified = ApplyDX11BackbufferCountOverride(desc, "CreateDeviceAndSwapChain") || modified;

        // MSAA Override
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0) {
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                modified = true;
                HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA OFF");
            } else {
                UINT samples = 1;
                if (strcmp(msaa, "2x") == 0)
                    samples = 2;
                else if (strcmp(msaa, "4x") == 0)
                    samples = 4;
                else if (strcmp(msaa, "8x") == 0)
                    samples = 8;

                if (samples > 1) {
                    desc.SampleDesc.Count = samples;
                    desc.SampleDesc.Quality = 0;
                    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // MSAA requires DISCARD in D3D11
                    modified = true;
                    HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA %dx", samples);
                }
            }
        }

        if (modified)
            pFinalDesc = &desc;
    }

    // Use the local copy of the real original.  The shared oD3D11CreateDeviceAndSwapChain
    // (from dx11_hook.h) may have been overwritten by HookExport -> PatchIATAllModules,
    // which sets it to Wrapped_D3D11CreateDeviceAndSwapChain when a prior IAT hook exists.
    // Calling that would re-enter the wrapper -> infinite recursion -> stack overflow.
    HRESULT hr = realOriginal(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion,
                              pFinalDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        DX11Hook_RegisterDeviceIdentity(*ppDevice, "D3D11CreateDeviceAndSwapChain", true);
        const GraphicsConfig& gfx = GetActiveGraphicsConfig();
        if (ppSwapChain && *ppSwapChain && HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC actualDesc = {};
            if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
                HookLogImportant(
                    "DX11: CreateDeviceAndSwapChain actual BufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }

        // Wrap the swapchain returned by D3D11CreateDeviceAndSwapChain so our
        // wrapper captures Present
        if (ppSwapChain && *ppSwapChain) {
            IUnknown* pDev = (ppDevice && *ppDevice) ? *ppDevice : nullptr;
            IDXGISwapChain* pReal = *ppSwapChain;
            *ppSwapChain = (IDXGISwapChain*)new CWrapDXGISwapChain(pReal, pDev);
            pReal->Release();
            HookLogImportant("DX11: Wrapped swapchain from D3D11CreateDeviceAndSwapChain");
        }

        IDXGISwapChain* sc = (ppSwapChain && *ppSwapChain) ? *ppSwapChain : nullptr;
        ID3D11DeviceContext* ctx = (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr;
        // If immediate context not provided, get it from device
        if (!ctx)
            (*ppDevice)->GetImmediateContext(&ctx);  // AddRef'd

        InstallVTableHooks(*ppDevice, ctx, sc);

        // Note: Factory vtable hooks removed - wrappers handle swapchain creation

        if (!ppImmediateContext && ctx)
            ctx->Release();

        // Explicitly set VRAM Total to prevent background thread crash
        if (ppDevice && *ppDevice) {
            IDXGIDevice* dxgiDevice = nullptr;
            if (SUCCEEDED((*ppDevice)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc;
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                    }
                    adapter->Release();
                }
                dxgiDevice->Release();
            }
        }
    }

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pFactory,  IUnknown* pDevice, 
                                                       DXGI_SWAP_CHAIN_DESC* pDesc,  IDXGISwapChain** ppSwapChain) {


    if (HookIsShuttingDown()) {
        return dx11_hook_oCreateSwapChain ? dx11_hook_oCreateSwapChain(pFactory, DeWrap(pDevice), pDesc, ppSwapChain)
                                          : DXGI_ERROR_INVALID_CALL;
    }
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DX11 CreateSwapChain")) {
        return dx11_hook_oCreateSwapChain
                   ? dx11_hook_oCreateSwapChain(pFactory, DeWrap(pDevice), pDesc, ppSwapChain)
                   : DXGI_ERROR_INVALID_CALL;
    }
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChain called. Width=%u Height=%u Windowed=%d "
                "BufferCount=%u SwapEffect=%d",
                pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->Windowed, pDesc->BufferCount,
                pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC modifiedDesc = {};
    DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChain");
        pDescToUse = &modifiedDesc;
    }

    HRESULT hr = dx11_hook_oCreateSwapChain(pFactory, DeWrap(pDevice), pDescToUse, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChain created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // FSR4/FG swapchain recreation detection (shared with
        // CreateSwapChainForHwnd)
        bool isGameSizedSwapchain =
            pDescToUse && pDescToUse->BufferDesc.Width >= 1920 && pDescToUse->BufferDesc.Height >= 1080;
        if (isGameSizedSwapchain) {
            if (dx11_hook_g_FirstGameSwapchainCreated) {
                // Recreation - likely FG taking over
                HookLog(
                    "DX11: CreateSwapChain: Game-sized swapchain recreated - "
                    "invalidating DX12 overlay");
                DX12_SignalFSR4SwapchainRecreated();
            } else {
                dx11_hook_g_FirstGameSwapchainCreated = true;
                HookLog("DX11: CreateSwapChain: First game-sized swapchain created (%ux%u)",
                        pDescToUse->BufferDesc.Width, pDescToUse->BufferDesc.Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog("DX11: CreateSwapChain - Detected DX12 Device from SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pFactory,  IUnknown* pDevice,  HWND hWnd, 
                                                              const DXGI_SWAP_CHAIN_DESC1* pDesc, 
                                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, 
                                                              IDXGIOutput* pRestrictToOutput, 
                                                              IDXGISwapChain1** ppSwapChain) {


    if (HookIsShuttingDown()) {
        return dx11_hook_oCreateSwapChainForHwnd
                   ? dx11_hook_oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc,
                                                       pRestrictToOutput, ppSwapChain)
                   : DXGI_ERROR_INVALID_CALL;
    }
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DX11 CreateSwapChainForHwnd")) {
        return dx11_hook_oCreateSwapChainForHwnd
                   ? dx11_hook_oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc,
                                                       pRestrictToOutput, ppSwapChain)
                   : DXGI_ERROR_INVALID_CALL;
    }
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        if (pDesc) {
            EarlyLog(
                "DX11: CreateSwapChainForHwnd called. Width=%u Height=%u "
                "BufferCount=%u SwapEffect=%d",
                pDesc->Width, pDesc->Height, pDesc->BufferCount, pDesc->SwapEffect);
        }
    }

    DXGI_SWAP_CHAIN_DESC1 modifiedDesc = {};
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        ApplyDX11BackbufferCountOverride(modifiedDesc, "CreateSwapChainForHwnd");
        pDescToUse = &modifiedDesc;
    }

    // CRITICAL FG FIX: Track game-sized swapchain recreation for FG overlay
    // safety We DON'T invalidate BEFORE creation - that can cause DXGI lock
    // issues (E_ACCESSDENIED) Instead, we invalidate AFTER successful recreation
    // to clean up stale overlay resources
    bool isGameSizedSwapchain = pDescToUse && pDescToUse->Width >= 1920 && pDescToUse->Height >= 1080;
    bool wasRecreation = isGameSizedSwapchain && dx11_hook_g_FirstGameSwapchainCreated;

    HookLog("DX11: BEFORE oCreateSwapChainForHwnd call");
    HRESULT hr = dx11_hook_oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDescToUse, pFullscreenDesc,
                                         pRestrictToOutput, ppSwapChain);
    HookLog("DX11: AFTER oCreateSwapChainForHwnd call (hr=0x%08X)", hr);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        DXGI_SWAP_CHAIN_DESC actualDesc = {};
        if (SUCCEEDED((*ppSwapChain)->GetDesc(&actualDesc))) {
            const GraphicsConfig& gfx = GetActiveGraphicsConfig();
            if (HasBackbufferCountOverride(gfx.backbufferCount)) {
                HookLogImportant(
                    "DX11: CreateSwapChainForHwnd created sc=%p actualBufferCount=%u requested=%d "
                    "size=%ux%u swapEffect=%d",
                    *ppSwapChain, actualDesc.BufferCount, gfx.backbufferCount, actualDesc.BufferDesc.Width,
                    actualDesc.BufferDesc.Height, actualDesc.SwapEffect);
            }
        }
        // Post-creation: Signal invalidation ONLY for successful recreation
        if (wasRecreation) {
            HookLog(
                "DX11: CreateSwapChainForHwnd: Game-sized swapchain RECREATED "
                "successfully - invalidating overlay");
            DX12_InvalidateSwapchain();
            DX12_SignalFSR4SwapchainRecreated();
        }
        // Post-creation tracking
        if (isGameSizedSwapchain) {
            if (!dx11_hook_g_FirstGameSwapchainCreated) {
                dx11_hook_g_FirstGameSwapchainCreated = true;
                HookLog(
                    "DX11: CreateSwapChainForHwnd: First game-sized swapchain "
                    "created (%ux%u)",
                    pDescToUse->Width, pDescToUse->Height);
            }
        }

        // First try D3D12 - DX12 games create swapchains via DXGI too
        ID3D12CommandQueue* pD3D12Queue = nullptr;
        ID3D12Device* pD3D12Device = nullptr;

        // Check for CommandQueue (standard DX12)
        if (pDevice && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D12CommandQueue), (void**)&pD3D12Queue))) {
            HookLog("DX11: CreateSwapChainForHwnd - Detected DX12 CommandQueue.");
            DX12_SignalFSR4SwapchainRecreated();
            // Hook the command queue vtable so ExecuteCommandLists is intercepted
            // This is needed to capture the command queue for overlay rendering
            DX12_HookQueueVTable(pD3D12Queue);
            HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
            pD3D12Queue->Release();
        }
        // Check for D3D12 Device (non-standard but possible, or checking on
        // swapchain itself)
        else if (ppSwapChain && *ppSwapChain &&
                 SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D12Device), (void**)&pD3D12Device))) {
            HookLog(
                "DX11: CreateSwapChainForHwnd - Detected DX12 Device from "
                "SwapChain.");
            DX12_SignalFSR4SwapchainRecreated();
            HookLog(
                "DX11: Skipping DX12 hook installation from DX11 path (Device "
                "detection)");
            pD3D12Device->Release();
        } else {
            // DX11 path - original logic
            ID3D11Device* pD3D11Device = nullptr;
            if (SUCCEEDED((*ppSwapChain)->GetDevice(__uuidof(ID3D11Device), (void**)&pD3D11Device))) {
                ID3D11DeviceContext* ctx = nullptr;
                pD3D11Device->GetImmediateContext(&ctx);
                InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
                if (ctx)
                    ctx->Release();
                pD3D11Device->Release();
            } else {
                // Fallback for D3D10/10.1 or other versions
                // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
                // checks above This prevents infinite ResizeBuffers loops in games like
                // Strange Brigade DX12
                InstallVTableHooks(NULL, NULL, *ppSwapChain);
            }
        }
    }
    return hr;

}
