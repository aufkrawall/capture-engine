#pragma once

#include <atomic>

#include "../common/hook_common.h"
#include "sl_result.h"

// Logging shared by the bridge's translation units: one line per distinct refusal reason or
// failing call, so a first bridged run diagnoses itself without a per-frame call turning the
// log into noise.
namespace ce::streamline_bridge {

inline void RefuseOnce(std::atomic<bool>& latch, const char* what, const char* why) {
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: refusing %s - %s", what, why);
    }
}

inline bool ResultOk(sl::Result result, const char* call, std::atomic<bool>& latch) {
    if (result == sl::Result::eOk) {
        return true;
    }
    if (!latch.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant("Streamline bridge: %s returned sl::Result=%d", call, static_cast<int>(result));
    }
    return false;
}

}  // namespace ce::streamline_bridge
