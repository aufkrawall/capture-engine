#include "dxgi_shared_internal.h"
#include "hook/fg/fg_presenter.h"
#include "hook/runtime/hook_clock.h"
#include "common/logging/log_meter.h"

#include <mutex>

// The Present of a swapchain a frame-generation runtime created and presents itself (Intel XeSS frame generation,
// ce::fg_presenter): every displayed frame, generated and real, from the runtime's presenter thread or from the
// game thread inside the runtime's proxy Present. CE composites on each of them, on the queue the runtime presents
// on, after the runtime generated the frame - so the overlay is on every displayed frame, never interpolated, and
// topmost below a foreign Present chain.
//
// Creator attribution decides, not the stack: OptiScaler's XeFG presents the real frame from inside Streamline's
// proxy on the game thread, so that Present looked Streamline-originated and took the PostSL route while the
// XeFG-thread Present looked like the application's and ProcessFrame skipped it (20261023_183119: the overlay on 72
// of 144 displayed frames). None of the Streamline/FFX routing applies here.
//
// Two threads present this swapchain. The back buffer a draw targets is the one the next Present shows, so each
// output's composite and its Present are one critical section: a second thread drawing between them would draw on
// the image the first one is about to show. DXGI already serializes Presents on one swapchain; CE adds only its own
// draw to that order, and never a CPU or GPU wait on the overlay - the runtime's presenter is pacing-critical.

namespace DXGIShared {
namespace {

std::mutex& RuntimeOutputMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace

bool IsRuntimeOwnedOutputPresent(IDXGISwapChain* pSwapChain) {
    return pSwapChain && ce::fg_presenter::HasRuntimeOwnedSwapchains() &&
           ce::fg_presenter::IsRuntimeOwnedOutputPresenter(ce::fg_presenter::PresenterFor(pSwapChain));
}

HRESULT ExecuteRuntimeOwnedOutputPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                         const DXGI_PRESENT_PARAMETERS* pPresentParameters, bool present1) {
    using CostStage = ce::present_stage_cost::Stage;
    using CostScope = ce::present_stage_cost::StageScope;
    const char* presentName = present1 ? "Present1" : "Present";
    {
        static std::atomic<uint32_t> s_outputs{0};
        static ce::log_meter::StreamChangeGate<16> s_threads;
        const uint32_t outputs = s_outputs.fetch_add(1, std::memory_order_relaxed) + 1;
        const DWORD threadId = GetCurrentThreadId();
        const auto verdict = s_threads.ObserveOrEveryPerStream(ce::log_meter::FieldKey(pSwapChain, threadId),
                                                               ce::log_meter::FieldKey(present1), 4096);
        if (verdict) {
            HookLogImportant(
                "[RUNTIME OUTPUT] %s on %s swapchain %p from tid=0x%04lX composites on queue %p (outputs=%u)%s",
                presentName, ce::fg_presenter::PresenterName(ce::fg_presenter::PresenterFor(pSwapChain)), pSwapChain,
                static_cast<unsigned long>(threadId), ce::fg_presenter::OutputQueue(pSwapChain), outputs,
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    ce::fg_presenter::NoteRuntimeOutputPresent();

    std::lock_guard<std::mutex> outputLock(RuntimeOutputMutex());
    bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
    auto hookGuard = ::ce::make_scope_guard([&] {
        if (isFirstHook)
            g_SharedState.inPresentHook.store(false);
    });
    {
        CostScope metricsStage(CostStage::kMetrics);
        g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);
        // The application frame rate advances at the runtime's input boundary (one per game frame), not once per
        // output; without that boundary every output counts, as on any swapchain.
        PublishDXGIPresentMetrics(isFirstHook, !ce::fg_presenter::HasRuntimeInputBoundary(),
                                  "DXGIShared::RuntimeOwnedOutputPresent");
    }
    ce::present_release_forecast::ScopedPreLimiterCapture preLimiterCapture;
    if (!IsShuttingDown()) {
        NoteOverlayCompositeSite(IsPresentInterceptedBelowForeignChain() ? OverlayCompositeSite::kBelowForeignChain
                                                                         : OverlayCompositeSite::kPresentEntry,
                                 "RuntimeOwnedOutputPresent");
        CostScope overlayStage(CostStage::kOverlay);
        HandleDX12RuntimeOwnedOutputFrame(pSwapChain);
    }
    {
        // Every output passes the limiter as the runtime output it is: the application's frames are paced at the
        // runtime's input boundary, and this thread is the runtime's presenter (PresentSite::kRuntimeOutputPresent).
        CostScope limiterStage(CostStage::kLimiter);
        ApplyFpsLimiterStage(pSwapChain, true, ce::fps_limiter_policy::PresentSite::kRuntimeOutputPresent);
        preLimiterCapture.Close();
        ProcessPresentVSyncOverride(SyncInterval, Flags, pSwapChain);
    }
    const HRESULT hr = present1 ? CallOriginalPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters)
                                : CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    ce::present_stage_cost::EnterStage(CostStage::kPostPresent);
    // The overlay's fence Signal follows the Present on the queue the overlay was submitted to.
    InvokeDX12FlushDeferredSignal();
    NoteDX12PresentResultForVtablePath(pSwapChain, presentName, SyncInterval, Flags, hr);
    if (SUCCEEDED(hr)) {
        g_SharedFpsLimiter.ApplyPostPresent();
    }
    NotePresentDeviceResult(hr, present1 ? "runtime-owned output Present1" : "runtime-owned output Present");
    return hr;
}

}  // namespace DXGIShared
