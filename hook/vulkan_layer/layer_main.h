/**
 * VK_LAYER_CE_overlay - CaptureEngine Vulkan Layer
 * 
 * This layer provides:
 * - ImGui overlay rendering
 * - Zero-copy video capture via VK_KHR_external_memory
 * - Anisotropic filtering override
 * 
 * The layer respects the whitelist in config.ini - only activates for whitelisted processes.
 */

#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
// #define VK_NO_PROTOTYPES // Defined in build.py

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <windows.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

// Layer name and description
#define CE_LAYER_NAME "VK_LAYER_CE_overlay"
#define CE_LAYER_DESCRIPTION "CaptureEngine Overlay and Recording Layer"
#define CE_LAYER_IMPL_VERSION 1

// Vulkan Dispatch Classes
// #include "layer_dispatch_table.h" // Removed
#include "../common/ipc_client.h"

// Forward declarations
struct CEInstanceDispatch;
struct CEDeviceDispatch;

// Global IPC Client declaration
extern IPCClient g_IPCClient;

// Layer state
struct CELayerState {
    bool initialized = false;
    bool whitelisted = false;          // Process is in config.ini whitelist
    bool overlayEnabled = true;        // Overlay should be rendered
    bool captureEnabled = false;       // Capture is active
    std::string processName;
    std::string configPath;
};

extern CELayerState g_LayerState;

// Instance dispatch table
struct CEInstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
};

// Device dispatch table
struct CEDeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    // External memory extensions
    PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR GetSemaphoreWin32HandleKHR = nullptr;
};

// Dispatch table management
extern std::mutex g_InstanceMapMutex;
extern std::unordered_map<VkInstance, CEInstanceDispatch> g_InstanceMap;
extern std::mutex g_DeviceMapMutex;
extern std::unordered_map<VkDevice, CEDeviceDispatch> g_DeviceMap;

// Get dispatch tables
CEInstanceDispatch* GetInstanceDispatch(VkInstance instance);
CEDeviceDispatch* GetDeviceDispatch(VkDevice device);

// Whitelist checking
// Whitelist checking
// bool CheckProcessWhitelist(); // Removed - using internal IsProcessWhitelistedFast
// std::string GetConfigPath(); // Removed




// Layer entry points (exported)
extern "C" {
    VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct);
    PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
    PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
}

// Layer-intercepted functions
VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);
VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler);
void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
uint32_t GetQueueFamilyIndex(VkQueue queue);

// Overlay functions
void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent, uint32_t imageCount, VkImage* images);
void CleanupOverlay(VkDevice device);
void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore);

// Capture functions
void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent, uint32_t imageCount);
void CleanupCapture(VkDevice device);
void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex);

// IPC functions
bool LayerIPC_Init();
void LayerIPC_Shutdown();
bool LayerIPC_IsConnected();
void LayerIPC_SetTextures(HANDLE* handles, uint32_t count, uint32_t width, uint32_t height, uint32_t format);
uint32_t VkFormatToDXGI(uint32_t vkFormat);
void LayerIPC_UpdateFrameTiming(uint64_t frameCount, float fps, float avgFps);
bool LayerIPC_ShouldShowOverlay();
bool LayerIPC_IsCaptureRequested();
void LayerIPC_SetCaptureActive(bool active);
void LayerIPC_SetOverlayActive(bool active);
uint32_t LayerIPC_GetWriteIndex();
void LayerIPC_IncrementWriteIndex(uint64_t timestamp);
void LayerIPC_Log(const char* fmt, ...);
void LayerIPC_SetLUID(int32_t low, int32_t high);

// Logging
void LayerLog(const char* fmt, ...);
