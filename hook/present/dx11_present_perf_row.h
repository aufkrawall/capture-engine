#pragma once

// The perf CSV row (perf_metrics_*.csv) of one DX11/DX10 present, owned by the outermost present hook
// that runs CE's frame work for it: CE's swapchain wrapper or the DXGI detours on the real swapchain
// (each for Present and Present1). Opens the present-row dedup scope and logs on destruction, on any
// exit path, unless the forwarded present logged an inner row. Inert while the CSV is off: no clock is
// read.
//
// Columns: overlay_us, capture_us and overlay_gpu_us are CE's frame work, timed where it ran
// (TakeDX11FramePhases); pre_present_us is CE's work in the hook before the down-call,
// present_call_us the down-call alone, post_present_us what follows it (frame-latency wait, limiter).
// The hook log names each hook's first row and every change of whether its row or the forwarded
// present's own row was written ([PERF CSV]): the CSV's api column cannot tell the hooks apart.
//
// A D3D12 present's row is its ProcessFrame's, logged before the Present (total_us is that span). A row
// constructed with `holdsInnerRow` (the DXGI detours for D3D12, 2026-10-18) holds that row until the
// down-call returned and logs it with the split above and this present's fps_limit_wait_us (ProcessFrame
// can only report the previous present's); with no such row it logs the catch-all (total_us and split).

#include <cstdint>

#include "hook/metrics/perf_logger.h"

namespace DXGIShared {

class DX11PresentPerfRow {
public:
    // `site` names the hook for the log; a string literal.
    DX11PresentPerfRow(bool enabled, const char* api, const char* site, bool holdsInnerRow = false);
    ~DX11PresentPerfRow();
    DX11PresentPerfRow(const DX11PresentPerfRow&) = delete;
    DX11PresentPerfRow& operator=(const DX11PresentPerfRow&) = delete;

    bool Enabled() const { return enabled_; }
    // The clock for a phase the hook times itself; 0 while the CSV is off.
    int64_t NowUs() const { return enabled_ ? PerfLogger::GetQpcUs() : 0; }
    // After the frame processing: the phases of the frame this thread just processed.
    void TakeFramePhases();
    // fps_limit_wait_us: the limiter's pre-present wait, from NowUs() before it.
    void NoteFpsLimitWaitSince(int64_t startUs);
    // Around the down-call. Calling Begin again restarts it: CE's waits before the call stay pre-present.
    void BeginPresentCall();
    void EndPresentCall();

private:
    void LogHeldRow();

    bool enabled_;
    bool holdsInnerRow_;
    bool fpsLimitNoted_ = false;
    const char* site_;
    FrameMetrics metrics_{};
    PerfLogger::HeldRow held_{};
    PerfLogger::HeldRow* previousSink_ = nullptr;
    int64_t presentCallStartUs_ = 0;
    int64_t presentCallEndUs_ = 0;
};

}  // namespace DXGIShared
