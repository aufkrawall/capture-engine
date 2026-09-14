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

// NVIDIA Smooth Motion (NvPresent64) creates this chain in ADDITION to the application's: it is
// the interposer's private output chain, presented on the interposer's own command queue while the
// application keeps a proxy object. CE must never composite into it. Strange Brigade DX12 + Smooth
// Motion (session 20260914_102700) is what this is for: CE's deep dxgi!Present body hook handed it
// exactly this swapchain, CE adopted it as the game's, submitted the overlay on the GAME's queue,
// and the first ExecuteCommandLists removed the device with DXGI_ERROR_ACCESS_DENIED.
bool NotePresentInterposerPrivateSwapchainCreate(const char* context, const void* callerAddress,
                                                 IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return false;
    }
    char callerModulePath[MAX_PATH] = {};
    const bool callerFromPresentInterposerModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromPresentInterposerModule(
                             callerAddress, callerModulePath, sizeof(callerModulePath));
    char stackModulePath[MAX_PATH] = {};
    const bool presentInterposerInStack =
        !callerFromPresentInterposerModule &&
        ce::overlay_compat::HasPresentInterposerModuleInStack(stackModulePath, sizeof(stackModulePath));
    if (!ce::overlay_compat::ShouldTreatCreatedSwapchainAsPresentInterposerPrivateChain(
            callerFromPresentInterposerModule, presentInterposerInStack)) {
        return false;
    }

    DXGIShared::DX12_RegisterPresentInterposerPrivateSwapchain(pSwapChain);
    static std::atomic<int> s_privateChainLogCount{0};
    const int logNum = s_privateChainLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 10 || (logNum % 500) == 0) {
        const char* creator = callerFromPresentInterposerModule ? callerModulePath : stackModulePath;
        HookLogImportant(
            "%s: Present interposer %s created its private output swapchain %p (#%d) — CE never composites into "
            "it; the overlay stays on the application-facing chain",
            context ? context : "CreateSwapChain", creator[0] ? creator : "unknown", (void*)pSwapChain, logNum);
    }
    return true;
}

bool ShouldPreserveDX12SwapchainIdentityForForeignChain(IUnknown* pDevice, IDXGISwapChain* pSwapChain) {
    return ce::overlay_compat::ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(
        IsStreamlineRuntimeSwapchainWrappable(pDevice), DXGIShared::ArePresentMethodsInterceptedBelowForeignChain(),
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay),
        DXGIShared::IsSwapchainPresentCoveredByDeepBodyHook(pSwapChain));
}
