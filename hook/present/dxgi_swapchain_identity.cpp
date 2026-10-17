// Whether a D3D10/D3D11 swapchain created below a foreign overlay chain goes back to the caller
// as the real object or as CE's CWrapDXGISwapChain (policy:
// ce::overlay_compat::ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain).
//
// The proxy was the D3D11 default even when CE's body hooks already saw every Present below the
// chain. An overlay whose factory hook sits above CE then receives the proxy: Steam rejected its
// vtable ("VTable for swap chain in unexpected module"), tracked the proxy pointer, and its
// Present hook - called with the real object - never drew (BioShock Infinite, 20261017_003122).

#include "dxgi_shared_internal.h"

namespace DXGIShared {

bool ShouldReturnRealD3D11SwapchainBelowForeignChain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return false;
    }
    const APIType api = DetectAPIType(pSwapChain);
    const bool d3d11Family = api == APIType::D3D11 || api == APIType::D3D10;
    const size_t loadedOverlays =
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay);
    if (!d3d11Family || loadedOverlays == 0) {
        return false;  // nothing of this rule's business; the caller's other rules decide
    }
    const bool presentMethodsBelowChain = ArePresentMethodsInterceptedBelowForeignChain();
    // False for CE's own proxy (its Present lives in capture_hook) and for a present interposer's
    // app-facing chain, so neither reaches the resize install below.
    const bool presentCovered = IsSwapchainPresentCoveredByDeepBodyHook(pSwapChain);
    bool resizeCovered = false;
    if (presentMethodsBelowChain && presentCovered) {
        // Idempotent: the Present refresh before the wrap decision normally installed them already.
        InstallResizeReconciliationHooks(pSwapChain, source ? source : "D3D11 identity");
        resizeCovered = IsSwapchainResizeCoveredByReconciliationHooks(pSwapChain);
    }
    const bool preserve = ce::overlay_compat::ShouldPreserveD3D11SwapchainIdentityBelowForeignPresentChain(
        d3d11Family, presentMethodsBelowChain, loadedOverlays, presentCovered, resizeCovered);
    // One line per create: creates are rare, and the inputs say which coverage was missing.
    HookLogImportant(
        "%s: D3D%d swapchain %p below the foreign chain (overlays=%zu) -> %s (presentMethodsBelowChain=%d "
        "presentCovered=%d resizeCovered=%d)",
        source ? source : "CreateSwapChain", api == APIType::D3D11 ? 11 : 10, pSwapChain, loadedOverlays,
        preserve ? "returning the REAL swapchain" : "wrapping (coverage incomplete)", presentMethodsBelowChain ? 1 : 0,
        presentCovered ? 1 : 0, resizeCovered ? 1 : 0);
    return preserve;
}

}  // namespace DXGIShared
