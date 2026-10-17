#pragma once

// The clock of the phases one present times for its consumers: the DX11 perf CSV row and a D3D12
// frame's detailed debug sample (PresentDebugSample, the "[PERF] ... Present summary" line). Stamps
// are taken only while a consumer needs them, so a frame nobody reads costs no clock read; a stamp
// that was never taken is 0 and measures as 0, never as the absolute clock (the D3D12 summary showed
// "now - 0" until 2026-10-17, when the stamps were gated by the DX11-only CSV flag).

#include <cstdint>

#include "hook/metrics/perf_logger.h"

namespace ce {

class PresentPhaseClock {
public:
    using ClockFn = int64_t (*)();

    // `timePhases`: a consumer reads this present's phases. `clock` is overridable for tests.
    explicit PresentPhaseClock(bool timePhases, ClockFn clock = &PerfLogger::GetQpcUs)
        : timePhases_(timePhases), clock_(clock) {}

    bool Enabled() const { return timePhases_; }
    // A phase's start stamp; 0 while no consumer reads the phases.
    int64_t Now() const { return timePhases_ ? clock_() : 0; }
    // The phase since `startUs` (a Now()), in microseconds; 0 for a phase that was never stamped.
    int32_t ElapsedUs(int64_t startUs) const {
        return timePhases_ && startUs != 0 ? static_cast<int32_t>(clock_() - startUs) : 0;
    }

private:
    bool timePhases_;
    ClockFn clock_;
};

}  // namespace ce
