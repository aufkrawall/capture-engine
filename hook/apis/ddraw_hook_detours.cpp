#include "ddraw_hook_internal.h"


HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis,  DDSURFACEDESC* pDesc, 
                                                                     IDirectDrawSurface** ppSurface, 
                                                                     IUnknown* ddraw_hook_pUnkOuter) {


    LegacyDDrawVTableRecord record;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_LegacyDDrawVTables.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_LegacyDDrawVTables.end())
            record = it->second;
    }
    if (!record.createSurface)
        return DDERR_GENERIC;

    if (!HookIsShuttingDown() && pDesc && g_IPC) {
        const int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc) && (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX)) {
            pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
            pDesc->dwBackBufferCount = static_cast<DWORD>(count - 1);
        }
    }

    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0);
        }
    }
    return hr;

}


LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface) {


    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    const auto it = ddraw_hook_g_LegacySurfaceVTables.find(surface ? *(void***)surface : nullptr);
    return it != ddraw_hook_g_LegacySurfaceVTables.end() ? it->second : LegacySurfaceVTableRecord{};

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface, 
                                                           IDirectDrawSurface* destOverride,  DWORD ddraw_hook_flags) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.flip)
        return DDERR_GENERIC;
    const HRESULT hr = record.flip(surface, destOverride, ddraw_hook_flags);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface,  LPRECT destRect, 
                                                          IDirectDrawSurface* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                          DDBLTFX* ddraw_hook_bltFx) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface, srcSurface);
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface,  LPRECT destRect, 
                                                           DDSURFACEDESC* surfaceDesc,  DWORD ddraw_hook_flags,  HANDLE ddraw_hook_event) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    return record.lock ? record.lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event) : DDERR_GENERIC;

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface,  LPVOID ddraw_hook_surfaceData) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, ddraw_hook_surfaceData);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis,  DDSURFACEDESC2* pDesc, 
                                                                IDirectDrawSurface7** ppSurface,  IUnknown* ddraw_hook_pUnkOuter) {


    HookLog("DDraw: DetourDirectDraw7CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (!HookIsShuttingDown() && pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw7CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw7CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw7CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        InstallSurfaceHooksForSurface(*ppSurface, "CreateSurface");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface = *ppSurface;
            HookLog("DDraw: Tracking primary surface from CreateSurface (%p)", *ppSurface);
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                InstallAttachedBackBufferHooks(*ppSurface, "CreateSurface attached backbuffer");
            }
        }
    }

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis,  DDSURFACEDESC2* pDesc, 
                                                                IDirectDrawSurface4** ppSurface,  IUnknown* ddraw_hook_pUnkOuter) {


    HookLog("DDraw: DetourDirectDraw4CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (!HookIsShuttingDown() && pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface4: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw4CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw4CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw4CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        InstallSurfaceHooksForSurface4(*ppSurface, "CreateSurface4");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface4 = *ppSurface;
            HookLog("DDraw: Tracking primary surface4 from CreateSurface (%p)", *ppSurface);
        }
    }

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface,  IDirectDrawSurface7* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface7Flip ? ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags)
                                         : DDERR_GENERIC;
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
    MaybeTrackPrimarySurface(surface, "Flip");

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                ddraw_hook_flags |= 0x00000008;   // DDFLIP_NOVSYNC
                ddraw_hook_flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                ddraw_hook_flags |= 0x00000001;   // DDFLIP_WAIT
                ddraw_hook_flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface,  IDirectDrawSurface4* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface4Flip ? ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags)
                                         : DDERR_GENERIC;
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
    MaybeTrackPrimarySurface4(surface, "Flip4");

    HRESULT hr = ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags);
    HandleCaptureSurface4(surface);
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface,  LPRECT destRect, 
                                                     IDirectDrawSurface7* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                     void* ddraw_hook_bltFx) {


    HRESULT hr = ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (HookIsShuttingDown())
        return hr;
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (SUCCEEDED(hr) && srcSurface && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        RememberPresentedSourceSurface(srcSurface);
    }

    if (surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    // Only capture if this is a blit to the tracked primary surface
    if (surface && surface != ddraw_hook_g_HookSurfacePrototype && (!ddraw_hook_g_PrimarySurface || surface == ddraw_hook_g_PrimarySurface)) {
        HandleCapture(surface, srcSurface);
    }

    return hr;

}


#include "ddraw_hook_internal.h"


HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface,  LPRECT destRect, 
                                                     IDirectDrawSurface4* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                     void* ddraw_hook_bltFx) {


    HRESULT hr = ddraw_hook_oDDSurface4Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (HookIsShuttingDown())
        return hr;
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != ddraw_hook_g_HookSurfacePrototype4 && !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Blt4");
    }

    if (SUCCEEDED(hr) && surface && surface != ddraw_hook_g_HookSurfacePrototype4 &&
        (!ddraw_hook_g_PrimarySurface4 || surface == ddraw_hook_g_PrimarySurface4)) {
        HandleCaptureSurface4(surface, srcSurface);
    }

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface,  LPRECT destRect,  void* surfaceDesc, 
                                                      DWORD ddraw_hook_flags,  HANDLE ddraw_hook_event) {


    HRESULT hr = ddraw_hook_oDDSurface7Lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype &&
        !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Lock");
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface,  LPRECT destRect,  void* surfaceDesc, 
                                                      DWORD ddraw_hook_flags,  HANDLE ddraw_hook_event) {


    HRESULT hr = ddraw_hook_oDDSurface4Lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype4 &&
        !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Lock4");
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface,  LPRECT ddraw_hook_rect) {


    HRESULT hr = ddraw_hook_oDDSurface7Unlock(surface, ddraw_hook_rect);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        HandleCapture(surface);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface,  LPRECT ddraw_hook_rect) {


    HRESULT hr = ddraw_hook_oDDSurface4Unlock(surface, ddraw_hook_rect);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface4) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        HandleCaptureSurface4(surface);
    }
    return hr;

}

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
            ddraw_hook_g_D3D7Device = *ddraw_hook_device;
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


    if (HookIsShuttingDown())
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
    if (HookIsShuttingDown())
        return setState ? setState(ddraw_hook_device, Stage, Type, ddraw_hook_Value) : DDERR_GENERIC;
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetTextureStageState");
    ddraw_hook_g_D3D7Device = ddraw_hook_device;  // Capture device for proactive use
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
    if (HookIsShuttingDown())
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
    if (HookIsShuttingDown())
        return endScene(ddraw_hook_device);
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    return endScene(ddraw_hook_device);

}

HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device,  DWORD ddraw_hook_blockHandle) {


    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, ddraw_hook_device);
    auto applyStateBlock = record ? record->applyStateBlock.load(std::memory_order_acquire) : nullptr;
    if (!applyStateBlock)
        return DDERR_GENERIC;
    const HRESULT hr = applyStateBlock(ddraw_hook_device, ddraw_hook_blockHandle);
    if (!HookIsShuttingDown() && SUCCEEDED(hr)) {
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
