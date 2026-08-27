/**
 * VK_LAYER_CE_overlay - Vulkan Hook Implementations
 *
 * Intercepted Vulkan functions for overlay and capture.
 */

#include <cstring>
#include <vector>
#include "../common/fps_limiter.h"
#include "layer_main.h"
#include "vulkan_layer_internal.h"

// Swapchain tracking
struct SwapchainState {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    std::vector<VkImage> images;
    uint32_t currentImageIndex = 0;
    bool overlayInitialized = false;
    bool captureInitialized = false;
};

static std::mutex g_SwapchainMapMutex;
static std::unordered_map<VkSwapchainKHR, SwapchainState> g_SwapchainMap;

// Queue tracking
static std::mutex g_QueueMapMutex;
static std::unordered_map<VkQueue, VkDevice> g_QueueToDevice;

// OPTIMIZATION: Thread-local cache for queue lookups to avoid mutex contention
// on hot path
static thread_local VkQueue tls_LastQueue = VK_NULL_HANDLE;
static thread_local DeviceDispatch* tls_LastDispatch = nullptr;


namespace {

// Steady-state cost is the relaxed atomic load inside
// IsTrackingPresentDependencies; the map is only touched while the queue route
// and its small ring of producer-boundary semaphores are being identified.
inline void NotePresentTopologyDependencies(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits) {
    if (!pSubmits || !VulkanLayerState::Get().IsTrackingPresentDependencies())
        return;
    for (uint32_t i = 0; i < submitCount; ++i) {
        VulkanLayerState::Get().NoteSemaphoreDependencies(
            queue, pSubmits[i].pWaitSemaphores, pSubmits[i].waitSemaphoreCount, pSubmits[i].pSignalSemaphores,
            pSubmits[i].signalSemaphoreCount);
    }
}

inline void NotePresentTopologyDependencies2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits) {
    if (!pSubmits || !VulkanLayerState::Get().IsTrackingPresentDependencies())
        return;
    for (uint32_t i = 0; i < submitCount; ++i) {
        VulkanLayerState::Get().NoteSemaphoreDependencies2(
            queue, pSubmits[i].pWaitSemaphoreInfos, pSubmits[i].waitSemaphoreInfoCount,
            pSubmits[i].pSignalSemaphoreInfos, pSubmits[i].signalSemaphoreInfoCount);
    }
}

}  // namespace

// vkQueueSubmit - wrapper to track submissions
VkResult VKAPI_CALL Capture_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                          VkFence fence) {
    // Use TLS caching for performance
    if (queue == tls_LastQueue && tls_LastDispatch) {
        if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
            return tls_LastDispatch->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
        if (fence != VK_NULL_HANDLE) {}
        VulkanLayerState::Get().NoteQueueSubmit(queue);
        NotePresentTopologyDependencies(queue, submitCount, pSubmits);
        const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        const bool paceProducer = prerenderLimit >= 0.0f &&
                                  VulkanLayerState::Get().IsPrerenderProducerSubmit(queue, submitCount, pSubmits);
        ScopedBorrowedQueueSubmission submissionGuard(queue);
        const VkResult result = tls_LastDispatch->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
        if (result == VK_SUCCESS && paceProducer) {
            ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit,
                                      true);
        }
        return result;
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
        return disp->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);

    // Track fence for prerender limiting
    if (fence != VK_NULL_HANDLE) {}

    VulkanLayerState::Get().NoteQueueSubmit(queue);
    NotePresentTopologyDependencies(queue, submitCount, pSubmits);
    const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
    const bool paceProducer = prerenderLimit >= 0.0f &&
                              VulkanLayerState::Get().IsPrerenderProducerSubmit(queue, submitCount, pSubmits);
    ScopedBorrowedQueueSubmission submissionGuard(queue);
    const VkResult result = disp->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
    if (result == VK_SUCCESS && paceProducer) {
        ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit, true);
    }
    return result;
}

// vkQueueSubmit2 - Vulkan 1.3 version
VkResult VKAPI_CALL Capture_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                           VkFence fence) {
    // Use TLS caching for performance
    if (queue == tls_LastQueue && tls_LastDispatch) {
        if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
            return tls_LastDispatch->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
        if (fence != VK_NULL_HANDLE) {}
        VulkanLayerState::Get().NoteQueueSubmit(queue);
        NotePresentTopologyDependencies2(queue, submitCount, pSubmits);
        const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        const bool paceProducer = prerenderLimit >= 0.0f &&
                                  VulkanLayerState::Get().IsPrerenderProducerSubmit2(queue, submitCount, pSubmits);
        ScopedBorrowedQueueSubmission submissionGuard(queue);
        const VkResult result = tls_LastDispatch->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
        if (result == VK_SUCCESS && paceProducer) {
            ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit,
                                      true);
        }
        return result;
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
        return disp->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);

    // Track fence for prerender limiting
    if (fence != VK_NULL_HANDLE) {}

    VulkanLayerState::Get().NoteQueueSubmit(queue);
    NotePresentTopologyDependencies2(queue, submitCount, pSubmits);
    const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
    const bool paceProducer = prerenderLimit >= 0.0f &&
                              VulkanLayerState::Get().IsPrerenderProducerSubmit2(queue, submitCount, pSubmits);
    ScopedBorrowedQueueSubmission submissionGuard(queue);
    const VkResult result = disp->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
    if (result == VK_SUCCESS && paceProducer) {
        ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit, true);
    }
    return result;
}

// vkQueueSubmit2KHR - extension version
VkResult VKAPI_CALL Capture_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                              VkFence fence) {
    if (queue == tls_LastQueue && tls_LastDispatch) {
        if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
            return tls_LastDispatch->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
        VulkanLayerState::Get().NoteQueueSubmit(queue);
        NotePresentTopologyDependencies2(queue, submitCount, pSubmits);
        const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        const bool paceProducer = prerenderLimit >= 0.0f &&
                                  VulkanLayerState::Get().IsPrerenderProducerSubmit2(queue, submitCount, pSubmits);
        ScopedBorrowedQueueSubmission submissionGuard(queue);
        const VkResult result = tls_LastDispatch->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
        if (result == VK_SUCCESS && paceProducer) {
            ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit,
                                      true);
        }
        return result;
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit2KHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    if (!g_LayerState.whitelisted.load(std::memory_order_acquire))
        return disp->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);

    VulkanLayerState::Get().NoteQueueSubmit(queue);
    NotePresentTopologyDependencies2(queue, submitCount, pSubmits);
    const float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
    const bool paceProducer = prerenderLimit >= 0.0f &&
                              VulkanLayerState::Get().IsPrerenderProducerSubmit2(queue, submitCount, pSubmits);
    ScopedBorrowedQueueSubmission submissionGuard(queue);
    const VkResult result = disp->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    if (result == VK_SUCCESS && paceProducer) {
        ApplyPrerenderLimitVulkan(VulkanLayerState::Get().GetVkDeviceFromQueue(queue), queue, prerenderLimit, true);
    }
    return result;
}

// vkQueuePresentKHR implementation is in vulkan_layer.cpp (avoids duplicate
// symbol)
