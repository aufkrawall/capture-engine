#pragma once

#include <atomic>
#include <cstdint>

#include "common/graphics/sharpen_policy.h"
#include "common/logging/log_meter.h"
#include "hook/runtime/hook_common.h"

// Shared rate limiter for the sharpen renderers' per-present diagnostics.
//
// The pass runs on every displayed frame, so an unconditional log line is a
// hot-path defect. What actually has to be diagnosable is *transitions*: the
// frame the pass started running on, the frame it stopped, and why. A steady
// state repeats at a low rate so a long session still proves the pass kept
// running rather than silently stopping after the first frame.
namespace ce::sharpen {

class DecisionLogGate {
public:
    DecisionLogGate() = default;
    DecisionLogGate(DecisionLogGate&&) noexcept {}
    DecisionLogGate& operator=(DecisionLogGate&&) noexcept {
        gate_.Reset();
        return *this;
    }

    bool ShouldLog(const Decision& decision) {
        return ShouldLog(decision, decision.run, decision.reason);
    }

    bool ShouldLog(const Decision& decision, bool run, const char* reason) {
        return static_cast<bool>(gate_.ObserveOrEvery(
            ce::log_meter::FieldKey(run, reason, decision.mode, decision.gamma.source,
                                   decision.gamma.destination, decision.gammaValuesLinear,
                                   decision.gammaDitherScale, decision.gammaOutputLinear, decision.intensity, decision.effectParameter),
            ++repeatCount_, kSteadyStateInterval));
    }

    // True when this frame's decision should be written to the log.
    bool ShouldLog(bool run, const char* reason) {
        return static_cast<bool>(gate_.ObserveOrEvery(ce::log_meter::FieldKey(run, reason),
                                                      ++repeatCount_, kSteadyStateInterval));
    }

private:
    ce::log_meter::ChangeGate gate_;
    // About once every two minutes at 120 displayed frames per second.
    static constexpr uint32_t kSteadyStateInterval = 15000;

    uint32_t repeatCount_ = 0;
};

}  // namespace ce::sharpen
