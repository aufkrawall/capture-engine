#pragma once

// When the "FG publication" line is worth a log entry. Frame generation turning on or off, or changing type, is always
// one. A factor change alone can come per game frame under dynamic multi-frame generation, and each change is a line on
// the Present thread: W3 (session 20261106_195839) spent 41% of its hook log's bytes here. Those keep the first few,
// which is where an activation or a stuck factor shows, then a heartbeat that carries the count.

#include <atomic>
#include <cstdint>

#include "common/logging/log_meter.h"

namespace ce::overlay_metrics {

class FGPublicationLogGate {
public:
    struct Verdict {
        bool log = false;
        uint32_t factorChanges = 0;  // since the last state change, this one included
    };

    Verdict Observe(bool active, int runtimeMode, int metricType, int multiplier) noexcept {
        const bool stateChanged = static_cast<bool>(state_.Observe(log_meter::FieldKey(active, runtimeMode, metricType)));
        const int previousMultiplier = multiplier_.exchange(multiplier, std::memory_order_acq_rel);
        if (stateChanged) {
            factorChanges_.store(0, std::memory_order_release);
            return {true, 0};
        }
        if (previousMultiplier == multiplier)
            return {};
        const uint32_t changes = factorChanges_.fetch_add(1, std::memory_order_acq_rel) + 1;
        return {log_meter::ShouldLogCadence(changes, kFactorChangeBurst, kFactorChangeStride), changes};
    }

    static constexpr uint32_t kFactorChangeBurst = 8;
    static constexpr uint32_t kFactorChangeStride = 512;

private:
    log_meter::ChangeGate state_;
    std::atomic<int> multiplier_{0};
    std::atomic<uint32_t> factorChanges_{0};
};

}  // namespace ce::overlay_metrics
