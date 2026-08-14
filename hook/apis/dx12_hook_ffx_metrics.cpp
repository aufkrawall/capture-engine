#include "dx12_hook_internal.h"

void DX12_LogRuntimeOwnedCallbackHDRSourceChange(DXGI_FORMAT format, int colorSpace,
                                                 bool presentationContractSupported, bool isHDR) {
    const uint64_t signature = static_cast<uint32_t>(format) |
                               (static_cast<uint64_t>(static_cast<uint32_t>(colorSpace + 1)) << 32) |
                               (static_cast<uint64_t>(presentationContractSupported ? 1 : 0) << 62) |
                               (static_cast<uint64_t>(isHDR ? 1 : 0) << 63);
    static std::atomic<uint64_t> s_lastSignature{UINT64_MAX};
    if (s_lastSignature.exchange(signature, std::memory_order_acq_rel) == signature) {
        return;
    }
    HookLogImportant(
        "DX12: Runtime-owned callback HDR source changed "
        "(format=%d colorSpace=%d supported=%d isHDR=%d) — stable per-frame configurations are silent",
        static_cast<int>(format), colorSpace, presentationContractSupported ? 1 : 0, isHDR ? 1 : 0);
}

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
