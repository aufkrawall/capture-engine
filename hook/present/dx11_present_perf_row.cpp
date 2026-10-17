#include "dx11_present_perf_row.h"

#include "dxgi_shared_internal.h"
#include "common/logging/log_meter.h"
#include "hook/d3d11/dx11_frame_phases.h"
#include "hook/metrics/performance_metrics.h"

#include <cmath>
#include <cstring>

namespace DXGIShared {
namespace {

void LogRowOwner(const char* site, const FrameMetrics& metrics, bool replacedByInnerRow) {
    static ce::log_meter::StreamChangeGate<4> s_ownerGate;
    const auto verdict =
        s_ownerGate.Observe(ce::log_meter::FieldKey(site), ce::log_meter::FieldKey(metrics.api, replacedByInnerRow));
    if (verdict) {
        HookLog("[PERF CSV] DX11 present row of %s (api=%s): %s%s", site, metrics.api,
                replacedByInnerRow ? "the forwarded present logged its own row" : "logged",
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

}  // namespace

DX11PresentPerfRow::DX11PresentPerfRow(bool enabled, const char* api, const char* site)
    : enabled_(enabled), site_(site) {
    if (!enabled_) {
        return;
    }
    metrics_.qpcUs = PerfLogger::GetQpcUs();
    strncpy(metrics_.api, api, sizeof(metrics_.api) - 1);
    metrics_.api[sizeof(metrics_.api) - 1] = '\0';
    // Outermost present row: skipped when the forwarded present logged an inner row, so a present never
    // writes two CSV rows (present-rate dedup).
    PerfLogger::BeginPresentRowScope();
    // Phases a frame left behind that no row took belong to no present of this row.
    (void)TakeDX11FramePhases();
    if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
        metrics_.sourceFrameIndex = GetLatestSourceFrameIndex();
        metrics_.sourceCapturePhase = sharedMemory->runtimeState.capturePhase.load(std::memory_order_relaxed);
        metrics_.sourceEncoderQueueDepth = sharedMemory->encoderQueueDepth.load(std::memory_order_relaxed);
        metrics_.sourceMuxQueueKb =
            (sharedMemory->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        metrics_.sourceOverloadFlags = sharedMemory->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (PerformanceMetrics* perf = GetPerformanceMetrics()) {
        metrics_.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
        metrics_.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
        metrics_.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
        metrics_.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
    }
}

DX11PresentPerfRow::~DX11PresentPerfRow() {
    if (!enabled_) {
        return;
    }
    if (PerfLogger::InnerRowLoggedInPresentRowScope()) {
        LogRowOwner(site_, metrics_, true);
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
    PerfLogger::Get().LogFrame(metrics_);
    LogRowOwner(site_, metrics_, false);
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
