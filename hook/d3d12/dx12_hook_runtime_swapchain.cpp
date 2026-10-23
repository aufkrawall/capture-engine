#include "dx12_runtime_swapchain.h"

#include "dx12_hook_internal.h"
#include "hook/ffx/ffx_hook.h"
#include "hook/fg/fg_presenter.h"
#include "hook/streamline/streamline_hook.h"
#include "hook/present/present_callback_association.h"
#include "hook/xefg/xefg_hook.h"

bool DX12_NoteFrameGenerationRuntimeSwapchainCreate(const char* context, IDXGISwapChain* swapchain, IUnknown* device,
                                                    HWND window, bool xefgModuleOnCreateStack,
                                                    const char* ffxModulePath) {
    if (!swapchain) {
        return false;
    }
    // A D3D12 swapchain's create "device" is the queue it presents on until a ResizeBuffers1 rebinds it. Identity
    // only: the runtime owns its queue for the lifetime of its chain.
    ID3D12CommandQueue* queue = nullptr;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))) && queue) {
        queue->Release();
    } else {
        queue = nullptr;
    }
    const bool ffxRuntimeCreator = ffxModulePath && ffxModulePath[0];
    const auto presenter = ce::fg_presenter::NoteSwapchainCreated(swapchain, window, queue, xefgModuleOnCreateStack,
                                                                  context, ffxRuntimeCreator);
    if (presenter == ce::fg_presenter::Presenter::kFFXSwapchain) {
        FFXHook::NoteFrameGenerationSwapchainCreator(ffxModulePath);
    }
    if (!ce::fg_presenter::IsRuntimeOwnedOutputPresenter(presenter)) {
        return false;
    }
    XeFGHook::NotePresenterSwapchainRegistered(swapchain);
    // A Streamline FG signal the game sent before XeFG created its chain was a front all along.
    StreamlineHook::ReconcileFrontSignal("frame-generation runtime presenter registered");
    return true;
}

namespace DXGIShared {
// One output of a runtime-owned output swapchain (dxgi_shared_present_runtime_output.cpp). The lean form of
// DX12_ProcessFrameExternal: none of the FSR/Streamline heuristics, which would read XeFG's zero-submission
// generated outputs as FSR's; every output is a displayed frame, so every output is drawn, judged on its own
// (no FG-composed inheritance) and captured. ProcessFrame draws on the presenter's output queue (stage 2).
void HandleDX12RuntimeOwnedOutputFrame(IDXGISwapChain* pSwapChain) {
    if (HookIsShuttingDown() || !pSwapChain) {
        return;
    }
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }
    if (!WasD3D12DeviceCreated()) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&device))) && device) {
            MarkD3D12DeviceCreated();
            device->Release();
        }
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return;
    }
    // Consumed here as on every other route, so no later Present inherits this one's submissions or verdict.
    const int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
    DX12_ObserveFrameClassificationTraffic(count);
    (void)ce::present_association::ConsumePresentFrameVerdict();
    ++dx12_hook_g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    auto overlayCoverageGuard = ce::make_scope_guard(
        [pSwapChain]() { AccountPresentForOverlayCoverage(false, "RuntimeOwnedOutput", pSwapChain); });
    const bool processCapture = !DX12_ShouldUseStreamlineFinalOutputCapture();
    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
    ProcessFrame(sc3, processCapture, /*applicationSourcePresent=*/false, /*frameGenerationPresentationActive=*/true);
    if (screenshotRequestId != 0 && GetPendingScreenshotRequestId(screenshotShm) == screenshotRequestId) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    sc3->Release();
}
}  // namespace DXGIShared

void DX12_ReleaseSwapchainForRuntimeTakeover(IDXGISwapChain* swapchain, const char* reason) {
    if (!swapchain) {
        return;
    }
    char captureRelease[128] = {};
    const bool captureReleased = DX12_ReleaseCaptureForSwapChainResize(swapchain, captureRelease, sizeof(captureRelease));
    DX12_OnSwapchainResizeBegin();
    ForgetSwapchainFromTracking(swapchain);
    DX12_OnSwapchainResizeEnd();
    HookLogImportant(
        "DX12: Released CE's references to swapchain %p before a runtime takes it over (%s): overlay render targets "
        "and sync flushed, tracking dropped, capture %s",
        swapchain, reason ? reason : "runtime takeover", captureReleased ? captureRelease : "unbound");
}
