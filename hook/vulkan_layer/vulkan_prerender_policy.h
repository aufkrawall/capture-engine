#pragma once

#include <cstdint>

namespace ce::vulkan_prerender_policy {

// Vulkan queues are externally synchronized. A marker for a graphics queue
// owned by another thread must be appended by that queue's submit wrapper, not
// borrowed later by the presentation thread.
inline bool ShouldPaceOnProducerSubmit(uint32_t producerThreadId, uint32_t presentThreadId) noexcept {
    return producerThreadId != 0 && presentThreadId != 0 && producerThreadId != presentThreadId;
}

}  // namespace ce::vulkan_prerender_policy
