#pragma once

#include <vulkan/vulkan.h>

#include <vector>

#include "vulkan_queue_depth_pacing_policy.h"

struct DeviceDispatch;
struct SwapchainData;

// Device-creation half of `backbuffer_count` queue-depth pacing (vulkan_queue_depth_pacing_policy.h).
struct QueueDepthPacingDeviceSetup {
    ce::vulkan_queue_depth_pacing::DeviceDecision decision =
        ce::vulkan_queue_depth_pacing::DeviceDecision::kNotConfigured;
    VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr, VK_FALSE};
    VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, nullptr, VK_FALSE};
    // What the create info looked like before CE added the extensions, so a refused create can be
    // retried without them.
    const void* pNextBefore = nullptr;
    size_t extensionCountBefore = 0;
};

// Decides, and when enabling appends both extensions to `extensions` and both feature structures to
// `modifiedCreateInfo`'s pNext (re-pointing its extension list at `extensions`).
void PrepareQueueDepthPacingForDevice(PFN_vkGetPhysicalDeviceFeatures2 getFeatures2, VkPhysicalDevice physicalDevice,
                                      const VkDeviceCreateInfo& applicationInfo,
                                      const std::vector<VkExtensionProperties>& availableExtensions,
                                      std::vector<const char*>& extensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                      QueueDepthPacingDeviceSetup& setup);

// Removes what PrepareQueueDepthPacingForDevice added; returns false when it added nothing.
bool WithdrawQueueDepthPacingFromCreateInfo(std::vector<const char*>& extensions,
                                            VkDeviceCreateInfo& modifiedCreateInfo,
                                            QueueDepthPacingDeviceSetup& setup);

void FinishQueueDepthPacingForDevice(DeviceDispatch* dispatch, VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
                                     const QueueDepthPacingDeviceSetup& setup);

// Present half. Called right before the down-call, after the FPS limiter, like the DXGI flip-queue
// wait. Blocks until the present `backbuffer_count - 1` back reached the display (bounded by the
// ceiling) and chains CE's VkPresentIdKHR in front of `presentChain`; returns true when it chained.
struct QueueDepthPresentTag {
    VkPresentIdKHR node = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, nullptr, 0, nullptr};
    uint64_t presentId = 0;
};

bool PaceVulkanPresentQueueDepth(DeviceDispatch* dispatch, VkDevice device, SwapchainData* swapchain,
                                 const VkPresentInfoKHR* presentInfo, bool frameGenerationOwnsPlacement,
                                 QueueDepthPresentTag& tag, const void*& presentChain);

void NoteVulkanQueueDepthPresentResult(SwapchainData* swapchain, const QueueDepthPresentTag& tag, VkResult result);

// Diagnostics across swapchain generations (policy::CarryWindow): a swapchain about to be destroyed
// hands its open statistics window to its window handle, logging it first when already due; the
// next swapchain created for that window on the same device adopts it.
void HandOverVulkanQueueDepthWindow(SwapchainData& swapchain);
void AdoptVulkanQueueDepthWindow(SwapchainData& swapchain);
