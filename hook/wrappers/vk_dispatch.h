/**
 * Custom Vulkan Dispatch Table
 *
 * Replaces Volk with direct GetProcAddress + vkGetInstanceProcAddr loading.
 * This eliminates the Volk dependency while providing the same functionality.
 */

#pragma once

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

namespace VkDispatch {

// ============================================================================
// Global Function Pointers (replaces Volk's global symbols)
// ============================================================================

// Core instance/loader functions
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
extern PFN_vkCreateInstance vkCreateInstance;
extern PFN_vkEnumerateInstanceExtensionProperties
    vkEnumerateInstanceExtensionProperties;
extern PFN_vkEnumerateInstanceLayerProperties
    vkEnumerateInstanceLayerProperties;
extern PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion;

// Instance-level functions
extern PFN_vkDestroyInstance vkDestroyInstance;
extern PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
extern PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
extern PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
extern PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
extern PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
extern PFN_vkGetPhysicalDeviceQueueFamilyProperties
    vkGetPhysicalDeviceQueueFamilyProperties;
extern PFN_vkGetPhysicalDeviceMemoryProperties
    vkGetPhysicalDeviceMemoryProperties;
extern PFN_vkCreateDevice vkCreateDevice;
extern PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
extern PFN_vkEnumerateDeviceExtensionProperties
    vkEnumerateDeviceExtensionProperties;

// Surface/swapchain (KHR extensions)
extern PFN_vkGetPhysicalDeviceSurfaceSupportKHR
    vkGetPhysicalDeviceSurfaceSupportKHR;
extern PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
extern PFN_vkGetPhysicalDeviceSurfaceFormatsKHR
    vkGetPhysicalDeviceSurfaceFormatsKHR;
extern PFN_vkGetPhysicalDeviceSurfacePresentModesKHR
    vkGetPhysicalDeviceSurfacePresentModesKHR;
extern PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
extern PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR;

// Device-level functions (populated per-device)
extern PFN_vkDestroyDevice vkDestroyDevice;
extern PFN_vkGetDeviceQueue vkGetDeviceQueue;
extern PFN_vkQueueSubmit vkQueueSubmit;
extern PFN_vkQueueWaitIdle vkQueueWaitIdle;
extern PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
extern PFN_vkAllocateMemory vkAllocateMemory;
extern PFN_vkFreeMemory vkFreeMemory;
extern PFN_vkMapMemory vkMapMemory;
extern PFN_vkUnmapMemory vkUnmapMemory;
extern PFN_vkBindBufferMemory vkBindBufferMemory;
extern PFN_vkBindImageMemory vkBindImageMemory;
extern PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
extern PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;

// Command buffers
extern PFN_vkCreateCommandPool vkCreateCommandPool;
extern PFN_vkDestroyCommandPool vkDestroyCommandPool;
extern PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
extern PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
extern PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
extern PFN_vkEndCommandBuffer vkEndCommandBuffer;
extern PFN_vkResetCommandBuffer vkResetCommandBuffer;
extern PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
extern PFN_vkCmdCopyImage vkCmdCopyImage;
extern PFN_vkCmdBlitImage vkCmdBlitImage;
extern PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;

// Images
extern PFN_vkCreateImage vkCreateImage;
extern PFN_vkDestroyImage vkDestroyImage;
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkDestroyImageView vkDestroyImageView;

// Buffers
extern PFN_vkCreateBuffer vkCreateBuffer;
extern PFN_vkDestroyBuffer vkDestroyBuffer;

// Synchronization
extern PFN_vkCreateSemaphore vkCreateSemaphore;
extern PFN_vkDestroySemaphore vkDestroySemaphore;
extern PFN_vkCreateFence vkCreateFence;
extern PFN_vkDestroyFence vkDestroyFence;
extern PFN_vkWaitForFences vkWaitForFences;
extern PFN_vkResetFences vkResetFences;

// Swapchain
extern PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
extern PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
extern PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
extern PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
extern PFN_vkQueuePresentKHR vkQueuePresentKHR;

// Samplers
extern PFN_vkCreateSampler vkCreateSampler;
extern PFN_vkDestroySampler vkDestroySampler;

// Descriptor sets
extern PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
extern PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
extern PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
extern PFN_vkFreeDescriptorSets vkFreeDescriptorSets;
extern PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
extern PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
extern PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;

// Pipeline
extern PFN_vkCreateRenderPass vkCreateRenderPass;
extern PFN_vkDestroyRenderPass vkDestroyRenderPass;
extern PFN_vkCreateFramebuffer vkCreateFramebuffer;
extern PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
extern PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
extern PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
extern PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
extern PFN_vkCreateComputePipelines vkCreateComputePipelines;
extern PFN_vkDestroyPipeline vkDestroyPipeline;
extern PFN_vkCreateShaderModule vkCreateShaderModule;
extern PFN_vkDestroyShaderModule vkDestroyShaderModule;

// Draw commands
extern PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
extern PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
extern PFN_vkCmdBindPipeline vkCmdBindPipeline;
extern PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
extern PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
extern PFN_vkCmdBindIndexBuffer vkCmdBindIndexBuffer;
extern PFN_vkCmdDraw vkCmdDraw;
extern PFN_vkCmdDrawIndexed vkCmdDrawIndexed;
extern PFN_vkCmdSetViewport vkCmdSetViewport;
extern PFN_vkCmdSetScissor vkCmdSetScissor;
extern PFN_vkCmdPushConstants vkCmdPushConstants;

// Win32 external memory/semaphore extensions
extern PFN_vkGetMemoryWin32HandleKHR vkGetMemoryWin32HandleKHR;
extern PFN_vkGetSemaphoreWin32HandleKHR vkGetSemaphoreWin32HandleKHR;
extern PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR;

// Timeline semaphores
extern PFN_vkWaitSemaphoresKHR vkWaitSemaphoresKHR;
extern PFN_vkSignalSemaphoreKHR vkSignalSemaphoreKHR;
extern PFN_vkGetSemaphoreCounterValueKHR vkGetSemaphoreCounterValueKHR;

// ============================================================================
// Initialization Functions (replaces volkInitialize, volkLoadInstance, etc.)
// ============================================================================

/**
 * Initialize Vulkan loader - replaces volkInitialize()
 * Loads vulkan-1.dll and gets vkGetInstanceProcAddr
 */
VkResult Initialize();

/**
 * Load instance-level functions - replaces volkLoadInstance()
 */
void LoadInstance(VkInstance instance);

/**
 * Load device-level functions - replaces volkLoadDevice()
 */
void LoadDevice(VkDevice device);

/**
 * Cleanup resources
 */
void Shutdown();

/**
 * Check if initialized
 */
bool IsInitialized();

} // namespace VkDispatch

