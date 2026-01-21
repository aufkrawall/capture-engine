/**
 * Vulkan Layer - Global State and Dispatch Management
 */

#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

// Reentrancy guard shared with other hooks
extern thread_local bool g_InPresentHook;

// Dispatch table for instance-level functions
struct InstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr fp_vkGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance fp_vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices fp_vkEnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties fp_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 fp_vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures fp_vkGetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 fp_vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties fp_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties fp_vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice fp_vkCreateDevice = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties fp_vkEnumerateDeviceExtensionProperties = nullptr;
    PFN_vkDestroySurfaceKHR fp_vkDestroySurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fp_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR fp_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR fp_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkCreateWin32SurfaceKHR fp_vkCreateWin32SurfaceKHR = nullptr;
#endif
};

// Dispatch table for device-level functions
struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr fp_vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice fp_vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue fp_vkGetDeviceQueue = nullptr;
    PFN_vkQueueSubmit fp_vkQueueSubmit = nullptr;
    PFN_vkQueueSubmit2 fp_vkQueueSubmit2 = nullptr;
    PFN_vkQueueSubmit2KHR fp_vkQueueSubmit2KHR = nullptr;
    PFN_vkQueueWaitIdle fp_vkQueueWaitIdle = nullptr;
    PFN_vkDeviceWaitIdle fp_vkDeviceWaitIdle = nullptr;
    PFN_vkAllocateMemory fp_vkAllocateMemory = nullptr;
    PFN_vkFreeMemory fp_vkFreeMemory = nullptr;
    PFN_vkMapMemory fp_vkMapMemory = nullptr;
    PFN_vkUnmapMemory fp_vkUnmapMemory = nullptr;
    PFN_vkBindBufferMemory fp_vkBindBufferMemory = nullptr;
    PFN_vkBindImageMemory fp_vkBindImageMemory = nullptr;
    PFN_vkCreateBuffer fp_vkCreateBuffer = nullptr;
    PFN_vkDestroyBuffer fp_vkDestroyBuffer = nullptr;
    PFN_vkCreateImage fp_vkCreateImage = nullptr;
    PFN_vkDestroyImage fp_vkDestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements fp_vkGetImageMemoryRequirements = nullptr;
    PFN_vkGetBufferMemoryRequirements fp_vkGetBufferMemoryRequirements = nullptr;
    PFN_vkCreateImageView fp_vkCreateImageView = nullptr;
    PFN_vkDestroyImageView fp_vkDestroyImageView = nullptr;
    PFN_vkCreateSampler fp_vkCreateSampler = nullptr;
    PFN_vkDestroySampler fp_vkDestroySampler = nullptr;
    PFN_vkCreateFramebuffer fp_vkCreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer fp_vkDestroyFramebuffer = nullptr;
    PFN_vkCreateRenderPass fp_vkCreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass fp_vkDestroyRenderPass = nullptr;
    PFN_vkCreateCommandPool fp_vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool fp_vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers fp_vkAllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers fp_vkFreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer fp_vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer fp_vkEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer fp_vkResetCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass fp_vkCmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass fp_vkCmdEndRenderPass = nullptr;
    PFN_vkCmdBindPipeline fp_vkCmdBindPipeline = nullptr;
    PFN_vkCmdDraw fp_vkCmdDraw = nullptr;
    PFN_vkCmdDrawIndexed fp_vkCmdDrawIndexed = nullptr;
    PFN_vkCmdCopyImage fp_vkCmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer fp_vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdBlitImage fp_vkCmdBlitImage = nullptr;
    PFN_vkCmdPipelineBarrier fp_vkCmdPipelineBarrier = nullptr;
    PFN_vkCreateFence fp_vkCreateFence = nullptr;
    PFN_vkDestroyFence fp_vkDestroyFence = nullptr;
    PFN_vkWaitForFences fp_vkWaitForFences = nullptr;
    PFN_vkResetFences fp_vkResetFences = nullptr;
    PFN_vkCreateSemaphore fp_vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore fp_vkDestroySemaphore = nullptr;
    PFN_vkCreateSwapchainKHR fp_vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR fp_vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR fp_vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR fp_vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR fp_vkQueuePresentKHR = nullptr;
    PFN_vkCreateDescriptorSetLayout fp_vkCreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout fp_vkDestroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool fp_vkCreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool fp_vkDestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets fp_vkAllocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets fp_vkFreeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets fp_vkUpdateDescriptorSets = nullptr;
    PFN_vkCreatePipelineLayout fp_vkCreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout fp_vkDestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines fp_vkCreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline fp_vkDestroyPipeline = nullptr;
    PFN_vkCreateShaderModule fp_vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule fp_vkDestroyShaderModule = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkGetMemoryWin32HandleKHR fp_vkGetMemoryWin32HandleKHR = nullptr;
