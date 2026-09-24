#include "ddraw_hook_internal.h"

namespace policy = ce::ddraw_present_policy;

HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                               DWORD flags, HANDLE event) {
    const HRESULT hr = ddraw_hook_oDDSurface7Lock(surface, destRect, surfaceDesc, flags, event);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype &&
        !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Lock");
    }
    if (!HookIsShuttingDown() && SUCCEEDED(hr)) {
        const bool writable = (flags & DDLOCK_READONLY) == 0;
        BeginDirectDrawSurfaceLock(surface, writable);
        // A null rectangle means the whole surface and is intentionally marked
        // conservatively by the tracker.
        if (writable && ddraw_hook_g_CaptureRecurse == 0) {
            const policy::Rect changed = destRect ? policy::Rect{destRect->left, destRect->top, destRect->right,
                                                                  destRect->bottom}
                                                   : policy::Rect{};
            RecordNativeLegacyD3DSurfaceWrite(surface, destRect != nullptr, changed);
        }
        if (writable)
            MarkDirectDrawSurfaceWrite(surface, destRect, true);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                               DWORD flags, HANDLE event) {
    const HRESULT hr = ddraw_hook_oDDSurface4Lock(surface, destRect, surfaceDesc, flags, event);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface != ddraw_hook_g_HookSurfacePrototype4 &&
        !ddraw_hook_g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Lock4");
    }
    if (!HookIsShuttingDown() && SUCCEEDED(hr)) {
        const bool writable = (flags & DDLOCK_READONLY) == 0;
        BeginDirectDrawSurfaceLock(surface, writable);
        if (writable && ddraw_hook_g_CaptureRecurse == 0) {
            const policy::Rect changed = destRect ? policy::Rect{destRect->left, destRect->top, destRect->right,
                                                                  destRect->bottom}
                                                   : policy::Rect{};
            RecordNativeLegacyD3DSurfaceWrite(surface, destRect != nullptr, changed);
        }
        if (writable)
            MarkDirectDrawSurfaceWrite(surface, destRect, true);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface7GetDC(IDirectDrawSurface7* surface, HDC* hdc) {
    if (!ddraw_hook_oDDSurface7GetDC)
        return DDERR_GENERIC;
    const HRESULT hr = ddraw_hook_oDDSurface7GetDC(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && hdc && *hdc) {
        // GDI drawing is not observable per-pixel; GetDC is the whole-surface
        // answer, and ReleaseDC restores the overlay once the DC is gone.
        if (ddraw_hook_g_CaptureRecurse == 0)
            RecordNativeLegacyD3DSurfaceWrite(surface, false, policy::Rect{});
        MarkDirectDrawSurfaceWrite(surface, nullptr, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface4GetDC(IDirectDrawSurface4* surface, HDC* hdc) {
    if (!ddraw_hook_oDDSurface4GetDC)
        return DDERR_GENERIC;
    const HRESULT hr = ddraw_hook_oDDSurface4GetDC(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && hdc && *hdc) {
        if (ddraw_hook_g_CaptureRecurse == 0)
            RecordNativeLegacyD3DSurfaceWrite(surface, false, policy::Rect{});
        MarkDirectDrawSurfaceWrite(surface, nullptr, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface7ReleaseDC(IDirectDrawSurface7* surface, HDC hdc) {
    if (!ddraw_hook_oDDSurface7ReleaseDC)
        return DDERR_GENERIC;
    const HRESULT hr = ddraw_hook_oDDSurface7ReleaseDC(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        diag.getDcPresentations.fetch_add(1, std::memory_order_relaxed);
        if (ddraw_hook_g_CaptureRecurse != 0) {
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
            if (ComposePresentation(surface, nullptr, policy::PresentKind::DirectScanout, false, policy::Rect{}))
                NotePresentationComplete();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface4ReleaseDC(IDirectDrawSurface4* surface, HDC hdc) {
    if (!ddraw_hook_oDDSurface4ReleaseDC)
        return DDERR_GENERIC;
    const HRESULT hr = ddraw_hook_oDDSurface4ReleaseDC(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface4) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        diag.getDcPresentations.fetch_add(1, std::memory_order_relaxed);
        if (ddraw_hook_g_CaptureRecurse != 0) {
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
            if (HandlePresentationSurface4(surface, nullptr, policy::PresentKind::DirectScanout, false,
                                           policy::Rect{})) {
                NotePresentationComplete();
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT rect) {
    const HRESULT hr = ddraw_hook_oDDSurface7Unlock(surface, rect);
    // Tracking is resolved on every Unlock attempt, failed or not: leaked depth
    // would defer DirectScanout presentations and the freeze-watchdog
    // heartbeat for the rest of the session.
    const DirectDrawLockAccess access = CompleteDirectDrawSurfaceLock(surface, SUCCEEDED(hr));
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        if (ddraw_hook_g_CaptureRecurse != 0) {
            // CE's own composite and capture locks pass through these hooks;
            // their unlock is not an application presentation.
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else if (access != DirectDrawLockAccess::ReadOnly) {
            diag.primaryUnlockPresentations.fetch_add(1, std::memory_order_relaxed);
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
            const policy::Rect changed = rect ? policy::Rect{rect->left, rect->top, rect->right, rect->bottom}
                                              : policy::Rect{};
            if (ComposePresentation(surface, nullptr, policy::PresentKind::DirectScanout, rect != nullptr, changed))
                NotePresentationComplete();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT rect) {
    const HRESULT hr = ddraw_hook_oDDSurface4Unlock(surface, rect);
    // Tracking is resolved on every Unlock attempt, failed or not: leaked depth
    // would defer DirectScanout presentations and the freeze-watchdog
    // heartbeat for the rest of the session.
    const DirectDrawLockAccess access = CompleteDirectDrawSurfaceLock(surface, SUCCEEDED(hr));
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && surface && surface == ddraw_hook_g_PrimarySurface4) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        if (ddraw_hook_g_CaptureRecurse != 0) {
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else if (access != DirectDrawLockAccess::ReadOnly) {
            diag.primaryUnlockPresentations.fetch_add(1, std::memory_order_relaxed);
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
            const policy::Rect changed = rect ? policy::Rect{rect->left, rect->top, rect->right, rect->bottom}
                                              : policy::Rect{};
            if (HandlePresentationSurface4(surface, nullptr, policy::PresentKind::DirectScanout, rect != nullptr,
                                           changed)) {
                NotePresentationComplete();
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                     DDSURFACEDESC* surfaceDesc, DWORD flags, HANDLE event) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    const HRESULT hr = record.lock ? record.lock(surface, destRect, surfaceDesc, flags, event) : DDERR_GENERIC;
    if (!HookIsShuttingDown() && SUCCEEDED(hr)) {
        const bool writable = (flags & DDLOCK_READONLY) == 0;
        BeginDirectDrawSurfaceLock(surface, writable);
        if (writable && ddraw_hook_g_CaptureRecurse == 0) {
            const policy::Rect changed =
                destRect ? policy::Rect{destRect->left, destRect->top, destRect->right, destRect->bottom}
                         : policy::Rect{};
            RecordNativeLegacyD3DSurfaceWrite(surface, destRect != nullptr, changed);
        }
        if (writable)
            MarkDirectDrawSurfaceWrite(surface, destRect, true);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID surfaceData) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, surfaceData);
    // Tracking is resolved on every Unlock attempt, failed or not: leaked depth
    // would defer DirectScanout presentations and the freeze-watchdog
    // heartbeat for the rest of the session.
    const DirectDrawLockAccess access = CompleteDirectDrawSurfaceLock(surface, SUCCEEDED(hr));
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        if (ddraw_hook_g_CaptureRecurse != 0) {
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else if (access != DirectDrawLockAccess::ReadOnly) {
            diag.primaryUnlockPresentations.fetch_add(1, std::memory_order_relaxed);
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
            if (HandlePresentationLegacySurface(surface, nullptr, policy::PresentKind::DirectScanout, false,
                                                policy::Rect{})) {
                NotePresentationComplete();
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyGetDC(IDirectDrawSurface* surface, HDC* hdc) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.getDc)
        return DDERR_GENERIC;
    const HRESULT hr = record.getDc(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && hdc && *hdc) {
        if (ddraw_hook_g_CaptureRecurse == 0)
            RecordNativeLegacyD3DSurfaceWrite(surface, false, policy::Rect{});
        MarkDirectDrawSurfaceWrite(surface, nullptr, false);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyReleaseDC(IDirectDrawSurface* surface, HDC hdc) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.releaseDc)
        return DDERR_GENERIC;
    const HRESULT hr = record.releaseDc(surface, hdc);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        auto& diag = ddraw_hook_g_PresentationDiagnostics;
        diag.getDcPresentations.fetch_add(1, std::memory_order_relaxed);
        if (ddraw_hook_g_CaptureRecurse != 0) {
            diag.reentrantPresentations.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (!DirectDrawSurfaceHasPendingWrite(surface))
                MarkDirectDrawSurfaceWrite(surface, nullptr, false);
            ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
            if (HandlePresentationLegacySurface(surface, nullptr, policy::PresentKind::DirectScanout, false,
                                                policy::Rect{})) {
                NotePresentationComplete();
            }
        }
    }
    return hr;
}
