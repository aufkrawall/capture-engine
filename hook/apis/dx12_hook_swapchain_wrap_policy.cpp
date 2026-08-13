#include "dx12_hook_internal.h"

// Streamline owns its swapchain lifecycle. A retaining CE wrapper would pin the old chain and
// make the runtime's replacement create fail E_ACCESSDENIED; only its dedicated non-retaining
// wrapper is permitted where CE needs a non-entry view of generated presents.
bool IsStreamlineLoaded() {
    static bool detected = false;
    if (detected) {
        return true;
    }
    if (GetModuleHandleA("sl.interposer.dll") != nullptr) {
        detected = true;
        HookLogImportant("DX12: Streamline interposer detected — skipping retaining swapchain wrapping");
        return true;
    }
    return false;
}

bool IsStreamlineRuntimeSwapchainWrappable(IUnknown* pDevice) {
    if (!pDevice) {
        return false;
    }
    ID3D12CommandQueue* queue = nullptr;
    const bool isCommandQueue = SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&queue))) && queue != nullptr;
    if (queue) {
        queue->Release();
    }
    return isCommandQueue;
}

bool ShouldWrapStreamlineRuntimeSwapchainForForeignChainView() {
    return ce::overlay_compat::CountLoadedTrackedOverlayModules(
               ce::overlay_compat::TrackedOverlaySubset::kOverlay) >= 2;
}

bool ShouldPreserveDX12SwapchainIdentityForForeignChain(IUnknown* pDevice) {
    return ce::overlay_compat::ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(
        IsStreamlineRuntimeSwapchainWrappable(pDevice), DXGIShared::ArePresentMethodsInterceptedBelowForeignChain(),
        ce::overlay_compat::CountLoadedTrackedOverlayModules(
            ce::overlay_compat::TrackedOverlaySubset::kOverlay));
}
