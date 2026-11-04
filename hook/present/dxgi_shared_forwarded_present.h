#pragma once

#include "dxgi_shared_internal.h"
#include "forwarded_present_record.h"

namespace DXGIShared {

// The reports a forwarded Present's result feeds: focus and occlusion tracking, and the device-loss latch. `presentName`
// is "Present" or "Present1". Routes that returned without them fed neither (`present_result` n=0, 20261104_183540).
HRESULT ReportForwardedPresentResult(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, APIType api, HRESULT hr,
                                     const char* presentName);

// How a Present ends once its route ran the limiter, forwarded it and flushed any deferred Signal: the reports, and the
// limiter's post-present half.
HRESULT FinishForwardedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, APIType api, HRESULT hr,
                               const char* presentName);

// Streamline's final output after an FSR FG -> DLSS FG switch with the Steam overlay loaded, forwarded past Steam's
// stale chain. It carries every output from the switch on, and had none of the normal route's bookkeeping: no CSV row
// for all of DLSS FG, no result reports, and its costs read as `startup_routing` (20261104_210842). `forward` is the
// Present or Present1 down-call, given the interval and flags the vsync override left. Flow scenarios cannot reach this
// route, which needs the Steam overlay.
template <typename Forward>
HRESULT ExecuteStandaloneOutputBypass(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                      PostSLOverlayRenderFn postSLCallback, bool wrapperOwnsPresent,
                                      bool callbackProvenRuntimeOutput, const char* route, const char* presentName,
                                      Forward&& forward) {
    using CostStage = ce::present_stage_cost::Stage;
    using CostScope = ce::present_stage_cost::StageScope;
    // Opened before the draw, so total_us covers it. PostSL logs no row of its own, so this one is the frame's.
    ForwardedPresentRecord record(true, route);
    const int64_t drawStartUs = record.NowUs();
    InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
    record.NoteOverlaySince(drawStartUs);

    RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, route);

    record.TimeLimiter([&] {
        ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain, wrapperOwnsPresent, callbackProvenRuntimeOutput,
                                                        route);
    });

    // This is Streamline's physical output, not the app's proxy Present. Forced FIFO belongs here: applying sync=1 to
    // the proxy can stall DLSS-G's pacer, while omitting it here lets the recovered post-FSR output exceed refresh
    // with tearing.
    {
        CostScope limiterStage(CostStage::kLimiter);
        ProcessPresentVSyncOverride(SyncInterval, Flags, pSwapChain);
    }

    const HRESULT hr = record.Forward([&] { return forward(SyncInterval, Flags); });

    ce::present_stage_cost::EnterStage(CostStage::kPostPresent);
    return FinishForwardedPresent(pSwapChain, SyncInterval, Flags, APIType::D3D12, hr, presentName);
}

}  // namespace DXGIShared
