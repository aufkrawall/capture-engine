/**
 * VK_LAYER_CE_overlay - Vulkan Hook Implementations
 *
 * Intercepted Vulkan functions for overlay and capture.
 */

#include <cstring>
#include <vector>
#include "../common/fps_limiter.h"
#include "layer_main.h"
#include "vulkan_layer.h"

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

// OPTIMIZATION: Thread-local cache for queue lookups to avoid mutex contention on hot path
static thread_local VkQueue tls_LastQueue = VK_NULL_HANDLE;
static thread_local DeviceDispatch* tls_LastDispatch = nullptr;

// vkQueueSubmit - wrapper to track submissions
VkResult VKAPI_CALL Capture_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                          VkFence fence)
{
    if (queue == tls_LastQueue && tls_LastDispatch) {
        return tls_LastDispatch->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    return disp->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

// vkQueueSubmit2 - Vulkan 1.3 version
VkResult VKAPI_CALL Capture_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                           VkFence fence)
{
    if (queue == tls_LastQueue && tls_LastDispatch) {
        return tls_LastDispatch->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit2) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    return disp->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

// vkQueueSubmit2KHR - extension version
VkResult VKAPI_CALL Capture_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits,
                                              VkFence fence)
{
    if (queue == tls_LastQueue && tls_LastDispatch) {
        return tls_LastDispatch->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);

    if (!disp || !disp->fp_vkQueueSubmit2KHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    tls_LastQueue = queue;
    tls_LastDispatch = disp;

    return disp->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

// vkQueuePresentKHR implementation is in vulkan_layer.cpp (avoids duplicate symbol)
