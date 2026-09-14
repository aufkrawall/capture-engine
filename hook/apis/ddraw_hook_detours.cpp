#include "ddraw_hook_internal.h"

namespace {

namespace policy = ce::ddraw_present_policy;

// One presentation classification for a blit-shaped call.
struct BlitPresentation {
    policy::PresentKind kind = policy::PresentKind::None;
    policy::Rect changedRect;
    bool haveChangedRect = false;
};

policy::Rect ToPolicyRect(const RECT& rect) {
    return policy::Rect{static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right),
                        static_cast<int>(rect.bottom)};
}

bool ReadSurfaceGeometry(IDirectDrawSurface7* surface, policy::Extent& extent, DWORD& caps) {
    return ResolveSurfaceGeometry(surface, extent, caps);
}

bool ReadSurfaceGeometry(IDirectDrawSurface4* surface, policy::Extent& extent, DWORD& caps) {
    extent = {};
    caps = 0;
    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    if (!surface || FAILED(surface->GetSurfaceDesc(&desc)))
        return false;
    extent.width = desc.dwWidth;
    extent.height = desc.dwHeight;
    caps = desc.ddsCaps.dwCaps;
    return extent.width > 0 && extent.height > 0;
}

bool ReadSurfaceGeometry(IDirectDrawSurface* surface, policy::Extent& extent, DWORD& caps) {
    extent = {};
    caps = 0;
    DDSURFACEDESC desc = {};
    desc.dwSize = sizeof(desc);
    if (!surface || FAILED(surface->GetSurfaceDesc(&desc)))
        return false;
    extent.width = desc.dwWidth;
    extent.height = desc.dwHeight;
    caps = desc.ddsCaps.dwCaps;
    return extent.width > 0 && extent.height > 0;
}

// A blit onto a flip chain's images is ordinary drawing - Flip publishes them -
// so only a single-buffered scanout surface can be presented to this way.
template <typename SurfaceT>
BlitPresentation ClassifyBlitCall(SurfaceT* dest, const RECT* destRect, SurfaceT* source, const RECT* sourceRect) {
    BlitPresentation result;
    policy::BlitGeometry geometry;
    DWORD destCaps = 0;
    if (!ReadSurfaceGeometry(dest, geometry.dest, destCaps))
        return result;

    geometry.destIsScanout = (destCaps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) != 0;
    geometry.destIsFlipChain = (destCaps & (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER)) != 0;
    if (!geometry.destIsScanout || geometry.destIsFlipChain) {
        // Flip owns a flip chain's images and an offscreen destination is not a
        // presentation at all, so the source never has to be described.
        return result;
    }
    if (destRect) {
        geometry.haveDestRect = true;
        geometry.destRect = ToPolicyRect(*destRect);
    }

    DWORD sourceCaps = 0;
    if (source && ReadSurfaceGeometry(source, geometry.source, sourceCaps)) {
        geometry.haveSource = true;
        if (sourceRect) {
            geometry.haveSourceRect = true;
            geometry.sourceRect = ToPolicyRect(*sourceRect);
        }
    }

    result.kind = policy::ClassifyBlit(geometry);
    result.haveChangedRect = geometry.haveDestRect;
    result.changedRect = geometry.haveDestRect
                             ? geometry.destRect
                             : policy::Rect{0, 0, static_cast<int>(geometry.dest.width),
                                            static_cast<int>(geometry.dest.height)};
    return result;
}

// BltFast has no destination rectangle: the destination is the source's extent
// placed at the given corner.
template <typename SurfaceT>
BlitPresentation ClassifyBltFastCall(SurfaceT* dest, DWORD destX, DWORD destY, SurfaceT* source,
                                     const RECT* sourceRect) {
    policy::Extent sourceExtent;
    DWORD sourceCaps = 0;
    const bool haveSource = source && ReadSurfaceGeometry(source, sourceExtent, sourceCaps);
    int copyWidth = haveSource ? static_cast<int>(sourceExtent.width) : 0;
    int copyHeight = haveSource ? static_cast<int>(sourceExtent.height) : 0;
    if (sourceRect) {
        copyWidth = static_cast<int>(sourceRect->right - sourceRect->left);
        copyHeight = static_cast<int>(sourceRect->bottom - sourceRect->top);
    }
    RECT destRect = {static_cast<LONG>(destX), static_cast<LONG>(destY),
                     static_cast<LONG>(destX) + copyWidth, static_cast<LONG>(destY) + copyHeight};
    return ClassifyBlitCall(dest, &destRect, source, sourceRect);
}

IDirectDrawSurface4* AcquireFlipPresentSource4(IDirectDrawSurface4* primarySurface,
                                               IDirectDrawSurface4* destOverride) {
    if (destOverride) {
        destOverride->AddRef();
        return destOverride;
    }
    if (!primarySurface)
        return nullptr;
    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface4* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer)
        return backBuffer;
    return nullptr;
}

