#include "ddraw_hook_internal.h"

#include "ddraw_hook_blit_classification.h"
#include "ddraw_hook_present_overrides.h"

using namespace ce::ddraw_detours;


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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant()) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_FLIP, "Flip", reinterpret_cast<void*>(record.flip), CE_DDRAW_RETURN_ADDRESS());
    }
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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_BLT, "Blt", reinterpret_cast<void*>(record.blt), CE_DDRAW_RETURN_ADDRESS());
    }
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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_BLTFAST, "BltFast", reinterpret_cast<void*>(record.bltFast), CE_DDRAW_RETURN_ADDRESS());
    }
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


HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface,  IDirectDrawSurface7* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface7Flip ? ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags)
                                         : DDERR_GENERIC;
    }
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant()) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_FLIP, "Flip", reinterpret_cast<void*>(ddraw_hook_oDDSurface7Flip), CE_DDRAW_RETURN_ADDRESS());
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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_BLT, "Blt", reinterpret_cast<void*>(ddraw_hook_oDDSurface7Blt), CE_DDRAW_RETURN_ADDRESS());
    }
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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return RefuseReenteredPresentation(surface, DDSURFACE7_VTABLE_BLTFAST, "BltFast", reinterpret_cast<void*>(ddraw_hook_oDDSurface7BltFast), CE_DDRAW_RETURN_ADDRESS());
    }
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
