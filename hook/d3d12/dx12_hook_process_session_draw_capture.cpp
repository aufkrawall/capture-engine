#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::PublishPostOverlayCapture() {
    // The overlay-free capture sits inside the overlay draw chain. When that chain
    // did not run (overlay backend not initialized, e.g. on a runtime-owned FSR FG
    // chain, or its draw was skipped) the frame is still recorded here.
    const bool overlayFreeCaptureMissedByDraw = captureBeforeOverlay && !captureBeforeOverlayPublished;
    if (overlayFreeCaptureMissedByDraw) {
        static std::atomic<uint64_t> s_overlayFreeFallbackCount{0};
        const uint64_t fallbackCount = s_overlayFreeFallbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallbackCount <= 5 || (fallbackCount % 3000) == 0) {
            HookLogImportant(
                "DX12: Overlay-free capture published after the overlay draw chain did not reach it "
                "(overlayInit=%d syncInit=%d independentFSRTopmost=%d runtimeOwns=%d count=%llu)",
                dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0,
                independentFSRTopmostCompositedThisPresent ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                static_cast<unsigned long long>(fallbackCount));
        }
    }
    if (captureAfterOverlay || overlayFreeCaptureMissedByDraw) {

        int64_t captureStartUs = PerfLogger::GetQpcUs();
        PublishDX12CapturedFrame(pSwapChain, captureShm, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx);
        const int64_t captureUs = PerfLogger::GetQpcUs() - captureStartUs;
        perfMetrics.captureUs = static_cast<int32_t>(captureUs);
        if (diagnostics) {
            diagnostics->captureUs += captureUs;
        }
    }
    return ProcessFrameFlow::kContinue;
}