#endif
};

// Swapchain tracking data
struct SwapchainData {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    std::vector<VkImage> images;
    uint32_t currentImageIndex = 0;
    bool overlayInitialized = false;
    bool captureEnabled = false;
};

// Singleton state manager
class VulkanLayerState {
public:
    static VulkanLayerState& Get();
    
    void RegisterInstance(VkInstance instance, InstanceDispatch* dispatch);
    void UnregisterInstance(VkInstance instance);
    InstanceDispatch* GetInstanceDispatch(VkInstance instance);
    
    void RegisterDevice(VkDevice device, DeviceDispatch* dispatch);
    void UnregisterDevice(VkDevice device);
    DeviceDispatch* GetDeviceDispatch(VkDevice device);
    
    void RegisterQueue(VkQueue queue, VkDevice device, uint32_t familyIndex);
    void UnregisterQueue(VkQueue queue) { std::lock_guard<std::recursive_mutex> lock(m_Lock); m_Queues.erase(queue); m_QueueFamilies.erase(queue); }
    DeviceDispatch* GetDeviceFromQueue(VkQueue queue);
    uint32_t GetQueueFamilyIndex(VkQueue queue);
    
    void RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data);
    void UnregisterSwapchain(VkSwapchainKHR swapchain);
    SwapchainData* GetSwapchainData(VkSwapchainKHR swapchain);
    
    void RegisterSurface(VkSurfaceKHR surface, HWND window);
    void UnregisterSurface(VkSurfaceKHR surface);
    HWND GetSurfaceWindow(VkSurfaceKHR surface);

    void TrackPhysicalDevice(VkPhysicalDevice pd, VkInstance inst);
    VkInstance GetInstanceFromPhysicalDevice(VkPhysicalDevice pd);

    bool IsOverlayEnabled() const { return m_OverlayEnabled; }
    bool IsCaptureEnabled() const { return m_CaptureEnabled; }
    uint32_t GetMaxAnisotropy() const { return m_MaxAnisotropy; }
    float GetMipLodBias() const { return m_MipLodBias; }

private:
    VulkanLayerState();
    mutable std::recursive_mutex m_Lock;
    std::unordered_map<VkInstance, InstanceDispatch*> m_Instances;
    std::unordered_map<VkDevice, DeviceDispatch*> m_Devices;
    std::unordered_map<VkQueue, VkDevice> m_Queues;
    std::unordered_map<VkQueue, uint32_t> m_QueueFamilies;
    std::unordered_map<VkSwapchainKHR, SwapchainData*> m_Swapchains;
    std::unordered_map<VkSurfaceKHR, HWND> m_Surfaces;
    std::unordered_map<VkPhysicalDevice, VkInstance> m_PhysDevToInstance;
    
    bool m_OverlayEnabled;
    bool m_CaptureEnabled;
    uint32_t m_MaxAnisotropy;
    float m_MipLodBias;
};

// Exported wrapper functions (Capture_ prefixed)
extern "C" {
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
    VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
    VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
    VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
    VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
#endif
}

// Functional entry points defined in other files but needed by hooks
void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent, uint32_t imageCount, VkImage* images, HWND window);
void CleanupOverlay(VkDevice device);
void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore);
void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent, uint32_t imageCount);
void CleanupCapture(VkDevice device);
void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex, VkSemaphore waitSemaphore);
