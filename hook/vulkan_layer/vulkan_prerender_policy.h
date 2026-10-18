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

inline bool ShouldRelearnPresentTopology(uint32_t learnedQueueFamily, uint32_t currentQueueFamily) noexcept {
    constexpr uint32_t kUnknownQueueFamily = std::numeric_limits<uint32_t>::max();
    return learnedQueueFamily != kUnknownQueueFamily && currentQueueFamily != kUnknownQueueFamily &&
           learnedQueueFamily != currentQueueFamily;
}

}  // namespace ce::vulkan_prerender_policy
