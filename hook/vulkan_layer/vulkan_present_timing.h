#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <vector>

struct DeviceDispatch;
struct InstanceDispatch;
struct SwapchainData;

struct PresentTimingDeviceEnablement {
    VkPhysicalDevicePresentTimingFeaturesEXT injectedFeatures = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT, nullptr, VK_FALSE, VK_FALSE, VK_FALSE};
    const void* previousPNext = nullptr;
    size_t extensionCountBefore = 0;
    bool enabled = false;
    bool featureNodeAdded = false;
    bool extensionsAdded = false;
};

struct PresentTimingSwapchainEnablement {
    bool enabled = false;
    bool flagAdded = false;
};

void PopulatePresentTimingInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance,
                                           PFN_vkGetInstanceProcAddr gipa);
void PopulatePresentTimingDeviceDispatch(DeviceDispatch* dispatch, VkDevice device,
                                         PFN_vkGetDeviceProcAddr gdpa);

bool EnablePresentTimingSurfaceQueries(PFN_vkGetInstanceProcAddr gipa, bool requested,
                                       std::vector<const char*>& extensions, bool* extensionAdded);

void PreparePresentTimingDevice(InstanceDispatch* instanceDispatch, VkPhysicalDevice physicalDevice,
                                const VkDeviceCreateInfo& applicationCreateInfo,
                                const std::vector<VkExtensionProperties>& availableExtensions,
                                std::vector<const char*>& enabledExtensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                PresentTimingDeviceEnablement& enablement);

VkResult CreateDeviceWithPresentTimingFallback(PFN_vkCreateDevice createFunction,
                                               VkPhysicalDevice physicalDevice,
                                               std::vector<const char*>& enabledExtensions,
                                               VkDeviceCreateInfo& modifiedCreateInfo,
                                               PresentTimingDeviceEnablement& enablement,
                                               const VkAllocationCallbacks* allocator,
                                               VkDevice* device);

PresentTimingSwapchainEnablement PreparePresentTimingSwapchain(
    DeviceDispatch* deviceDispatch, const VkSwapchainCreateInfoKHR& applicationCreateInfo,
    VkSwapchainCreateInfoKHR& modifiedCreateInfo);

void InitializePresentTimingSwapchain(DeviceDispatch* deviceDispatch, SwapchainData* swapchainData);

bool BuildRelativePresentTiming(DeviceDispatch* deviceDispatch, SwapchainData* swapchainData,
                                const VkPresentInfoKHR& applicationPresentInfo, bool fifoPresentModeActive,
                                const void* nextChain,
                                VkPresentTimingInfoEXT& timingInfo, VkPresentTimingsInfoEXT& timingsInfo);
