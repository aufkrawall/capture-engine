#include "ddraw_hook_internal.h"

// Legacy Direct3D (DX6/DX7) device interception: immediate-mode device
// creation, the sampler/render-state reconciliation those devices need, and
// the scene-end hooks that keep the forced-filtering policy applied.


void ReportLegacyD3DUse(unsigned version,  const char* evidence) {


    if (HookIsShuttingDown() || ddraw_hook_g_DDrawBootstrapDepth != 0)
        return;
    const unsigned previous = ddraw_hook_g_LegacyD3DCallbackVersion.exchange(version, std::memory_order_acq_rel);
    ddraw_hook_g_ActiveLegacyD3DVersion.store(version, std::memory_order_release);
    if (previous != version) {
        HookLogImportant("[GraphicsAPI] legacy Direct3D use accepted api=DX%u evidence=%s", version,
                         evidence ? evidence : "unknown");
    }

}

HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d,  REFCLSID deviceClass, 
                                                        IDirectDrawSurface7* target,  IDirect3DDevice7** ddraw_hook_device) {


    D3D7CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_D3D7CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != ddraw_hook_g_D3D7CreateDeviceOriginals.end())
            original = it->second;
    }

    const HRESULT hr = original ? original(d3d, deviceClass, target, ddraw_hook_device) : DDERR_GENERIC;
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_device && *ddraw_hook_device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, *ddraw_hook_device,
                                    ddraw_hook_g_DDrawBootstrapDepth == 0, "IDirect3D7::CreateDevice");
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            TrackLegacyD3D7Device(*ddraw_hook_device);
            AssociateLegacyD3DSurface(target, 7);
            ReportLegacyD3DUse(7, "IDirect3D7::CreateDevice");
        }
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d,  REFCLSID deviceClass, 
                                                        IDirectDrawSurface4* target,  IUnknown** ddraw_hook_device, 
                                                        IUnknown* ddraw_hook_outer) {


    D3D3CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_D3D3CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != ddraw_hook_g_D3D3CreateDeviceOriginals.end())
            original = it->second;
    }
    const HRESULT hr = original ? original(d3d, deviceClass, target, ddraw_hook_device, ddraw_hook_outer) : DDERR_GENERIC;
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_device && *ddraw_hook_device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, *ddraw_hook_device,
                                    ddraw_hook_g_DDrawBootstrapDepth == 0, "IDirect3D3::CreateDevice");
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            AssociateLegacyD3DSurface(target, 6);
            ReportLegacyD3DUse(6, "IDirect3D3::CreateDevice");
        }
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device,  DWORD Type,  DWORD ddraw_hook_Value) {


    if (HookIsShuttingDown() || LegacyD3DInternalCallActive())
        return ddraw_hook_oSetRenderState7(ddraw_hook_device, Type, ddraw_hook_Value);
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetRenderState");
    if (g_IPC) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (Type == 2 /* D3DRENDERSTATE_ANTIALIAS */) {
                if (strcmp(msaa, "off") == 0)
                    ddraw_hook_Value = 0;  // D3DANTIALIAS_NONE
                else
                    ddraw_hook_Value = 2;  // D3DANTIALIAS_SORTINDEPENDENT
            }
        }
    }
    return ddraw_hook_oSetRenderState7(ddraw_hook_device, Type, ddraw_hook_Value);

}

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device,  DWORD Stage,  DWORD Type, 
                                                             DWORD ddraw_hook_Value) {


    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState7);
    if (HookIsShuttingDown() || LegacyD3DInternalCallActive())
        return setState ? setState(ddraw_hook_device, Stage, Type, ddraw_hook_Value) : DDERR_GENERIC;
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetTextureStageState");
    TrackLegacyD3D7Device(ddraw_hook_device);
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, Stage, Type, ddraw_hook_Value, setState, getState, QueryD3D7MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device,  DWORD Stage,  DWORD Type, 
                                                             DWORD* ddraw_hook_pValue) {


    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState7);
    if (HookIsShuttingDown() || LegacyD3DInternalCallActive())
        return getState ? getState(ddraw_hook_device, Stage, Type, ddraw_hook_pValue) : DDERR_GENERIC;
    ReportLegacyD3DUse(7, "IDirect3DDevice7::GetTextureStageState");
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, Stage, Type, ddraw_hook_pValue, getState, setState,
        QueryD3D7MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device,  DWORD Stage,  DWORD Type,  DWORD ddraw_hook_Value) {


    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState6);
    if (HookIsShuttingDown())
        return setState ? setState(ddraw_hook_device, Stage, Type, ddraw_hook_Value) : DDERR_GENERIC;
    ReportLegacyD3DUse(6, "IDirect3DDevice3::SetTextureStageState");
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, Stage, Type, ddraw_hook_Value, setState, getState, QueryD3D6MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device,  DWORD Stage,  DWORD Type,  DWORD* ddraw_hook_pValue) {


    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(ddraw_hook_oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(ddraw_hook_oGetTextureStageState6);
    if (HookIsShuttingDown())
        return getState ? getState(ddraw_hook_device, Stage, Type, ddraw_hook_pValue) : DDERR_GENERIC;
    ReportLegacyD3DUse(6, "IDirect3DDevice3::GetTextureStageState");
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, Stage, Type, ddraw_hook_pValue, getState, setState,
        QueryD3D6MaxAnisotropy);

}

HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device) {


    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    if (HookIsShuttingDown() || LegacyD3DInternalCallActive())
        return endScene(ddraw_hook_device);
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    // The application is still inside its own scene here. Drawing before the
    // one real EndScene avoids the synthetic scene boundary that confused the
    // co-resident Steam overlay, while keeping every overlay pixel on the GPU.
    DrawNativeLegacyD3DOverlayAtEndScene(ddraw_hook_device);
    return endScene(ddraw_hook_device);

}

HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device,  DWORD ddraw_hook_blockHandle) {


    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto applyStateBlock = record ? record->applyStateBlock.load(std::memory_order_acquire) : nullptr;
    if (!applyStateBlock)
        return DDERR_GENERIC;
    const HRESULT hr = applyStateBlock(ddraw_hook_device, ddraw_hook_blockHandle);
    if (!HookIsShuttingDown() && !LegacyD3DInternalCallActive() && SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
            record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* ddraw_hook_device) {


    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    if (HookIsShuttingDown())
        return endScene(ddraw_hook_device);
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D6, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D6MaxAnisotropy);
    return endScene(ddraw_hook_device);

}

HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid,  IDirectDraw** lplpDD,  IUnknown* ddraw_hook_pUnkOuter) {


    const HRESULT hr = ddraw_hook_oDirectDrawCreate ? ddraw_hook_oDirectDrawCreate(lpGuid, lplpDD, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;

}

HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid,  LPVOID* lplpDD,  REFIID iid,  IUnknown* ddraw_hook_pUnkOuter) {


    HookLog("DDraw: DetourDirectDrawCreateEx called (iidIsDDraw7=%d, iidIsDDraw4=%d, out=%p)",
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0, lplpDD);
    HRESULT hr = ddraw_hook_oDirectDrawCreateEx ? ddraw_hook_oDirectDrawCreateEx(lpGuid, lplpDD, iid, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDrawCreateEx returned hr=0x%08x, object=%p", hr,
            (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && lplpDD && *lplpDD) {
        HookDirectDrawObject(*lplpDD, iid);
    }
    return hr;

}
