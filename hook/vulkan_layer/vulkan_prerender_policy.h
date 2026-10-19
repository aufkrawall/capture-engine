#pragma once

#include <cstdint>
#include <limits>

namespace ce::vulkan_prerender_policy {

// Vulkan queues are externally synchronized. A marker for a graphics queue
// owned by another thread must be appended by that queue's submit wrapper, not
// borrowed later by the presentation thread.
inline bool ShouldPaceOnProducerSubmit(uint32_t producerThreadId, uint32_t presentThreadId) noexcept {
    return producerThreadId != 0 && presentThreadId != 0 && producerThreadId != presentThreadId;
}

// Whether the present hook may append the queue-depth marker itself.
// vkQueuePresentKHR obliges the application to hold the present queue
// exclusively for the whole call, so a marker on that queue is synchronized no
// matter which threads otherwise submit to it; the async-present latch (a
// limiter-boundary signal) must not withhold it. Strange Brigade Vulkan
// (session 20261018_172054) presents from its producer queue while a second
// thread's submits latch async-present, and the latch alone paced nothing. A
// different producer queue is only safe from the thread that last submitted to
// it, and only while presentation is not asynchronous.
inline bool CanAppendMarkerAtPresent(bool markerOnPresentQueue, bool asyncPresentDetected,
                                     uint32_t producerThreadId, uint32_t presentThreadId) noexcept {
    if (markerOnPresentQueue)
        return true;
    return !asyncPresentDetected && (producerThreadId == 0 || producerThreadId == presentThreadId);
}

// The producer-submit route is learned in a bounded window after swapchain
// creation, but an engine may move its render submissions onto a worker thread
// long after that window closed (Strange Brigade Vulkan, session
// 20261018_170350: same-thread menu, split 11 s later). The present path can
// then neither append the marker itself nor rely on a producer route, so it
// re-arms boundary learning once per same-thread -> split transition.
inline bool ShouldRearmProducerRouteLearning(bool producerSubmitArmed, bool learningActive,
                                             bool rearmedForCurrentSplit, uint32_t producerThreadId,
                                             uint32_t presentThreadId) noexcept {
    return !producerSubmitArmed && !learningActive && !rearmedForCurrentSplit &&
           ShouldPaceOnProducerSubmit(producerThreadId, presentThreadId);
}

// The `Vulkan Prerender: summary` line covers a wall-clock window, like
// [D3D11 PRERENDER]: a fixed marker count logged ~2 lines/s in a ~2000 fps menu
// (Strange Brigade, session 20261018_173529). The window is per queue, so it
// spans the swapchain recreations of a game that rebuilds every few seconds.
inline constexpr uint64_t kSummaryWindowUs = 10ull * 1000ull * 1000ull;

inline bool IsSummaryWindowDue(uint64_t windowStartUs, uint64_t nowUs) noexcept {
    return windowStartUs != 0 && nowUs >= windowStartUs && nowUs - windowStartUs >= kSummaryWindowUs;
}

inline bool ShouldRelearnPresentTopology(uint32_t learnedQueueFamily, uint32_t currentQueueFamily) noexcept {
    constexpr uint32_t kUnknownQueueFamily = std::numeric_limits<uint32_t>::max();
    return learnedQueueFamily != kUnknownQueueFamily && currentQueueFamily != kUnknownQueueFamily &&
           learnedQueueFamily != currentQueueFamily;
}

}  // namespace ce::vulkan_prerender_policy