// ============================================================================
// Compatibility Macros (makes existing volk code work with minimal changes)
// When USE_VKDISPATCH is defined, replace volk symbols with our dispatch
// symbols
// ============================================================================

#ifdef USE_VKDISPATCH

// Replace volkInitialize/volkLoadInstance/volkLoadDevice
#define volkInitialize VkDispatch::Initialize
#define volkLoadInstance VkDispatch::LoadInstance
#define volkLoadDevice VkDispatch::LoadDevice

// Map Vulkan functions to our dispatch table via "using" (C++ way)
// This allows existing code using bare vkXxx names to find our dispatch
// versions

using VkDispatch::vkAcquireNextImageKHR;
using VkDispatch::vkAllocateCommandBuffers;
using VkDispatch::vkAllocateDescriptorSets;
using VkDispatch::vkAllocateMemory;
using VkDispatch::vkBeginCommandBuffer;
using VkDispatch::vkBindBufferMemory;
using VkDispatch::vkBindImageMemory;
using VkDispatch::vkCmdBeginRenderPass;
using VkDispatch::vkCmdBindDescriptorSets;
using VkDispatch::vkCmdBindIndexBuffer;
using VkDispatch::vkCmdBindPipeline;
using VkDispatch::vkCmdBindVertexBuffers;
using VkDispatch::vkCmdBlitImage;
using VkDispatch::vkCmdCopyBufferToImage;
using VkDispatch::vkCmdCopyImage;
using VkDispatch::vkCmdDraw;
using VkDispatch::vkCmdDrawIndexed;
using VkDispatch::vkCmdEndRenderPass;
using VkDispatch::vkCmdPipelineBarrier;
using VkDispatch::vkCmdPushConstants;
using VkDispatch::vkCmdSetScissor;
using VkDispatch::vkCmdSetViewport;
using VkDispatch::vkCreateBuffer;
using VkDispatch::vkCreateCommandPool;
using VkDispatch::vkCreateComputePipelines;
using VkDispatch::vkCreateDescriptorPool;
using VkDispatch::vkCreateDescriptorSetLayout;
using VkDispatch::vkCreateDevice;
using VkDispatch::vkCreateFence;
using VkDispatch::vkCreateFramebuffer;
using VkDispatch::vkCreateGraphicsPipelines;
using VkDispatch::vkCreateImage;
using VkDispatch::vkCreateImageView;
using VkDispatch::vkCreateInstance;
using VkDispatch::vkCreatePipelineLayout;
using VkDispatch::vkCreateRenderPass;
using VkDispatch::vkCreateSampler;
using VkDispatch::vkCreateSemaphore;
using VkDispatch::vkCreateShaderModule;
using VkDispatch::vkCreateSwapchainKHR;
using VkDispatch::vkCreateWin32SurfaceKHR;
using VkDispatch::vkDestroyBuffer;
using VkDispatch::vkDestroyCommandPool;
using VkDispatch::vkDestroyDescriptorPool;
using VkDispatch::vkDestroyDescriptorSetLayout;
using VkDispatch::vkDestroyDevice;
using VkDispatch::vkDestroyFence;
using VkDispatch::vkDestroyFramebuffer;
using VkDispatch::vkDestroyImage;
using VkDispatch::vkDestroyImageView;
using VkDispatch::vkDestroyInstance;
using VkDispatch::vkDestroyPipeline;
using VkDispatch::vkDestroyPipelineLayout;
using VkDispatch::vkDestroyRenderPass;
using VkDispatch::vkDestroySampler;
using VkDispatch::vkDestroySemaphore;
using VkDispatch::vkDestroyShaderModule;
using VkDispatch::vkDestroySurfaceKHR;
using VkDispatch::vkDestroySwapchainKHR;
using VkDispatch::vkDeviceWaitIdle;
using VkDispatch::vkEndCommandBuffer;
using VkDispatch::vkEnumerateDeviceExtensionProperties;
using VkDispatch::vkEnumeratePhysicalDevices;
using VkDispatch::vkFreeCommandBuffers;
using VkDispatch::vkFreeDescriptorSets;
using VkDispatch::vkFreeMemory;
using VkDispatch::vkGetBufferMemoryRequirements;
using VkDispatch::vkGetDeviceProcAddr;
using VkDispatch::vkGetDeviceQueue;
using VkDispatch::vkGetImageMemoryRequirements;
using VkDispatch::vkGetInstanceProcAddr;
using VkDispatch::vkGetMemoryWin32HandleKHR;
using VkDispatch::vkGetPhysicalDeviceFeatures;
using VkDispatch::vkGetPhysicalDeviceFeatures2;
using VkDispatch::vkGetPhysicalDeviceMemoryProperties;
using VkDispatch::vkGetPhysicalDeviceProperties;
using VkDispatch::vkGetPhysicalDeviceProperties2;
using VkDispatch::vkGetPhysicalDeviceQueueFamilyProperties;
using VkDispatch::vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
using VkDispatch::vkGetPhysicalDeviceSurfaceFormatsKHR;
using VkDispatch::vkGetPhysicalDeviceSurfacePresentModesKHR;
using VkDispatch::vkGetPhysicalDeviceSurfaceSupportKHR;
using VkDispatch::vkGetSemaphoreCounterValueKHR;
using VkDispatch::vkGetSemaphoreWin32HandleKHR;
using VkDispatch::vkGetSwapchainImagesKHR;
using VkDispatch::vkImportSemaphoreWin32HandleKHR;
using VkDispatch::vkMapMemory;
using VkDispatch::vkQueuePresentKHR;
using VkDispatch::vkQueueSubmit;
using VkDispatch::vkQueueWaitIdle;
using VkDispatch::vkResetCommandBuffer;
using VkDispatch::vkResetFences;
using VkDispatch::vkSignalSemaphoreKHR;
using VkDispatch::vkUnmapMemory;
using VkDispatch::vkUpdateDescriptorSets;
using VkDispatch::vkWaitForFences;
using VkDispatch::vkWaitSemaphoresKHR;

#endif // USE_VKDISPATCH
