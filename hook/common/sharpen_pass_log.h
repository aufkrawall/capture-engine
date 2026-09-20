#pragma once

#include <atomic>
#include <cstdint>

#include "../../common/sharpen_policy.h"
#include "hook_common.h"

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
    // True when this frame's decision should be written to the log.
    bool ShouldLog(bool run, const char* reason) {
        const bool changed = run != lastRun_ || reason != lastReason_;
        lastRun_ = run;
        lastReason_ = reason;
        if (changed) {
            repeatCount_ = 0;
            return true;
        }
        return (++repeatCount_ % kSteadyStateInterval) == 0;
    }

private:
    // About once every two minutes at 120 displayed frames per second.
    static constexpr uint32_t kSteadyStateInterval = 15000;

    bool lastRun_ = false;
    // Compared by pointer: every reason is a string literal owned by the policy
    // header, so identity is exactly "the same refusal as last frame".
    const char* lastReason_ = nullptr;
    uint32_t repeatCount_ = 0;
};

}  // namespace ce::sharpen
