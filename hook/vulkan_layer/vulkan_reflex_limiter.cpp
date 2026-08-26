#include "vulkan_layer_internal.h"

#include "vulkan_reflex_limiter.h"

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                                                               const VkLatencySleepModeInfoNV* pSleepModeInfo) {
    DeviceDispatch* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    return g_VulkanReflexLimiter.InterceptSetSleepMode(
        device, swapchain, pSleepModeInfo, dispatch ? dispatch->fp_vkSetLatencySleepModeNV : nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                                                        const VkLatencySleepInfoNV* pSleepInfo) {
    DeviceDispatch* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    return g_VulkanReflexLimiter.InterceptSleep(device, swapchain, pSleepInfo,
                                                dispatch ? dispatch->fp_vkLatencySleepNV : nullptr);
}
