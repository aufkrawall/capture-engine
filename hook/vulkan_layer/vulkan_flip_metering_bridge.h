#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <vector>

#include "vulkan_flip_metering_bridge_policy.h"

struct DeviceDispatch;
struct SwapchainData;

// Vulkan side of the D3D12 flip-metering bridge; the policy and the measurement behind it are in
// vulkan_flip_metering_bridge_policy.h, the D3D12 side in hook/ngx/flip_metering_bridge.h.

// Everything vkCreateDevice changed for the bridge, so a failed create can be retried without it.
struct FlipMeteringBridgeDeviceSetup {
    ce::vulkan_flip_metering_bridge::DeviceDecision decision =
        ce::vulkan_flip_metering_bridge::DeviceDecision::kNotD3D12Translation;
    ce::vulkan_flip_metering_bridge::PhysicalDevicePresentMeteringFeaturesNV features = {};
    const void* pNextBefore = nullptr;
    size_t extensionCountBefore = 0;
    bool applied = false;
};

// Adds VK_NV_present_metering and its feature to a D3D12 translation device's create info.
void PrepareFlipMeteringBridgeForDevice(bool d3d12TranslationEngine, PFN_vkGetPhysicalDeviceFeatures2 getFeatures2,
                                        VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& applicationInfo,
                                        const std::vector<VkExtensionProperties>& availableExtensions,
                                        std::vector<const char*>& extensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                        FlipMeteringBridgeDeviceSetup& setup);

// Undoes PrepareFlipMeteringBridgeForDevice after vkCreateDevice failed with it. Returns true when there
// was something to withdraw, i.e. when a retry can differ.
bool WithdrawFlipMeteringBridgeFromCreateInfo(std::vector<const char*>& extensions,
                                              VkDeviceCreateInfo& modifiedCreateInfo,
                                              FlipMeteringBridgeDeviceSetup& setup);

// Records the outcome on the created device. `applicationEnabledPresentMetering` is the application's
// own VK_NV_present_metering, which this also publishes to the hook DLL.
void FinishFlipMeteringBridgeForDevice(DeviceDispatch* dispatch, const VkDeviceCreateInfo& applicationInfo,
                                       const FlipMeteringBridgeDeviceSetup& setup);

// vkSetLatencyMarkerNV: vkd3d announces each present's frame id here, on its present thread, right
// before vkQueuePresentKHR.
extern "C" VKAPI_ATTR void VKAPI_CALL Capture_vkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                                                   const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo);

// Per present, lives in vkQueuePresentKHR's frame until the down-call returned.
struct FlipMeteringPresentTag {
    ce::vulkan_present_metering_policy::SetPresentConfigNV node = {};
    ce::vulkan_flip_metering_bridge::PresentDecision decision;
    bool attached = false;
};

// Decides, before any present-mode handling, whether this present belongs to a metered batch.
void PlanFlipMeteringPresent(const DeviceDispatch* disp, SwapchainData* sd, const VkPresentInfoKHR* pPresentInfo,
                             FlipMeteringPresentTag& tag);

// Prepends VkSetPresentConfigNV to the chain of a metered batch's first present. Returns true when it did.
bool AttachFlipMeteringNode(FlipMeteringPresentTag& tag, const void*& finalPresentChain);

// Records the driver's feedback for an attached node, which the D3D12 generator reads back.
void NoteFlipMeteringPresentResult(SwapchainData* sd, const FlipMeteringPresentTag& tag, VkResult result);

// vkDestroySwapchainKHR: the driver's answer about this swapchain no longer stands.
void RetireFlipMeteringSwapchain(VkSwapchainKHR swapchain);

// The application's own VK_NV_present_metering node under vertical-blank-paced presentation: one line
// per state (activation, multiplier change, chain shape change), never per present.
void ReportApplicationPresentMetering(SwapchainData* sd, const VkPresentInfoKHR* pPresentInfo,
                                      const ce::vulkan_present_metering_policy::ChainScan& scan);

namespace ce::vulkan_flip_metering_bridge {
// A present on this process's bridge carried or continued a metered batch since the last unmetered one.
bool IsMeteringEngaged();
}  // namespace ce::vulkan_flip_metering_bridge

// Hook DLL entry points (resolved by hook/present/vulkan_layer_metering_bridge.h).
// A D3D12 translation device with VK_NV_present_metering enabled by CE exists in this process.
extern "C" __declspec(dllexport) BOOL CEVulkanLayerFlipMeteringBridgeAvailable(void);
// Binds a frame-generation batch size to the D3D12 frame id its presents will carry.
extern "C" __declspec(dllexport) void CEVulkanLayerPublishFlipMeteringBatch(UINT64 frameId, UINT32 framesPerBatch);
// The driver's presentConfigFeedback for the latest metered batch on a live swapchain (0 = metered or not
// yet asked).
extern "C" __declspec(dllexport) UINT32 CEVulkanLayerFlipMeteringFeedback(void);
