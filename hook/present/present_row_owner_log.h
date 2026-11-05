#pragma once

#include <cstdint>

#include "common/logging/log_meter.h"
#include "hook/metrics/perf_logger.h"

namespace DXGIShared {

enum class PresentRowOwner : uint8_t { kLogged, kReplacedByInnerRow, kHeldInnerRow };

// When a present route's [PERF CSV] line is due: on its first row and when what it writes changes. The gate fails toward
// logging once full, so it must hold every route that writes a row, 15 in 2026-11. With room for 4, the fifth route of a
// session logged every row: post-FSR DLSS FG x1076, 14% of the log, `perf_row` 7.8 us instead of 0.6 (20261105_011907).
class PresentRowOwnerLog {
public:
    ce::log_meter::ChangeGate::Verdict Observe(const char* route, const FrameMetrics& row,
                                               PresentRowOwner owner) noexcept {
        return gate_.Observe(ce::log_meter::FieldKey(route), ce::log_meter::FieldKey(row.api, owner));
    }

private:
    ce::log_meter::StreamChangeGate<64> gate_;
};

}  // namespace DXGIShared
