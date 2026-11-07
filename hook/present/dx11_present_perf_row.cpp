#include "dx11_present_perf_row.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"
#include "hook/d3d11/dx11_frame_phases.h"
#include "hook/present/present_row_owner_log.h"
#include "hook/metrics/perf_row_source_stats.h"
#include "hook/metrics/performance_metrics.h"

#include <cstring>

namespace DXGIShared {
namespace {

void LogRowOwner(const char* site, const FrameMetrics& metrics, PresentRowOwner owner) {
    static PresentRowOwnerLog s_ownerLog;
    const auto verdict = s_ownerLog.Observe(site, metrics, owner);
    if (verdict) {
        const char* const how = owner == PresentRowOwner::kReplacedByInnerRow ? "the forwarded present logged its own row"
                                : owner == PresentRowOwner::kHeldInnerRow ? "the frame's own row, with the down-call split"
                                                                          : "logged";
        HookLog("[PERF CSV] present row of %s (api=%s): %s%s", site, metrics.api, how,
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

// overlay_us read 0 on every PostSL route and total_us about 8 us, with CE spending 116-172 us per Present
// (20261104_221303), so the CSV showed DLSS FG as free. A draw that started before `totalSpanStartUs` is outside the span
// total_us measures.
void AddDrawOutsideRow(FrameMetrics& row, int64_t totalSpanStartUs) {
    const PerfLogger::DrawOutsideRow draw = PerfLogger::TakeDrawOutsideRow();
    row.overlayUs += draw.durationUs;
    if (draw.startUs < totalSpanStartUs) {
        row.totalUs += draw.durationUs;
    }
}

}  // namespace

DX11PresentPerfRow::DX11PresentPerfRow(bool enabled, const char* api, const char* site, bool holdsInnerRow)
    : enabled_(enabled), holdsInnerRow_(enabled && holdsInnerRow), site_(site) {
    if (!enabled_) {
        return;
    }
    metrics_.qpcUs = PerfLogger::GetQpcUs();
    strncpy(metrics_.api, api, sizeof(metrics_.api) - 1);
    metrics_.api[sizeof(metrics_.api) - 1] = '\0';
    // Outermost present row: skipped when the forwarded present logged an inner row, so a present never
    // writes two CSV rows (present-rate dedup).
    PerfLogger::BeginPresentRowScope();
    if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
        metrics_.sourceFrameIndex = GetLatestSourceFrameIndex();
        metrics_.sourceCapturePhase = sharedMemory->runtimeState.capturePhase.load(std::memory_order_relaxed);
        metrics_.sourceEncoderQueueDepth = sharedMemory->encoderQueueDepth.load(std::memory_order_relaxed);
        metrics_.sourceMuxQueueKb =
            (sharedMemory->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        metrics_.sourceOverloadFlags = sharedMemory->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (PerformanceMetrics* perf = GetPerformanceMetrics()) {
        ce::perf_row::FillSourceStats(metrics_, *perf);
    }
    if (holdsInnerRow_) {
        // The frame's own row brings its columns; this row adds the split around the down-call.
        previousSink_ = PerfLogger::SetInnerRowSink(&held_);
        return;
    }
    // Phases a frame left behind that no row took belong to no present of this row.
    (void)TakeDX11FramePhases();
}

DX11PresentPerfRow::~DX11PresentPerfRow() {
    if (!enabled_) {
        return;
    }
    if (holdsInnerRow_) {
        LogHeldRow();
        return;
    }
    if (PerfLogger::InnerRowLoggedInPresentRowScope()) {
        LogRowOwner(site_, metrics_, PresentRowOwner::kReplacedByInnerRow);
        return;
    }
    const int64_t endUs = PerfLogger::GetQpcUs();
    metrics_.totalUs = static_cast<int32_t>(endUs - metrics_.qpcUs);
    if (presentCallEndUs_ != 0) {
        metrics_.postPresentUs = static_cast<int32_t>(endUs - presentCallEndUs_);
    }
    // The CPU prerender wait of this present: the detour waits before the row of an inner present
    // opens, and inside the row of a wrapper present. Not part of total_us.
    metrics_.prerenderWaitUs = TakeD3D11PrerenderWaitUs();
    AddDrawOutsideRow(metrics_, metrics_.qpcUs);
    PerfLogger::Get().LogFrame(metrics_);
    LogRowOwner(site_, metrics_, PresentRowOwner::kLogged);
}

void DX11PresentPerfRow::LogHeldRow() {
    PerfLogger::SetInnerRowSink(previousSink_);
    const int64_t endUs = PerfLogger::GetQpcUs();
    FrameMetrics row = held_.filled ? held_.row : metrics_;
    if (!held_.filled) {
        row.totalUs = static_cast<int32_t>(endUs - metrics_.qpcUs);  // the catch-all: the whole hook
    }
    if (presentCallEndUs_ != 0) {
        row.prePresentUs = metrics_.prePresentUs;
        row.presentCallUs = metrics_.presentCallUs;
        row.postPresentUs = static_cast<int32_t>(endUs - presentCallEndUs_);
    }
    if (fpsLimitNoted_) {
        row.fpsLimitWaitUs = metrics_.fpsLimitWaitUs;
    }
    // A held row's total_us is its ProcessFrame's span, which a PostSL draw is never part of.
    AddDrawOutsideRow(row, held_.filled ? INT64_MAX : metrics_.qpcUs);
    PerfLogger::Get().LogFrame(row);
    LogRowOwner(site_, row, held_.filled ? PresentRowOwner::kHeldInnerRow : PresentRowOwner::kLogged);
}

void DX11PresentPerfRow::TakeFramePhases() {
    if (!enabled_) {
        return;
    }
    const DX11FramePhases phases = TakeDX11FramePhases();
    metrics_.overlayUs = phases.overlayUs;
    metrics_.captureUs = phases.captureUs;
    metrics_.overlayGpuUs = phases.overlayGpuUs;
}

void DX11PresentPerfRow::NoteFpsLimitWaitSince(int64_t startUs) {
    if (enabled_) {
        metrics_.fpsLimitWaitUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - startUs);
        fpsLimitNoted_ = true;
    }
}

void DX11PresentPerfRow::BeginPresentCall() {
    if (enabled_) {
        presentCallStartUs_ = PerfLogger::GetQpcUs();
    }
}

void DX11PresentPerfRow::EndPresentCall() {
    if (!enabled_ || presentCallStartUs_ == 0) {
        return;
    }
    presentCallEndUs_ = PerfLogger::GetQpcUs();
    metrics_.prePresentUs = static_cast<int32_t>(presentCallStartUs_ - metrics_.qpcUs);
    metrics_.presentCallUs = static_cast<int32_t>(presentCallEndUs_ - presentCallStartUs_);
}

}  // namespace DXGIShared