IDirectDrawSurface* AcquireFlipPresentSourceLegacy(IDirectDrawSurface* primarySurface,
                                                   IDirectDrawSurface* destOverride) {
    if (destOverride) {
        destOverride->AddRef();
        return destOverride;
    }
    if (!primarySurface)
        return nullptr;
    DDSCAPS backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer)
        return backBuffer;
    return nullptr;
}

}  // namespace



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
    if (!HookIsShuttingDown() && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        IDirectDrawSurface* presentSource = AcquireFlipPresentSourceLegacy(surface, destOverride);
        HandlePresentationLegacySurface(surface, presentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
        if (presentSource)
            presentSource->Release();
    }
    const HRESULT hr = record.flip(surface, destOverride, ddraw_hook_flags);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        NotePresentationComplete();
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface,  LPRECT destRect, 
                                                          IDirectDrawSurface* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                          DDBLTFX* ddraw_hook_bltFx) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const bool live = !HookIsShuttingDown() && ddraw_hook_g_DDrawBootstrapDepth == 0;
    const BlitPresentation presentation =
        live ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect) : BlitPresentation{};
    if (presentation.kind != policy::PresentKind::None) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
    }
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationLegacySurface(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        HandlePresentationLegacySurface(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                        presentation.changedRect);
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBltFast(IDirectDrawSurface* surface, DWORD dwX, DWORD dwY,
                                                      IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD dwTrans) {

    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.bltFast)
        return DDERR_GENERIC;
    const bool live = !HookIsShuttingDown() && ddraw_hook_g_DDrawBootstrapDepth == 0;
    const BlitPresentation presentation =
        live ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect) : BlitPresentation{};
    if (presentation.kind != policy::PresentKind::None) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
    }
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationLegacySurface(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }
    const HRESULT hr = record.bltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        HandlePresentationLegacySurface(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                        presentation.changedRect);
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
        // The application drew straight into the scanout surface, so the frame
        // is already visible and the overlay goes back on top of it.
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandlePresentationLegacySurface(surface, nullptr, policy::PresentKind::DirectScanout, false, policy::Rect{});
        NotePresentationComplete();
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

    // The overlay has to be inside the image this flip publishes. Compositing
    // into the surface that is already on screen races scanout, and the flip
    // then replaces that surface with one the overlay never touched.
    IDirectDrawSurface7* flipPresentSource = AcquireFlipPresentSource(surface, destOverride);
    ComposePresentation(surface, flipPresentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
    if (flipPresentSource)
        flipPresentSource->Release();

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

    NotePresentationComplete();

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

    IDirectDrawSurface4* flipPresentSource = AcquireFlipPresentSource4(surface, destOverride);
    HandlePresentationSurface4(surface, flipPresentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
    if (flipPresentSource)
        flipPresentSource->Release();

    HRESULT hr = ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags);
    NotePresentationComplete();
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface,  LPRECT destRect, 
                                                     IDirectDrawSurface7* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                     void* ddraw_hook_bltFx) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    const bool eligible = surface && surface != ddraw_hook_g_HookSurfacePrototype &&
                          (!ddraw_hook_g_PrimarySurface || surface == ddraw_hook_g_PrimarySurface);
    const BlitPresentation presentation =
        eligible ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect) : BlitPresentation{};

    // A full-surface blit onto a single-buffered scanout surface is this
    // application's present, so the overlay goes into its source first.
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        ComposePresentation(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    HRESULT hr = ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        ComposePresentation(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                            presentation.changedRect);
    }

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface7BltFast(IDirectDrawSurface7* surface, DWORD dwX, DWORD dwY,
                                                 IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD dwTrans) {

    if (!ddraw_hook_oDDSurface7BltFast)
        return DDERR_GENERIC;
    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface7BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "BltFast");
    }

    const bool eligible = surface && surface != ddraw_hook_g_HookSurfacePrototype &&
                          (!ddraw_hook_g_PrimarySurface || surface == ddraw_hook_g_PrimarySurface);
    const BlitPresentation presentation =
        eligible ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect) : BlitPresentation{};

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        ComposePresentation(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    HRESULT hr = ddraw_hook_oDDSurface7BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        ComposePresentation(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                            presentation.changedRect);
    }

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface,  LPRECT destRect, 
                                                     IDirectDrawSurface4* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                     void* ddraw_hook_bltFx) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface4Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != ddraw_hook_g_HookSurfacePrototype4 && !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Blt4");
    }

    const bool eligible = surface && surface != ddraw_hook_g_HookSurfacePrototype4 &&
                          (!ddraw_hook_g_PrimarySurface4 || surface == ddraw_hook_g_PrimarySurface4);
    const BlitPresentation presentation =
        eligible ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect) : BlitPresentation{};

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationSurface4(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    HRESULT hr = ddraw_hook_oDDSurface4Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        HandlePresentationSurface4(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                   presentation.changedRect);
    }

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDDSurface4BltFast(IDirectDrawSurface4* surface, DWORD dwX, DWORD dwY,
                                                 IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD dwTrans) {

    if (!ddraw_hook_oDDSurface4BltFast)
        return DDERR_GENERIC;
    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface4BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    }
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != ddraw_hook_g_HookSurfacePrototype4 && !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "BltFast4");
    }

    const bool eligible = surface && surface != ddraw_hook_g_HookSurfacePrototype4 &&
                          (!ddraw_hook_g_PrimarySurface4 || surface == ddraw_hook_g_PrimarySurface4);
    const BlitPresentation presentation =
        eligible ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect) : BlitPresentation{};

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationSurface4(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    HRESULT hr = ddraw_hook_oDDSurface4BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        HandlePresentationSurface4(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                   presentation.changedRect);
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
        // Software rendering straight into the scanout surface: the frame is
        // already visible, so the overlay is restored on top of it.
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        ComposePresentation(surface, nullptr, policy::PresentKind::DirectScanout, false, policy::Rect{});
        NotePresentationComplete();
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface,  LPRECT ddraw_hook_rect) {


    HRESULT hr = ddraw_hook_oDDSurface4Unlock(surface, ddraw_hook_rect);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface4) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        HandlePresentationSurface4(surface, nullptr, policy::PresentKind::DirectScanout, false, policy::Rect{});
        NotePresentationComplete();
    }
    return hr;

}
