#pragma once

#include <optional>

#include "dx11_present_perf_row.h"
#include "present_stage_cost.h"
#include "present_stall_attribution.h"

namespace DXGIShared {

// What a route records when it forwards a D3D12 Present itself, past ExecutePresentCore: the Present's CSV row and the
// down-call's [PRESENT STALL] accounting. Every such route lost one or both: XeFG's rows had no present columns, the
// post-FSR bypass had no row until 0.1.7418 and no stall accounting, and the 9 presents before it took over had neither
// (20261104_210842, 20261104_222523).
//
// `forwards` false makes the record inert, for a route that opens it before it knows it will forward. A row an outer
// hook on this thread already holds stays that hook's: a Present never writes two rows. `holdsInnerRow`: the route's
// frame work logs a ProcessFrame row, which this one holds and completes (DX11PresentPerfRow).
class ForwardedPresentRecord {
public:
    ForwardedPresentRecord(bool forwards, const char* route, bool holdsInnerRow = false)
        : active_(forwards) {
        row_.emplace(forwards && PerfLogger::Get().IsEnabled() && !PerfLogger::HasInnerRowSink(), "DX12", route,
                     holdsInnerRow);
    }
    ~ForwardedPresentRecord() {
        if (active_) {
            ce::present_stage_cost::StageScope perfRowStage(ce::present_stage_cost::Stage::kPerfRow);
            row_.reset();
        }
    }
    ForwardedPresentRecord(const ForwardedPresentRecord&) = delete;
    ForwardedPresentRecord& operator=(const ForwardedPresentRecord&) = delete;

    // fps_limit_wait_us: the limiter's pre-present wait.
    template <typename Wait>
    void TimeLimiter(Wait&& wait) {
        const int64_t startUs = row_->NowUs();
        wait();
        row_->NoteFpsLimitWaitSince(startUs);
    }

    // Around the down-call. Begin again restarts it: a guarded overlay invoke that declined presented nothing.
    void BeginDownCall() {
        if (!active_) {
            return;
        }
        if (probe_) {
            probe_->Restart();
        } else {
            probe_.emplace(true);
        }
        row_->BeginPresentCall();
    }
    void EndDownCall() {
        if (active_) {
            row_->EndPresentCall();
            if (probe_) {
                probe_->Finish();
            }
        }
    }
    template <typename DownCall>
    HRESULT Forward(DownCall&& downCall) {
        BeginDownCall();
        const HRESULT hr = downCall();
        EndDownCall();
        return hr;
    }

private:
    bool active_;
    std::optional<DX11PresentPerfRow> row_;
    std::optional<ce::present_stall::DownCallProbe> probe_;
};

}  // namespace DXGIShared
