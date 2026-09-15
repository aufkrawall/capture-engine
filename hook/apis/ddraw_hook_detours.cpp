#include "ddraw_hook_internal.h"

#include "ddraw_hook_present_overrides.h"

namespace {

namespace policy = ce::ddraw_present_policy;

// One presentation classification for a blit-shaped call.
struct BlitPresentation {
    policy::PresentKind kind = policy::PresentKind::None;
    policy::Rect changedRect;
    bool haveChangedRect = false;
};

bool BlitCopiesSourceExactly(DWORD flags) {
    // Scheduling/presentation hints do not change pixels. Every other Blt flag
    // can key, alpha-blend, fill, rotate, ROP or otherwise transform the source;
    // in that case compose into the destination after the operation instead of
    // assuming an overlay stamped into the source will survive the copy.
    constexpr DWORD kPassThroughFlags =
        DDBLT_ASYNC | DDBLT_WAIT | DDBLT_DONOTWAIT | DDBLT_PRESENTATION | DDBLT_LAST_PRESENTATION;
    return (flags & ~kPassThroughFlags) == 0;
}

bool BltFastCopiesSourceExactly(DWORD flags) {
    constexpr DWORD kPassThroughFlags = DDBLTFAST_WAIT | DDBLTFAST_DONOTWAIT;
    return (flags & ~kPassThroughFlags) == 0;
}

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
BlitPresentation ClassifyBlitCall(SurfaceT* dest, const RECT* destRect, SurfaceT* source, const RECT* sourceRect,
                                  bool sourceCopyIsExact) {
    BlitPresentation result;
    policy::BlitGeometry geometry;
    DWORD destCaps = 0;
    if (!ReadSurfaceGeometry(dest, geometry.dest, destCaps))
        return result;

    // The destination extent makes a null Blt rectangle exact as well. Keep
    // this even for offscreen/back-buffer writes: a native overlay may already
    // be in that surface even though the write is not itself a presentation.
    result.haveChangedRect = true;
    result.changedRect = destRect
                             ? ToPolicyRect(*destRect)
                             : policy::Rect{0, 0, static_cast<int>(geometry.dest.width),
                                            static_cast<int>(geometry.dest.height)};

    // Only the primary surface is being scanned out. A back buffer carries
    // DDSCAPS_BACKBUFFER without DDSCAPS_PRIMARYSURFACE and is published later
    // by Flip; writing the primary is visible the moment the blit completes,
    // flip chain or not.
    geometry.destIsScanout = (destCaps & DDSCAPS_PRIMARYSURFACE) != 0;
    geometry.destIsBackBuffer = !geometry.destIsScanout && (destCaps & DDSCAPS_BACKBUFFER) != 0;
    geometry.destOwnsFlipChain = (destCaps & DDSCAPS_FLIP) != 0;
    if (geometry.destIsBackBuffer || !geometry.destIsScanout) {
        // Flip owns a back buffer and an offscreen destination is not a
        // presentation at all, so the source never has to be described.
        return result;
    }
    if (destRect) {
        geometry.haveDestRect = true;
        geometry.destRect = ToPolicyRect(*destRect);
    }

    DWORD sourceCaps = 0;
    if (sourceCopyIsExact && source && ReadSurfaceGeometry(source, geometry.source, sourceCaps)) {
        geometry.haveSource = true;
        if (sourceRect) {
            geometry.haveSourceRect = true;
            geometry.sourceRect = ToPolicyRect(*sourceRect);
        }
    }

    result.kind = policy::ClassifyBlit(geometry);
    return result;
}

// BltFast has no destination rectangle: the destination is the source's extent
// placed at the given corner.
template <typename SurfaceT>
BlitPresentation ClassifyBltFastCall(SurfaceT* dest, DWORD destX, DWORD destY, SurfaceT* source,
                                     const RECT* sourceRect, bool sourceCopyIsExact) {
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
    return ClassifyBlitCall(dest, &destRect, source, sourceRect, sourceCopyIsExact);
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

    const uint32_t createOrdinal =
        ddraw_hook_g_PresentationDiagnostics.surfaceCreations.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool logCreate = IsPrimarySurfaceDesc(pDesc) || createOrdinal <= 4 || (createOrdinal % 256) == 0;
    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        if (ddraw_hook_g_DDrawBootstrapDepth == 0 && IsPrimarySurfaceDesc(pDesc)) {
            ResetDirectDrawPresentationStateForPrimaryChange();
            ddraw_hook_g_PrimarySurface = nullptr;
            ddraw_hook_g_PrimarySurface4 = nullptr;
        }
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (ddraw_hook_g_DDrawBootstrapDepth == 0 && logCreate) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d ordinal=%u",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0, createOrdinal);
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
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Flip, true, ddraw_hook_flags);
    IDirectDrawSurface* presentSource = nullptr;
    if (!HookIsShuttingDown() && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        presentSource = AcquireFlipPresentSourceLegacy(surface, destOverride);
        HandlePresentationLegacySurface(surface, presentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
    }
    presentationOverride.PrepareForCall();
    const HRESULT hr = record.flip(surface, destOverride, ddraw_hook_flags);
    presentationOverride.Complete(hr);
    NoteDirectDrawPresentationAttempt(policy::PresentOperation::Flip, hr);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(presentSource, surface, true);
        PublishNativeLegacyD3DOverlay(presentSource, surface, true);
        NotePresentationComplete();
    }
    if (presentSource)
        presentSource->Release();
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
        live ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect,
                                BlitCopiesSourceExactly(ddraw_hook_flags))
             : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Blt,
        presentation.kind == policy::PresentKind::BlitPresent, ddraw_hook_flags);
    if (presentation.kind != policy::PresentKind::None) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
    }
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationLegacySurface(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }
    presentationOverride.PrepareForCall();
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::Blt, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (HandlePresentationLegacySurface(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                            presentation.changedRect)) {
            NotePresentationComplete();
        }
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
        live ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect,
                                   BltFastCopiesSourceExactly(dwTrans))
             : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::BltFast,
        presentation.kind == policy::PresentKind::BlitPresent, dwTrans);
    if (presentation.kind != policy::PresentKind::None) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
    }
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationLegacySurface(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }
    presentationOverride.PrepareForCall();
    const HRESULT hr = record.bltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::BltFast, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (HandlePresentationLegacySurface(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                            presentation.changedRect)) {
            NotePresentationComplete();
        }
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis,  DDSURFACEDESC2* pDesc, 
                                                                IDirectDrawSurface7** ppSurface,  IUnknown* ddraw_hook_pUnkOuter) {


    const uint32_t createOrdinal =
        ddraw_hook_g_PresentationDiagnostics.surfaceCreations.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool logCreate = IsPrimarySurfaceDesc(pDesc) || createOrdinal <= 4 || (createOrdinal % 256) == 0;
    if (logCreate) {
        HookLog("DDraw: DetourDirectDraw7CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x ordinal=%u)",
                pThis, pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0, createOrdinal);
    }

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
    if (logCreate) {
        HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p ordinal=%u", hr,
                (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr, createOrdinal);
    }
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        if (ddraw_hook_g_DDrawBootstrapDepth == 0 && IsPrimarySurfaceDesc(pDesc)) {
            ResetDirectDrawPresentationStateForPrimaryChange();
            ddraw_hook_g_PrimarySurface4 = nullptr;
        }
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


    const uint32_t createOrdinal =
        ddraw_hook_g_PresentationDiagnostics.surfaceCreations.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool logCreate = IsPrimarySurfaceDesc(pDesc) || createOrdinal <= 4 || (createOrdinal % 256) == 0;
    if (logCreate) {
        HookLog("DDraw: DetourDirectDraw4CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x ordinal=%u)",
                pThis, pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0, createOrdinal);
    }

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
    if (logCreate) {
        HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p ordinal=%u", hr,
                (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr, createOrdinal);
    }
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ppSurface && *ppSurface) {
        if (ddraw_hook_g_DDrawBootstrapDepth == 0 && IsPrimarySurfaceDesc(pDesc)) {
            ResetDirectDrawPresentationStateForPrimaryChange();
            ddraw_hook_g_PrimarySurface = nullptr;
        }
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
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Flip, true, ddraw_hook_flags);

    // The overlay has to be inside the image this flip publishes. Compositing
    // into the surface that is already on screen races scanout, and the flip
    // then replaces that surface with one the overlay never touched.
    IDirectDrawSurface7* flipPresentSource = AcquireFlipPresentSource(surface, destOverride);
    ComposePresentation(surface, flipPresentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags);
    presentationOverride.Complete(hr);
    NoteDirectDrawPresentationAttempt(policy::PresentOperation::Flip, hr);

    if (SUCCEEDED(hr)) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(flipPresentSource, surface, true);
        PublishNativeLegacyD3DOverlay(flipPresentSource, surface, true);
        NotePresentationComplete();
    }
    if (flipPresentSource)
        flipPresentSource->Release();

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
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Flip, true, ddraw_hook_flags);

    IDirectDrawSurface4* flipPresentSource = AcquireFlipPresentSource4(surface, destOverride);
    HandlePresentationSurface4(surface, flipPresentSource, policy::PresentKind::FlipChain, false, policy::Rect{});
    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags);
    presentationOverride.Complete(hr);
    NoteDirectDrawPresentationAttempt(policy::PresentOperation::Flip, hr);
    if (SUCCEEDED(hr)) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(flipPresentSource, surface, true);
        PublishNativeLegacyD3DOverlay(flipPresentSource, surface, true);
        NotePresentationComplete();
    }
    if (flipPresentSource)
        flipPresentSource->Release();
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
        eligible ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect,
                                    BlitCopiesSourceExactly(ddraw_hook_flags))
                 : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Blt,
        presentation.kind == policy::PresentKind::BlitPresent, ddraw_hook_flags);
    if (eligible && presentation.kind == policy::PresentKind::None) {
        ddraw_hook_g_PresentationDiagnostics.ignoredBlits.fetch_add(1, std::memory_order_relaxed);
    }

    // A full-surface blit onto a single-buffered scanout surface is this
    // application's present, so the overlay goes into its source first.
    if (presentation.kind == policy::PresentKind::BlitPresent) {
        ComposePresentation(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::Blt, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (ComposePresentation(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                presentation.changedRect)) {
            NotePresentationComplete();
        }
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
        eligible ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect,
                                       BltFastCopiesSourceExactly(dwTrans))
                 : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::BltFast,
        presentation.kind == policy::PresentKind::BlitPresent, dwTrans);
    if (eligible && presentation.kind == policy::PresentKind::None) {
        ddraw_hook_g_PresentationDiagnostics.ignoredBlits.fetch_add(1, std::memory_order_relaxed);
    }

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        ComposePresentation(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface7BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::BltFast, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (ComposePresentation(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                presentation.changedRect)) {
            NotePresentationComplete();
        }
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
        eligible ? ClassifyBlitCall(surface, destRect, srcSurface, srcRect,
                                    BlitCopiesSourceExactly(ddraw_hook_flags))
                 : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::Blt,
        presentation.kind == policy::PresentKind::BlitPresent, ddraw_hook_flags);

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationSurface4(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface4Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::Blt, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (HandlePresentationSurface4(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                       presentation.changedRect)) {
            NotePresentationComplete();
        }
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
        eligible ? ClassifyBltFastCall(surface, dwX, dwY, srcSurface, srcRect,
                                       BltFastCopiesSourceExactly(dwTrans))
                 : BlitPresentation{};
    DirectDrawPresentationOverrideScope presentationOverride(
        surface, policy::PresentOperation::BltFast,
        presentation.kind == policy::PresentKind::BlitPresent, dwTrans);

    if (presentation.kind == policy::PresentKind::BlitPresent) {
        HandlePresentationSurface4(surface, srcSurface, presentation.kind, false, policy::Rect{});
    }

    presentationOverride.PrepareForCall();
    HRESULT hr = ddraw_hook_oDDSurface4BltFast(surface, dwX, dwY, srcSurface, srcRect, dwTrans);
    presentationOverride.Complete(hr);
    if (policy::PresentKindIsPresentation(presentation.kind)) {
        NoteDirectDrawPresentationAttempt(policy::PresentOperation::BltFast, hr);
    }
    if (SUCCEEDED(hr) && presentation.kind != policy::PresentKind::BlitPresent) {
        RecordNativeLegacyD3DSurfaceWrite(surface, presentation.haveChangedRect, presentation.changedRect);
    }
    if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::BlitPresent) {
        ddraw_hook_g_DDrawCapture.PublishCompositeState(srcSurface, surface, false);
        PublishNativeLegacyD3DOverlay(srcSurface, surface, false);
        NotePresentationComplete();
    } else if (SUCCEEDED(hr) && presentation.kind == policy::PresentKind::DirectScanout) {
        if (HandlePresentationSurface4(surface, nullptr, presentation.kind, presentation.haveChangedRect,
                                       presentation.changedRect)) {
            NotePresentationComplete();
        }
    }

    return hr;

}
