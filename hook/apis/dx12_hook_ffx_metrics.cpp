#include "dx12_hook_internal.h"

void DX12_UpdateFFXPresentCallbackFrameTiming(PerformanceMetrics* metrics,
                                              bool runtimeOwnsNativeFSRPresentation,
                                              bool callbackYieldsToTopmostRoute) {
    if (!metrics) {
        return;
    }

    const bool presentInterceptedBelowForeignChain = DXGIShared::IsPresentInterceptedBelowForeignChain();
    const bool callbackSamplesFrameTiming =
        ce::dx12_overlay_policy::ShouldSampleFrameTimingFromFFXPresentCallback(
            runtimeOwnsNativeFSRPresentation, callbackYieldsToTopmostRoute,
            presentInterceptedBelowForeignChain);
    if (runtimeOwnsNativeFSRPresentation && !callbackYieldsToTopmostRoute) {
        DXGIShared::NoteOverlayCompositeSite(DXGIShared::kFGRuntimeUiCompositeSite,
                                             "DX12_RenderOverlayViaFFXPresentCallback");
    }
    if (callbackSamplesFrameTiming) {
        // Runtime-only fallback: use the callback only when the displayed output does not re-enter CE's
        // deep DXGI Present observer. Otherwise Present is the single frame-time/FPS writer.
        metrics->Update(PerfLogger::GetQpcUs());
    }

    static std::atomic<int> s_callbackFrameTimingOwner{-1};
    const int timingOwner = callbackSamplesFrameTiming ? 1 : 0;
    if (runtimeOwnsNativeFSRPresentation &&
        s_callbackFrameTimingOwner.exchange(timingOwner, std::memory_order_acq_rel) != timingOwner) {
        HookLogImportant(
            "[OVERLAY FPS] FFX callback %s frame-time sampling "
            "(callbackYield=%d deepPresentObserver=%d) — exactly one displayed-output observer advances FPS/history",
            callbackSamplesFrameTiming ? "OWNS" : "YIELDS",
            callbackYieldsToTopmostRoute ? 1 : 0, presentInterceptedBelowForeignChain ? 1 : 0);
    }
}
