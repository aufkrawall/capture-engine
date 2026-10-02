#pragma once

#include <atomic>

#include "../common/hook_common.h"
#include "sl_result.h"
#include "streamline_bridge_result_log.h"

// Logging shared by the bridge's translation units: one line per distinct refusal reason or
// failing call, so a first bridged run diagnoses itself without a per-frame call turning the
// log into noise.
namespace ce::streamline_bridge {

inline void RefuseOnce(std::atomic<bool>& latch, const char* what, const char* why) {
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: refusing %s - %s", what, why);
    }
}

// A failing call is logged at its 1st, 2nd, 4th... consecutive failure and on a new result, and
// once more when it succeeds again (see streamline_bridge_result_log.h).
static_assert(static_cast<int>(sl::Result::eOk) == 0 && static_cast<int>(sl::Result::eErrorNGXFailed) == 15 &&
                  static_cast<int>(sl::Result::eErrorInvalidState) == 38 &&
                  static_cast<int>(sl::Result::eWarnOutOfVRAM) == 39,
              "ResultCodeName's table follows sl::Result's numbering");

inline bool ResultOk(sl::Result result, const char* call, ResultTracker& tracker) {
    const ResultLogDecision decision = tracker.Observe(static_cast<int>(result));
    if (decision.event == ResultLogEvent::kFailure) {
        HookLogImportant(
            "Streamline bridge: %s returned sl::Result=%d (%s) - %u consecutive failure(s), %u in total; the title "
            "sees the call fail",
            call, static_cast<int>(result), ResultCodeName(static_cast<int>(result)), decision.failures, decision.total);
    } else if (decision.event == ResultLogEvent::kRecovered) {
        HookLogImportant("Streamline bridge: %s succeeds again after %u consecutive failure(s) (%u in total)", call,
                         decision.failures, decision.total);
    }
    return result == sl::Result::eOk;
}

}  // namespace ce::streamline_bridge
