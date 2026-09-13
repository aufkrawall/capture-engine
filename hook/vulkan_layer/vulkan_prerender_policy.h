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

inline bool ShouldRelearnPresentTopology(uint32_t learnedQueueFamily, uint32_t currentQueueFamily) noexcept {
    constexpr uint32_t kUnknownQueueFamily = std::numeric_limits<uint32_t>::max();
    return learnedQueueFamily != kUnknownQueueFamily && currentQueueFamily != kUnknownQueueFamily &&
           learnedQueueFamily != currentQueueFamily;
}

}  // namespace ce::vulkan_prerender_policy
