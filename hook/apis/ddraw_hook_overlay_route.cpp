#include "ddraw_hook_internal.h"

// Which renderer the DirectDraw overlay route uses, and the rule that the
// backend the adapter holds must always match the route actually executing.
//
// Gothic II session `20260914_182411` is what this exists to prevent. The
// native Direct3D 7 backend came up on the first Flip, the application then
// stopped rendering into the surface being presented, and the route fell back
// to the D3D9Ex composite - but `OverlayAdapter::InitDX9` silently succeeds
// when an adapter is already initialized, so the backend stayed bound to the
// application's own device. Every composite frame afterwards issued
// `BeginScene`, sampler and render-state changes, a draw and `EndScene` on the
// game's device, at a point the game never asked for, outside
// `LegacyD3DInternalScope` - so the forced-filtering layer recorded CE's
// sampler states as the application's - and then read back a helper backbuffer
// the overlay had never been drawn into. Half a second later Steam's overlay
// copied from a null frame pointer and the process died.
//
// So: the required backend is derived from the presentation, the adapter is
// switched to it before anything renders, and nothing renders at all while the
// two disagree.

namespace {

// An application that alternated presentation shapes could otherwise re-upload
// the font atlas on every frame. The composite route works for every shape, so
// it is what the route latches to once the budget is spent.
constexpr uint32_t kOverlayRouteSwitchLimit = 8;

void RetireOverlayBackendForRouteChange() {
    if (!g_OverlayAdapter.IsInitialized())
        return;
    // Releasing the native backend hands Direct3D 7 objects back through the
    // application's own interfaces, which the forced-filtering layer watches.
    LegacyD3DInternalScope internalScope;
    g_OverlayAdapter.Shutdown();
}

// A route change only takes effect once this many presentations in a row have
// asked for it. The application's render target legitimately alternates - a
// Gothic II session moved between its flip target and something else five
// times in eight seconds - and acting on every single presentation rebuilt the
// backend's GPU objects, inside the Flip detour, each time.
constexpr uint32_t kOverlayRouteStabilityPresentations = 45;

bool RouteChangeIsStable(DDrawOverlayRoute requestedRoute) {
    if (ddraw_hook_g_OverlayRoutePending != requestedRoute) {
        ddraw_hook_g_OverlayRoutePending = requestedRoute;
        ddraw_hook_g_OverlayRoutePendingPresentations = 1;
        return false;
    }
    if (ddraw_hook_g_OverlayRoutePendingPresentations < kOverlayRouteStabilityPresentations) {
        ++ddraw_hook_g_OverlayRoutePendingPresentations;
        return false;
    }
    return true;
}

bool AllowOverlayRouteSwitch() {
    if (ddraw_hook_g_OverlayRouteLatchedToComposite)
        return false;
    if (ddraw_hook_g_OverlayRouteSwitches < kOverlayRouteSwitchLimit)
        return true;

    ddraw_hook_g_OverlayRouteLatchedToComposite = true;
    HookLogImportant(
        "DDraw: Overlay route switched %u times; latching on the D3D9Ex composite, which works for every "
        "presentation shape",
        ddraw_hook_g_OverlayRouteSwitches);
    return false;
}

}  // namespace

IDirect3DDevice7* AcquireNativeLegacyD3DDeviceForSurface(IDirectDrawSurface7* presentedSurface) {
    if (!presentedSurface || ddraw_hook_g_OverlayRouteLatchedToComposite)
        return nullptr;

    // Opt-in. Drawing the overlay with the application's own Direct3D 7 device
    // removes the composite's readback, but it has twice taken down a
    // co-resident Steam overlay in Gothic II with an identical fault inside
    // gameoverlayrenderer.dll, and the crash dumps carry no 32-bit stack to
    // attribute it further. The composite draws the same overlay on every
    // title, so it is what runs unless this is turned on deliberately.
    if (!GetActiveGraphicsConfig().legacyD3DNativeOverlay)
        return nullptr;

    IDirect3DDevice7* device = AcquireLegacyD3D7Device();
    if (!device)
        return nullptr;

    // The device has to be rendering into the exact surface this presentation
    // publishes. Drawing into anything else would put the overlay where nobody
    // looks and issue device work the application never asked for.
    using GetRenderTarget7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, IDirectDrawSurface7**);
    void** deviceVTable = *(void***)device;
    auto getRenderTarget = reinterpret_cast<GetRenderTarget7_t>(deviceVTable[D3D7_VTABLE_GETRENDERTARGET]);

    IDirectDrawSurface7* renderTarget = nullptr;
    bool targetsThisSurface = false;
    if (getRenderTarget && SUCCEEDED(getRenderTarget(device, &renderTarget)) && renderTarget != nullptr) {
        // DirectDraw hands out one IDirectDrawSurface7 per surface, so the
        // pointers match outright in every normal case; the COM identity
        // comparison is only the fallback for an aggregated or wrapped object.
        targetsThisSurface = renderTarget == presentedSurface ||
                             DirectDrawObjectIdentity(renderTarget) == DirectDrawObjectIdentity(presentedSurface);
    }
    if (renderTarget)
        renderTarget->Release();

    if (!targetsThisSurface) {
        device->Release();
        return nullptr;
    }
    return device;
}

