#pragma once

// When a thread mismatch counts as evidence that the application does not
// present from the thread currently inside the present hook. Pure policy
// beside vulkan_present_boundary.h - which resolves the present hook's limiter
// boundary against live state - so both detection windows are unit-testable.
//
// Both routes move the limiter boundary to vkAcquireNextImageKHR: pacing
// inside the present of a split production/presentation topology cannot
// throttle production, while the acquire boundary can. The evidence is only
// evidence while it is fresh: a worker thread that submitted once, minutes
// ago, says nothing about the present being asynchronous now. The submit
// route originally had no staleness window at all and latched for the whole
// session off a single background-worker submit.

#include <cstdint>

namespace ce::vulkan_present_boundary {

enum class AsyncRoute { kNone, kAcquireThreadMismatch, kSubmitThreadMismatch };

inline const char* AsyncRouteName(AsyncRoute route) {
    switch (route) {
        case AsyncRoute::kAcquireThreadMismatch:
            return "acquire-thread mismatch";
        case AsyncRoute::kSubmitThreadMismatch:
            return "submit-thread mismatch";
        default:
            return "none";
    }
}

inline constexpr uint64_t kThreadRecencyWindowMs = 2000ULL;

inline bool IsThreadRecencyFresh(uint64_t lastTickMs, uint64_t nowTickMs) {
    return lastTickMs != 0 && (nowTickMs - lastTickMs) < kThreadRecencyWindowMs;
}

inline AsyncRoute DetectAcquireThreadMismatch(uint32_t acquireThreadId, uint64_t lastAcquireTickMs,
                                              uint32_t currentThreadId, uint64_t nowTickMs) {
    if (acquireThreadId != 0 && acquireThreadId != currentThreadId &&
        IsThreadRecencyFresh(lastAcquireTickMs, nowTickMs)) {
        return AsyncRoute::kAcquireThreadMismatch;
    }
    return AsyncRoute::kNone;
}

// The submit route keeps the same recency window as the acquire route: a
// mismatched submit thread only counts while it is still the one submitting.
inline AsyncRoute DetectSubmitThreadMismatch(uint32_t submitThreadId, uint64_t lastSubmitTickMs,
                                             uint32_t currentThreadId, uint64_t nowTickMs) {
    if (submitThreadId != 0 && submitThreadId != currentThreadId &&
        IsThreadRecencyFresh(lastSubmitTickMs, nowTickMs)) {
        return AsyncRoute::kSubmitThreadMismatch;
    }
    return AsyncRoute::kNone;
}

}  // namespace ce::vulkan_present_boundary
