#include "dx12_hook_internal.h"

#include "dx12_hook_ffx_shared.h"

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

FFXHookCostAccumulator g_FFXPresentCallbackCost;

// Once per 600 displayed frames: at 2x frame generation that is a summary every
// few seconds, which is enough to see a regression and cheap enough to leave on.
void DX12_ReportFFXPresentCallbackCostIfDue() {
    const uint64_t calls = g_FFXPresentCallbackCost.calls.load(std::memory_order_relaxed);
    if (calls == 0 || (calls % 600) != 0) {
        return;
    }
    const uint64_t selfUs = g_FFXPresentCallbackCost.selfUs.load(std::memory_order_relaxed);
    const uint64_t wrappedUs = g_FFXPresentCallbackCost.wrappedUs.load(std::memory_order_relaxed);
    HookLogImportant(
        "[OVERLAY COST] FFX present-callback bridge: calls=%llu ceAvgUs=%llu ceMaxUs=%llu "
        "runtimeAvgUs=%llu runtimeMaxUs=%llu — CE's own share is the part a frame-rate A/B cannot separate",
        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(selfUs / calls),
        static_cast<unsigned long long>(g_FFXPresentCallbackCost.selfMaxUs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(wrappedUs / calls),
        static_cast<unsigned long long>(g_FFXPresentCallbackCost.wrappedMaxUs.load(std::memory_order_relaxed)));
}

FFXHookCostAccumulator g_FFXProxyPresentCost;

// The game thread's own view of a frame-generation Present: CE's prework, then
// the runtime's Present, which is where the runtime blocks the game to pace its
// generated output. Splitting the two says whether a cost belongs to CE or to
// the pacing the game would have paid anyway.
void DX12_ObserveFFXProxyPresentCost(int64_t enterUs, int64_t forwardUs, int64_t returnUs) {
    g_FFXProxyPresentCost.Observe(returnUs - enterUs, returnUs - forwardUs);
    const uint64_t calls = g_FFXProxyPresentCost.calls.load(std::memory_order_relaxed);
    if (calls == 0 || (calls % 600) != 0) {
        return;
    }
    HookLogImportant(
        "[OVERLAY COST] FFX proxy Present on the game thread: calls=%llu ceAvgUs=%llu ceMaxUs=%llu "
        "runtimePresentAvgUs=%llu runtimePresentMaxUs=%llu",
        static_cast<unsigned long long>(calls),
        static_cast<unsigned long long>(g_FFXProxyPresentCost.selfUs.load(std::memory_order_relaxed) / calls),
        static_cast<unsigned long long>(g_FFXProxyPresentCost.selfMaxUs.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_FFXProxyPresentCost.wrappedUs.load(std::memory_order_relaxed) / calls),
        static_cast<unsigned long long>(g_FFXProxyPresentCost.wrappedMaxUs.load(std::memory_order_relaxed)));
}