bool EnsureOverlayRouteBackend(DDrawOverlayRoute requiredRoute, IDirect3DDevice7* nativeDevice) {
    const OverlayBackendType currentBackend = g_OverlayAdapter.GetBackendType();

    if (requiredRoute == DDrawOverlayRoute::NativeLegacyD3D) {
        auto* nativeBackend = currentBackend == OverlayBackendType::D3D7
                                  ? static_cast<CustomOverlay::D3D7Backend*>(g_OverlayAdapter.GetBackend())
                                  : nullptr;
        const bool boundToThisDevice = nativeBackend && nativeBackend->GetDevice() == static_cast<void*>(nativeDevice);
        if (ce::ddraw_present_policy::BackendCanRenderRoute(ce::ddraw_present_policy::OverlayRoute::NativeDevice,
                                                            boundToThisDevice, false)) {
            ddraw_hook_g_OverlayRoute = requiredRoute;
            ddraw_hook_g_OverlayRoutePending = requiredRoute;
            ddraw_hook_g_OverlayRoutePendingPresentations = 0;
            return true;
        }

        if (!RouteChangeIsStable(requiredRoute) || !AllowOverlayRouteSwitch())
            return false;

        RetireOverlayBackendForRouteChange();
        // The composite's staging surfaces belong to a route that is not
        // running; the native path never touches them.
        ddraw_hook_g_DDrawCapture.ReleaseCompositeRegionResources();

        bool initialized = false;
        {
            LegacyD3DInternalScope internalScope;
            initialized = g_OverlayAdapter.InitD3D7(nativeDevice);
        }
        if (!initialized) {
            ddraw_hook_g_OverlayRouteLatchedToComposite = true;
            HookLogImportant("DDraw: Direct3D 7 overlay backend unavailable; latching on the D3D9Ex composite");
            return false;
        }

        // OverlayAdapter::Init* reports success for an adapter that is already
        // initialized with a different backend. Confirming the backend that is
        // actually loaded is what keeps that from silently leaving the wrong
        // one in place, which is exactly how the Gothic II crash happened.
        auto* initializedBackend = g_OverlayAdapter.GetBackendType() == OverlayBackendType::D3D7
                                       ? static_cast<CustomOverlay::D3D7Backend*>(g_OverlayAdapter.GetBackend())
                                       : nullptr;
        if (!initializedBackend || initializedBackend->GetDevice() != static_cast<void*>(nativeDevice)) {
            ddraw_hook_g_OverlayRouteLatchedToComposite = true;
            HookLogImportant("DDraw: Direct3D 7 overlay backend did not take the device; latching on the composite");
            return false;
        }

        ++ddraw_hook_g_OverlayRouteSwitches;
        ddraw_hook_g_OverlayRoute = requiredRoute;
        if (ddraw_hook_g_CachedHwnd)
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        HookLogImportant("DDraw: Overlay route -> the application's Direct3D 7 device (device=%p switch=%u)",
                         static_cast<void*>(nativeDevice), ddraw_hook_g_OverlayRouteSwitches);
        return true;
    }

    if (ce::ddraw_present_policy::BackendCanRenderRoute(
            ce::ddraw_present_policy::OverlayRoute::HelperComposite, false,
            currentBackend == OverlayBackendType::DX9 && ddraw_hook_g_DDrawCapture.d3d9DeviceEx != nullptr)) {
        ddraw_hook_g_OverlayRoute = requiredRoute;
        ddraw_hook_g_OverlayRoutePending = requiredRoute;
        ddraw_hook_g_OverlayRoutePendingPresentations = 0;
        return true;
    }

    if (currentBackend == OverlayBackendType::D3D7) {
        // The native backend is loaded and this presentation cannot use it.
        // Nothing renders until the change has proved stable, because tearing
        // the backend down and rebuilding it on every alternating presentation
        // churns Direct3D 7 objects inside the application's Flip.
        if (!RouteChangeIsStable(requiredRoute))
            return false;
        RetireOverlayBackendForRouteChange();
        if (AllowOverlayRouteSwitch())
            ++ddraw_hook_g_OverlayRouteSwitches;
    }

    if (!ddraw_hook_g_DDrawCapture.EnsureOverlayCompositeDevice())
        return false;

    if (!g_OverlayAdapter.IsInitialized()) {
        if (ddraw_hook_g_CachedHwnd) {
            InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        }
        if (!g_OverlayAdapter.InitDX9(ddraw_hook_g_DDrawCapture.d3d9DeviceEx)) {
            return false;
        }
        if (ddraw_hook_g_CachedHwnd)
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        HookLogImportant("DDraw: Overlay route -> the D3D9Ex composite (helper=%p switch=%u)",
                         static_cast<void*>(ddraw_hook_g_DDrawCapture.d3d9DeviceEx),
                         ddraw_hook_g_OverlayRouteSwitches);
    }

    // A backend that is neither of the two this route knows how to drive would
    // render into something the route is not reading back. This is the check
    // whose absence produced the Gothic II crash, so it is explicit rather
    // than implied by the branches above.
    if (g_OverlayAdapter.GetBackendType() != OverlayBackendType::DX9) {
        static std::atomic<int> s_mismatchLogCount{0};
        if (s_mismatchLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
            HookLogImportant("DDraw: Overlay suppressed - the composite route cannot render through backend %d",
                             static_cast<int>(g_OverlayAdapter.GetBackendType()));
        }
        return false;
    }

    ddraw_hook_g_OverlayRoute = requiredRoute;
    return true;
}
