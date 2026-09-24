#include "ddraw_hook_internal.h"

#include "ddraw_hook_blit_classification.h"
#include "ddraw_hook_present_overrides.h"
#include "../common/ddraw_chain_lifetime_policy.h"

// The DirectDraw 4 generation's detours: IDirectDraw4::CreateSurface and the
// IDirectDrawSurface4 presentation paths. They ask the shared classification in
// ddraw_hook_blit_classification.h the same questions the 7 and legacy paths
// do, and answer a nested presentation the same way - by returning.

using namespace ce::ddraw_detours;

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
    const bool applicationPrimary = ce::ddraw_chain_lifetime::ShouldReleaseChainBeforeCreation(
        IsPrimarySurfaceDesc(pDesc), ddraw_hook_g_DDrawBootstrapDepth, HookIsShuttingDown());
    if (applicationPrimary)
        ReleaseDirectDrawChainBeforePrimaryCreation("DirectDraw4");
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    if (logCreate) {
        HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p ordinal=%u", hr,
                (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr, createOrdinal);
    }
    if (applicationPrimary && FAILED(hr))
        LogApplicationPrimaryCreationFailure("DirectDraw4", hr, createOrdinal);
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

HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface,  IDirectDrawSurface4* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    if (HookIsShuttingDown()) {
        return ddraw_hook_oDDSurface4Flip ? ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags)
                                         : DDERR_GENERIC;
    }
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant()) {
        return AnswerReenteredPresentation(surface, DDSURFACE7_VTABLE_FLIP, "Flip",
                                          reinterpret_cast<void*>(ddraw_hook_oDDSurface4Flip),
                                          CE_DDRAW_RETURN_ADDRESS(), ddraw_hook_oDDSurface4Flip, surface,
                                          destOverride, ddraw_hook_flags);
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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return AnswerReenteredPresentation(surface, DDSURFACE7_VTABLE_BLT, "Blt",
                                          reinterpret_cast<void*>(ddraw_hook_oDDSurface4Blt),
                                          CE_DDRAW_RETURN_ADDRESS(), ddraw_hook_oDDSurface4Blt, surface, destRect,
                                          srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    }

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
    DirectDrawPresentDetourScope presentDetourScope;
    if (presentDetourScope.IsReentrant() && policy::PresentKindIsPresentation(presentation.kind)) {
        return AnswerReenteredPresentation(surface, DDSURFACE7_VTABLE_BLTFAST, "BltFast",
                                          reinterpret_cast<void*>(ddraw_hook_oDDSurface4BltFast),
                                          CE_DDRAW_RETURN_ADDRESS(), ddraw_hook_oDDSurface4BltFast, surface, dwX,
                                          dwY, srcSurface, srcRect, dwTrans);
    }

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
