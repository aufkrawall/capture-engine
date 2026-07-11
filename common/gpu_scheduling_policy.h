#pragma once

#include <cstdint>

namespace ce::gpu_scheduling {

enum class HagsSupportState : uint32_t {
    kUnsupported = 0,
    kExperimental = 1,
    kStable = 2,
    kAlwaysOn = 3,
    kUnknown = 4,
};

struct HagsStatus {
    bool querySucceeded = false;
    bool supported = false;
    bool enabled = false;
    bool enabledByDefault = false;
    HagsSupportState supportState = HagsSupportState::kUnknown;
};

// D3DKMT_SCHEDULINGPRIORITYCLASS values are stable public ABI values.
constexpr int kSchedulingPriorityAboveNormal = 3;
constexpr int kSchedulingPriorityHigh = 4;

inline int ResolveAutomaticProcessSchedulingPriority(const HagsStatus& status) {
    return status.querySucceeded && status.enabled ? kSchedulingPriorityHigh : kSchedulingPriorityAboveNormal;
}

inline const char* HagsSupportStateName(HagsSupportState state) {
    switch (state) {
        case HagsSupportState::kUnsupported:
            return "unsupported";
        case HagsSupportState::kExperimental:
            return "experimental";
        case HagsSupportState::kStable:
            return "stable";
        case HagsSupportState::kAlwaysOn:
            return "always_on";
        default:
            return "unknown";
    }
}

}  // namespace ce::gpu_scheduling
