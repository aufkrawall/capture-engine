/**
 * Vulkan Layer - Core Header
 * 
 * Defines dispatch tables and layer infrastructure for overlay and zero-copy capture.
 */

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>

// ============================================================================
// Layer Identification
// ============================================================================

#define LAYER_NAME "VK_LAYER_CAPTURE_overlay"
#define LAYER_DESCRIPTION "Capture Overlay and Zero-Copy Capture Layer"
#define LAYER_IMPLEMENTATION_VERSION 1
#define LAYER_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, 0)

// ============================================================================
// Instance Dispatch Table
// ============================================================================

struct InstanceDispatch {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkCreateDevice CreateDevice;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    
    // Surface functions
    PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
    
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
#endif
};

// ============================================================================
// Device Dispatch Table
// ============================================================================

struct DeviceDispatch {
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    InstanceDispatch* instanceDispatch;
    
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkDestroyDevice DestroyDevice;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    
    // Memory
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkMapMemory MapMemory;
    PFN_vkUnmapMemory UnmapMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkBindImageMemory BindImageMemory;
    
    // Buffers and Images
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    
    // Image Views
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    
    // Samplers (for AF/mip override)
    PFN_vkCreateSampler CreateSampler;
    PFN_vkDestroySampler DestroySampler;
    
    // Framebuffers and Render Passes
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkDestroyFramebuffer DestroyFramebuffer;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkDestroyRenderPass DestroyRenderPass;
    
    // Command Pools and Buffers
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    
    // Command buffer recording
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdDrawIndexed CmdDrawIndexed;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    
    // Synchronization
    PFN_vkCreateFence CreateFence;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkCreateSemaphore CreateSemaphore;
    PFN_vkDestroySemaphore DestroySemaphore;
    
    // Swapchain
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    
    // Descriptors
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
    PFN_vkCreateDescriptorPool CreateDescriptorPool;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
    PFN_vkFreeDescriptorSets FreeDescriptorSets;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
    
    // Pipelines
    PFN_vkCreatePipelineLayout CreatePipelineLayout;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkDestroyPipeline DestroyPipeline;
    PFN_vkCreateShaderModule CreateShaderModule;
    PFN_vkDestroyShaderModule DestroyShaderModule;
    
    // External memory (for zero-copy capture)
#ifdef VK_KHR_external_memory_win32
    PFN_vkGetMemoryWin32HandleKHR GetMemoryWin32HandleKHR;
#endif
};

// ============================================================================
// Swapchain Tracking
// ============================================================================

struct SwapchainData {
    VkSwapchainKHR swapchain;
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t imageCount;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    
    // Overlay resources
    VkRenderPass overlayRenderPass;
    std::vector<VkFramebuffer> overlayFramebuffers;
    VkCommandPool overlayCommandPool;
    std::vector<VkCommandBuffer> overlayCommandBuffers;
    
    // Capture resources
    std::vector<VkFence> captureFences;
    std::atomic<bool> captureEnabled;
    
    // Current frame state
    uint32_t currentImageIndex;
};

// ============================================================================
// Layer State Manager
// ============================================================================

class VulkanLayerState {
public:
    static VulkanLayerState& Get() {
        static VulkanLayerState instance;
        return instance;
    }
    
    // Instance management
    void RegisterInstance(VkInstance instance, InstanceDispatch* dispatch);
    void UnregisterInstance(VkInstance instance);
    InstanceDispatch* GetInstanceDispatch(VkInstance instance);
    
    // Device management
    void RegisterDevice(VkDevice device, DeviceDispatch* dispatch);
    void UnregisterDevice(VkDevice device);
    DeviceDispatch* GetDeviceDispatch(VkDevice device);
    
    // Queue management
    void RegisterQueue(VkQueue queue, VkDevice device);
    DeviceDispatch* GetDeviceFromQueue(VkQueue queue);
    
    // Swapchain management
    void RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data);
    void UnregisterSwapchain(VkSwapchainKHR swapchain);
    SwapchainData* GetSwapchainData(VkSwapchainKHR swapchain);
    
    // Configuration
    bool IsAppWhitelisted(const char* appName);
    bool IsOverlayEnabled() const { return m_OverlayEnabled; }
    bool IsCaptureEnabled() const { return m_CaptureEnabled; }
    
    // AF/Mip override config
    uint32_t GetMaxAnisotropy() const { return m_MaxAnisotropy; }
    float GetMipLodBias() const { return m_MipLodBias; }
    
private:
    VulkanLayerState();
    
    std::mutex m_Lock;
    std::unordered_map<VkInstance, InstanceDispatch*> m_Instances;
    std::unordered_map<VkDevice, DeviceDispatch*> m_Devices;
    std::unordered_map<VkQueue, VkDevice> m_Queues;
    std::unordered_map<VkSwapchainKHR, SwapchainData*> m_Swapchains;
    
    bool m_OverlayEnabled;
    bool m_CaptureEnabled;
    uint32_t m_MaxAnisotropy;
    float m_MipLodBias;
};

// ============================================================================
// Layer Entry Points (exported)
// ============================================================================

extern "C" {
    // Loader negotiation
    VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
        VkNegotiateLayerInterface* pVersionStruct);
    
    // Instance proc addr
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Capture_vkGetInstanceProcAddr(
        VkInstance instance, const char* pName);
    
    // Device proc addr
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Capture_vkGetDeviceProcAddr(
        VkDevice device, const char* pName);
    
    // Layer enumeration
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumerateInstanceLayerProperties(
        uint32_t* pPropertyCount, VkLayerProperties* pProperties);
    
    VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumerateDeviceLayerProperties(
        VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties);
}

// ============================================================================
// Wrapped Vulkan Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages);

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex);
