#include "dx12_hook_internal.h"

#include "dx12_hook_ffx_shared.h"
#include "../common/hook_cost_window.h"

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

namespace {
void ReportFFXCost(const char* site, const ce::HookCostSnapshot& cost) {
    HookLogImportant(
        "[OVERLAY COST] %s: windowCalls=%llu ceAvgUs=%llu ceMaxUs=%llu "
        "runtimeAvgUs=%llu runtimeMaxUs=%llu ceOver500Us=%llu ceOver1ms=%llu "
        "(per-thread window; forwarded runtime time excluded)",
        site, static_cast<unsigned long long>(cost.calls),
        static_cast<unsigned long long>(cost.selfUs / cost.calls),
        static_cast<unsigned long long>(cost.selfMaxUs),
        static_cast<unsigned long long>(cost.wrappedUs / cost.calls),
        static_cast<unsigned long long>(cost.wrappedMaxUs),
        static_cast<unsigned long long>(cost.over500Us), static_cast<unsigned long long>(cost.over1ms));
}
}  // namespace

void DX12_ObserveFFXPresentCallbackCost(int64_t totalUs, int64_t wrappedUs) {
    static thread_local ce::HookCostWindow<> window;
    const auto sample = window.Observe(totalUs, wrappedUs);
    if (sample.has_value())
        ReportFFXCost("FFX present-callback bridge", sample.value());
}

void DX12_ObserveFFXProxyPresentCost(int64_t enterUs, int64_t forwardUs, int64_t returnUs) {
    static thread_local ce::HookCostWindow<> window;
    const auto sample = window.Observe(returnUs - enterUs, returnUs - forwardUs);
    if (sample.has_value())
        ReportFFXCost("FFX proxy Present on the game thread", sample.value());
}
