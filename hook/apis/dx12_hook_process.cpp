#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

void DX12_ResetImGuiFrameCounter() {
    dx12_hook_s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    dx12_hook_s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}
void DX12_ResetOverlayFrameDelay() {
    dx12_hook_s_framesSinceInit = 0;
    dx12_hook_s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}
// Minimal-overhead ProcessFrame for no-callback FSR FG: skips the ~400 lines of policy/lock/heuristic
// work in DX12_ProcessFrameExternal (two g_CommandQueueMutex acquisitions, FSR heuristic checks, stale
// cleanup with AddRef/Release, PostSL processing) that take ~27.5ms and desync AMD's QPC-timed pacing.
// Does only: frame-count reset, RecordFrame, sc3 acquire, capture decision, inner ProcessFrame (which
// has the overlay-skip gate — no overlay draw during no-callback FSR FG). Capture still works.
void DX12_ProcessFrameMinimal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                              bool frameGenerationPresentationActive) {
    if (HookIsShuttingDown() || !pSwapChain) {
        return;
    }
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return;
    }
    const int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
    ++dx12_hook_g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    const bool isInterpolatedFrame = (count == 0);
    bool processCapture = !isInterpolatedFrame;
    if (processCapture && ShouldSkipCaptureForTargetCadence()) {
        processCapture = false;
    }
    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotWantsOverlay =
        screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
    if (screenshotRequested && !screenshotWantsOverlay) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive);
    if (screenshotWantsOverlay && !ShouldUseConfirmedPostSLForOverlayIncludedWork(screenshotOverlayCfg)) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    sc3->Release();
}
static void DX12_ProcessFrameExternalForPresent(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                                                bool frameGenerationPresentationActive) {
    ce::dx12_process_frame_diagnostics::StageTimings timings;
    const auto wrapperActivityBefore = GetWrapperHookActivitySnapshot();
    DX12_ProcessFrameExternal(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive, &timings);
    if (timings.totalUs >= 5000) {
        const auto wrapperActivityAfter = GetWrapperHookActivitySnapshot();
        static std::atomic<int> s_slowProcessFrameLogCount{0};
        const int logCount = s_slowProcessFrameLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 200 || (logCount % 50) == 0) {
            const auto breakdown = ce::dx12_process_frame_diagnostics::ComputeBreakdown(timings);
            const bool wrapperActivityOverlap = ce::dx12_process_frame_diagnostics::DidActivityOverlap(
                wrapperActivityBefore, wrapperActivityAfter);
            HookLogImportant(
                "DX12 DIAG: ProcessFrame (overlay) SLOW %.1fms breakdown external=%.3fms inner=%.3fms "
                "innerOther=%.3fms capture=%.3fms overlay=%.3fms "
                "[valid=%d acquire=%.3fms record=%.3fms submit=%.3fms post=%.3fms] screenshot=%.3fms "
                "queueLockWait=%.3fms wrapperInitOverlap=%d wrapperActivity=%llu/%u->%llu/%u "
                "innerCalled=%d reentrantSkip=%d tid=0x%04X",
                static_cast<double>(timings.totalUs) / 1000.0,
                static_cast<double>(breakdown.externalUs) / 1000.0,
                static_cast<double>(timings.innerUs) / 1000.0,
                static_cast<double>(breakdown.innerOtherUs) / 1000.0,
                static_cast<double>(timings.captureUs) / 1000.0,
                static_cast<double>(breakdown.overlayUs) / 1000.0, timings.overlayBreakdownValid ? 1 : 0,
                static_cast<double>(timings.overlayAcquireUs) / 1000.0,
                static_cast<double>(timings.overlayRecordUs) / 1000.0,
                static_cast<double>(timings.overlaySubmitUs) / 1000.0,
                static_cast<double>(timings.overlayPostSubmitUs) / 1000.0,
                static_cast<double>(timings.screenshotUs) / 1000.0,
                static_cast<double>(timings.commandQueueLockWaitUs) / 1000.0, wrapperActivityOverlap ? 1 : 0,
                static_cast<unsigned long long>(wrapperActivityBefore.generation), wrapperActivityBefore.activeCalls,
                static_cast<unsigned long long>(wrapperActivityAfter.generation), wrapperActivityAfter.activeCalls,
                timings.innerCalled ? 1 : 0, timings.reentrantInnerSkipped ? 1 : 0, GetCurrentThreadId());
        }
    }
}
void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    const bool frameGenerationPresentationActive =
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
        DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ||
        HookHasRuntimeOwnedNativeFGPresentPath();
    const bool applicationSourcePresent = ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
        frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), GetCurrentThreadId());
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}
namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                            bool frameGenerationPresentationActive) {
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}
}
namespace DXGIShared {
void HandleDX12ResizeBegin() {
    HookLog("DX12: HandleDX12ResizeBegin CALLED from DetourResizeBuffers");
    DX12_OnSwapchainResizeBegin();
}
}
namespace DXGIShared {
void HandleDX12ResizeEnd() {
    HookLog("DX12: HandleDX12ResizeEnd CALLED");
    DX12_OnSwapchainResizeEnd();
}
}
bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}
void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}
// FIXED: Clean up the global hook instance if allocated
// Service the deferred ECL probe: if ProbeRealD3D12ECL was skipped because
// the Streamline startup window was active, try to probe now that the window
// has expired.  Safe to call from any thread at any time.
void DX12_ServiceDeferredECLProbe() {
    if (!dx12_hook_g_ProbeRealD3D12ECLDeferred.load(std::memory_order_acquire)) {
        return;
    }
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return;
    }
    auto* srvDev = g_Device.load(std::memory_order_acquire);
    if (srvDev && IsStreamlineLoaded()) {
        ProbeRealD3D12ECL(srvDev);
        auto* srvProbed = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        dx12_hook_g_ProbeRealD3D12ECLDeferred.store(false, std::memory_order_release);
        HookLogImportant("DX12: ServiceDeferredECLProbe — realECL=%p", srvProbed);
    }
}
DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}
