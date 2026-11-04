#pragma once

#include <optional>

#include "dx11_present_perf_row.h"
#include "dxgi_shared_internal.h"

namespace DXGIShared {

// How a Present ends once its route forwarded it and flushed any deferred Signal: the result reports that focus and
// occlusion tracking and the device-loss latch run on, and the limiter's post-present half. `presentName` is "Present"
// or "Present1". Routes that returned without it fed neither report (`present_result` n=0, 20261104_183540).
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
    std::optional<DX11PresentPerfRow> row;
    row.emplace(PerfLogger::Get().IsEnabled(), "DX12", route);
    auto rowGuard = ce::make_scope_guard([&row] {
        CostScope perfRowStage(CostStage::kPerfRow);
        row.reset();
    });

    const int64_t drawStartUs = row->NowUs();
    InvokePostSLCallbackForFinalOutputPresent(postSLCallback, pSwapChain);
    row->NoteOverlaySince(drawStartUs);

    RefreshLivePresentHooksForSwapchainIfNeeded(pSwapChain, route);

    const int64_t limiterStartUs = row->NowUs();
    ApplyFpsLimiterBeforeBypassedFinalOutputPresent(pSwapChain, wrapperOwnsPresent, callbackProvenRuntimeOutput, route);
    row->NoteFpsLimitWaitSince(limiterStartUs);

    // This is Streamline's physical output, not the app's proxy Present. Forced FIFO belongs here: applying sync=1 to
    // the proxy can stall DLSS-G's pacer, while omitting it here lets the recovered post-FSR output exceed refresh
    // with tearing.
    {
        CostScope limiterStage(CostStage::kLimiter);
        ProcessPresentVSyncOverride(SyncInterval, Flags, pSwapChain);
    }

    row->BeginPresentCall();
    const HRESULT hr = forward(SyncInterval, Flags);
    row->EndPresentCall();

    ce::present_stage_cost::EnterStage(CostStage::kPostPresent);
    return FinishForwardedPresent(pSwapChain, SyncInterval, Flags, APIType::D3D12, hr, presentName);
}

}  // namespace DXGIShared
