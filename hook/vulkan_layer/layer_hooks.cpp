/**
 * VK_LAYER_CE_overlay - Vulkan Hook Implementations
 * 
 * Intercepted Vulkan functions for overlay and capture.
 */

#include "layer_main.h"
#include "vulkan_layer.h"
#include <vector>
#include <cstring>
#include "../common/fps_limiter.h"

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

// vkQueueSubmit - wrapper to track submissions
VkResult VKAPI_CALL Capture_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    LayerLog("Layer: vkQueueSubmit (queue=%p, submits=%d)", queue, submitCount);
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    
    if (!disp || !disp->fp_vkQueueSubmit) {
        LayerLog("Layer: vkQueueSubmit FAILED - No dispatch for queue %p", queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkResult res = disp->fp_vkQueueSubmit(queue, submitCount, pSubmits, fence);
    return res;
}

// vkQueueSubmit2 - Vulkan 1.3 version
VkResult VKAPI_CALL Capture_vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence)
{
    LayerLog("Layer: vkQueueSubmit2 (queue=%p, submits=%d)", queue, submitCount);
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    
    if (!disp || !disp->fp_vkQueueSubmit2) {
        LayerLog("Layer: vkQueueSubmit2 FAILED - No dispatch for queue %p", queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    return disp->fp_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

// vkQueueSubmit2KHR - extension version
VkResult VKAPI_CALL Capture_vkQueueSubmit2KHR(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence)
{
    LayerLog("Layer: vkQueueSubmit2KHR (queue=%p, submits=%d)", queue, submitCount);
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    
    if (!disp || !disp->fp_vkQueueSubmit2KHR) {
        LayerLog("Layer: vkQueueSubmit2KHR FAILED - No dispatch for queue %p", queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    return disp->fp_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

// vkQueuePresentKHR - main hook for overlay and capture
VkResult VKAPI_CALL vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    // Track queue -> device mapping
    VkDevice device = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_QueueMapMutex);
        auto it = g_QueueToDevice.find(queue);
        if (it != g_QueueToDevice.end()) {
            device = it->second;
        }
    }
    
    // If device not tracked yet, find it from swapchain
    if (device == VK_NULL_HANDLE && pPresentInfo->swapchainCount > 0) {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        auto it = g_SwapchainMap.find(pPresentInfo->pSwapchains[0]);
        if (it != g_SwapchainMap.end()) {
            device = it->second.device;
            // Cache for future calls
            std::lock_guard<std::mutex> qlock(g_QueueMapMutex);
            g_QueueToDevice[queue] = device;
        }
    }
    
    if (device == VK_NULL_HANDLE) {
        return VK_ERROR_DEVICE_LOST;
    }
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkQueuePresentKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Apply FPS Limiter
    bool isFirstHook = !g_InPresentHook;
    g_InPresentHook = true;
    
    if (isFirstHook) {
        g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
        g_SharedFpsLimiter.Apply();
    }
    
    // Render overlay and capture before present
    if (g_LayerState.overlayEnabled && pPresentInfo->swapchainCount > 0) {
        uint32_t imageIndex = pPresentInfo->pImageIndices[0];
        RenderOverlay(device, queue, imageIndex, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    
    if (g_LayerState.captureEnabled && pPresentInfo->swapchainCount > 0) {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        auto it = g_SwapchainMap.find(pPresentInfo->pSwapchains[0]);
        if (it != g_SwapchainMap.end() && it->second.currentImageIndex < it->second.images.size()) {
            VkImage srcImage = it->second.images[it->second.currentImageIndex];
            VkSemaphore waitSemaphore = pPresentInfo->pWaitSemaphores && pPresentInfo->waitSemaphoreCount > 0 
                ? pPresentInfo->pWaitSemaphores[0] : VK_NULL_HANDLE;
            CaptureFrame(device, queue, srcImage, it->second.currentImageIndex, waitSemaphore);
        }
    }
    
    // Call the real present
    VkResult result = disp->fp_vkQueuePresentKHR(queue, pPresentInfo);
    
    if (isFirstHook) g_InPresentHook = false;
    return result;
}
