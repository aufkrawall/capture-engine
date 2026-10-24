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

// The non-retaining wrapper borrows the runtime's CreateSwapChain reference and adds no refs of its
// own, so Streamline's release/recreate on an FG transition stays byte-identical to a process without
// CE; a retaining wrapper would pin the old swapchain and break the DLSS-G handoff with E_ACCESSDENIED
// on the same HWND. ChooseStreamlineRuntimeSwapchainView decides whether CE needs it at all.
namespace {
// TSwapChain picks the wrapper constructor the create path always used (IDXGISwapChain1 for ForHwnd).
template <typename TSwapChain>
CWrapDXGISwapChain* TakeStreamlineRuntimeSwapchainViewFor(const char* context, IUnknown* pDevice,
                                                          TSwapChain* pCreated, HWND hwnd) {
    using ce::overlay_compat::StreamlineRuntimeSwapchainView;
    IDXGISwapChain* const pSwapChain = static_cast<IDXGISwapChain*>(pCreated);
    const size_t loadedOverlayCount =
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay);
    const StreamlineRuntimeSwapchainView view = ce::overlay_compat::ChooseStreamlineRuntimeSwapchainView(
        IsStreamlineRuntimeSwapchainWrappable(pDevice), DXGIShared::ArePresentMethodsInterceptedBelowForeignChain(),
        loadedOverlayCount, DXGIShared::IsSwapchainPresentCoveredByDeepBodyHook(pSwapChain));
    const char* caller = context ? context : "CreateSwapChain";
    static std::atomic<int> s_viewLogCount{0};
    const int logNum = s_viewLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool logView = logNum <= 20 || (logNum % 500) == 0;

    if (view == StreamlineRuntimeSwapchainView::kRealBelowForeignChain) {
        // The application side resizes this exact object, so CE's resize reconciliation must reach it.
        DXGIShared::InstallResizeReconciliationHooks(pSwapChain, "Streamline runtime preserved identity");
        if (logView) {
            HookLogImportant(
                "%s: Streamline runtime swapchain %p returned as created (view=%s hwnd=%p overlays=%zu #%d) — the deep "
                "Present hook below the foreign chain sees its presents; a CE proxy would be what the overlays above "
                "this create hook",
                caller, pSwapChain, ce::overlay_compat::StreamlineRuntimeSwapchainViewName(view), hwnd,
                loadedOverlayCount, logNum);
        }
        return nullptr;
    }
    if (view == StreamlineRuntimeSwapchainView::kReal) {
        HookLog("%s: Streamline present, skipping wrap (sc=%p view=%s loadedOverlays=%zu)", caller, pSwapChain,
                ce::overlay_compat::StreamlineRuntimeSwapchainViewName(view), loadedOverlayCount);
        return nullptr;
    }

    void* pExistingWrapper = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
        ((IUnknown*)pExistingWrapper)->Release();
        HookLog("%s: Streamline swapchain already wrapped, skipping double-wrap", caller);
        return nullptr;
    }
    auto* wrapper = new CWrapDXGISwapChain(pCreated, pDevice, /*streamlineRuntimeNonRetaining=*/true);
    if (logView) {
        HookLogImportant("%s: Wrapped Streamline runtime swapchain (real=%p wrapper=%p hwnd=%p view=%s overlays=%zu #%d)",
                         caller, pSwapChain, wrapper, hwnd, ce::overlay_compat::StreamlineRuntimeSwapchainViewName(view),
                         loadedOverlayCount, logNum);
    }
    DXGIShared::MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain(pSwapChain, caller);
    return wrapper;
}
}  // namespace

CWrapDXGISwapChain* TakeStreamlineRuntimeSwapchainView(const char* context, IUnknown* pDevice,
                                                       IDXGISwapChain* pSwapChain, HWND hwnd) {
    return TakeStreamlineRuntimeSwapchainViewFor(context, pDevice, pSwapChain, hwnd);
}

CWrapDXGISwapChain* TakeStreamlineRuntimeSwapchainView(const char* context, IUnknown* pDevice,
                                                       IDXGISwapChain1* pSwapChain, HWND hwnd) {
    return TakeStreamlineRuntimeSwapchainViewFor(context, pDevice, pSwapChain, hwnd);
}

// NVIDIA Smooth Motion (NvPresent64) creates this chain in ADDITION to the application's: it is the
// interposer's private output chain, presented on the interposer's own command queue while the
// application keeps a proxy object. It is where CE's overlay belongs — below every overlay that
// patches the dxgi entry, and after the driver generated the frame — but ONLY on that queue.
// Strange Brigade DX12 + Smooth Motion (session 20260914_102700) is what this is for: CE adopted the
// chain and submitted the overlay on the GAME's queue, and the first ExecuteCommandLists removed the
// device with DXGI_ERROR_ACCESS_DENIED (0x887A002B) — the cross-queue backbuffer access
// dx12_overlay_policy/ffx_routing.h already documents by that exact HRESULT.
//
// `pDevice` is the D3D12 command queue for a DX12 swapchain create. Recording it here is what makes
// the chain compositable at all; without it CE keeps the overlay on the application-facing chain.
bool NotePresentInterposerPrivateSwapchainCreate(const char* context, const void* callerAddress,
                                                 IDXGISwapChain* pSwapChain, IUnknown* pDevice) {
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

    ID3D12CommandQueue* outputQueue = nullptr;
    if (pDevice) {
        // A DX12 swapchain's "device" is its command queue; anything else (a D3D11 device) leaves
        // the chain non-compositable, which is the safe state.
        if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&outputQueue)))) {
            outputQueue = nullptr;
        }
        if (outputQueue) {
            // The map holds a raw pointer for identity only. The interposer owns this queue for the
            // lifetime of its chain, and a retaining reference would pin a driver object CE has no
            // business keeping alive.
            outputQueue->Release();
        }
    }
    DXGIShared::DX12_RegisterPresentInterposerPrivateSwapchain(pSwapChain, outputQueue);
    static std::atomic<int> s_privateChainLogCount{0};
    const int logNum = s_privateChainLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 10 || (logNum % 500) == 0) {
        const char* creator = callerFromPresentInterposerModule ? callerModulePath : stackModulePath;
        HookLogImportant(
            "%s: Present interposer %s created its output swapchain %p on queue %p (#%d) — CE composites there, "
            "on that queue only%s",
            context ? context : "CreateSwapChain", creator[0] ? creator : "unknown", (void*)pSwapChain,
            (void*)outputQueue, logNum,
            outputQueue ? "" : "; no queue observed, so the overlay stays on the application-facing chain");
    }
    return true;
}

bool ShouldPreserveDX12SwapchainIdentityForForeignChain(IUnknown* pDevice, IDXGISwapChain* pSwapChain) {
    return ce::overlay_compat::ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(
        IsStreamlineRuntimeSwapchainWrappable(pDevice), DXGIShared::ArePresentMethodsInterceptedBelowForeignChain(),
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay),
        DXGIShared::IsSwapchainPresentCoveredByDeepBodyHook(pSwapChain));
}
