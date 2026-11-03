#pragma once

#include <cmath>
#include <cstdint>

#include "hook/metrics/perf_logger.h"
#include "hook/metrics/performance_metrics.h"

// The perf CSV row's rolling FPS columns. The 1% and 0.1% lows each walk about five seconds of frame
// history through a heap and then sort it, on the presenting thread, and feed nothing but these two
// columns: about 700 cold samples, twice per present, at 140 FPS. Over a five-second window a value 32
// frames old says the same thing, so the scans run once per kRefreshRows rows. The standard deviation is
// one atomic read and stays per row.
namespace ce::perf_row {

class SourceStats {
public:
    static constexpr uint32_t kRefreshRows = 32;

    void Fill(FrameMetrics& row, const ::PerformanceMetrics& source) {
        if (source_ != &source || rowsLeft_ == 0) {
            source_ = &source;
            rowsLeft_ = kRefreshRows;
            currentFpsTimes100_ = Times100(source.GetCurrentFPS());
            low1PctTimes100_ = Times100(source.Get1PercentLowFPS());
            lowPoint1PctTimes100_ = Times100(source.Get01PercentLowFPS());
        }
        --rowsLeft_;
        row.sourceCurrentFpsTimes100 = currentFpsTimes100_;
        row.source1PctLowTimes100 = low1PctTimes100_;
        row.sourcePoint1PctLowTimes100 = lowPoint1PctTimes100_;
        row.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(source.GetWindowStdDev()));
    }

private:
    static int32_t Times100(float value) {
        return static_cast<int32_t>(std::lround(value * 100.0f));
    }

    const ::PerformanceMetrics* source_ = nullptr;  // compared only, never read through
    uint32_t rowsLeft_ = 0;
    int32_t currentFpsTimes100_ = 0;
    int32_t low1PctTimes100_ = 0;
    int32_t lowPoint1PctTimes100_ = 0;
};

// Per thread, so two presenting threads never share the cached values.
inline void FillSourceStats(FrameMetrics& row, const ::PerformanceMetrics& source) {
    thread_local SourceStats stats;
    stats.Fill(row, source);
}

}  // namespace ce::perf_row
