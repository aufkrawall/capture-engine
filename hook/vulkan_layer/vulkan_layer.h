/**
 * Vulkan Layer - Global State and Dispatch Management
 */

#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vulkan_instance_registry.h"

struct SharedMemoryLayout;

// Reentrancy guard shared with other hooks
extern thread_local bool g_InPresentHook;

// Dispatch table for instance-level functions
struct InstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr fp_vkGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance fp_vkDestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices fp_vkEnumeratePhysicalDevices = nullptr;
    // A Vulkan 1.1 application may take its physical devices from the device
    // group enumeration only, so both entry points have to feed the ownership
    // map that vkCreateDevice depends on.
    PFN_vkEnumeratePhysicalDeviceGroups fp_vkEnumeratePhysicalDeviceGroups = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroups fp_vkEnumeratePhysicalDeviceGroupsKHR = nullptr;
    PFN_vkGetPhysicalDeviceProperties fp_vkGetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 fp_vkGetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures fp_vkGetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 fp_vkGetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2 fp_vkGetPhysicalDeviceFormatProperties2 = nullptr;
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
    // Graphics queue CE asked for on top of the game's own at device creation,
    // so the overlay has somewhere to submit when the game presents from a
    // non-graphics queue. VK_NULL_HANDLE when no spare queue existed.
    VkQueue overlayQueue = VK_NULL_HANDLE;
    uint32_t overlayQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bool captureInteropEnabled = false;
    bool samplerAnisotropyEnabled = false;
    bool formatFeatureFlags2Available = false;
    bool storageImageReadWithoutFormatAvailable = false;
    bool storageImageWriteWithoutFormatAvailable = false;
    float maxSamplerAnisotropy = 1.0f;
    float maxSamplerLodBias = 0.0f;
    PFN_vkGetDeviceProcAddr fp_vkGetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice fp_vkDestroyDevice = nullptr;
    PFN_vkGetDeviceQueue fp_vkGetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 fp_vkGetDeviceQueue2 = nullptr;
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
    PFN_vkCmdPushConstants fp_vkCmdPushConstants = nullptr;
    PFN_vkCmdSetViewport fp_vkCmdSetViewport = nullptr;
    PFN_vkCmdSetScissor fp_vkCmdSetScissor = nullptr;
    PFN_vkCmdBindVertexBuffers fp_vkCmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer fp_vkCmdBindIndexBuffer = nullptr;
    PFN_vkCmdCopyImage fp_vkCmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer fp_vkCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage fp_vkCmdCopyBufferToImage = nullptr;
    PFN_vkCmdBlitImage fp_vkCmdBlitImage = nullptr;
    PFN_vkCmdBindDescriptorSets fp_vkCmdBindDescriptorSets = nullptr;
    PFN_vkCmdPipelineBarrier fp_vkCmdPipelineBarrier = nullptr;
    PFN_vkCmdClearAttachments fp_vkCmdClearAttachments = nullptr;
    PFN_vkCreateFence fp_vkCreateFence = nullptr;
    PFN_vkDestroyFence fp_vkDestroyFence = nullptr;
    PFN_vkWaitForFences fp_vkWaitForFences = nullptr;
    PFN_vkResetFences fp_vkResetFences = nullptr;
    PFN_vkWaitSemaphores fp_vkWaitSemaphores = nullptr;
    PFN_vkCreateQueryPool fp_vkCreateQueryPool = nullptr;
    PFN_vkDestroyQueryPool fp_vkDestroyQueryPool = nullptr;
    PFN_vkCmdResetQueryPool fp_vkCmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp fp_vkCmdWriteTimestamp = nullptr;
    PFN_vkGetQueryPoolResults fp_vkGetQueryPoolResults = nullptr;
    PFN_vkCreateSemaphore fp_vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore fp_vkDestroySemaphore = nullptr;
    PFN_vkCreateSwapchainKHR fp_vkCreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR fp_vkDestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR fp_vkGetSwapchainImagesKHR = nullptr;
    PFN_vkAcquireNextImageKHR fp_vkAcquireNextImageKHR = nullptr;
    PFN_vkQueuePresentKHR fp_vkQueuePresentKHR = nullptr;
    PFN_vkSetLatencySleepModeNV fp_vkSetLatencySleepModeNV = nullptr;
    PFN_vkLatencySleepNV fp_vkLatencySleepNV = nullptr;
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
    PFN_vkCreateComputePipelines fp_vkCreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline fp_vkDestroyPipeline = nullptr;
    PFN_vkCmdDispatch fp_vkCmdDispatch = nullptr;
    PFN_vkCreateShaderModule fp_vkCreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule fp_vkDestroyShaderModule = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
    PFN_vkGetMemoryWin32HandleKHR fp_vkGetMemoryWin32HandleKHR = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR fp_vkGetMemoryWin32HandlePropertiesKHR = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR fp_vkGetSemaphoreWin32HandleKHR = nullptr;
#endif
};

// Prerender limit fence data per device
struct PrerenderFenceData {
    std::vector<VkFence> fences;
    VkDevice device = VK_NULL_HANDLE;
    bool initialized = false;
};

// Swapchain tracking data
struct SwapchainData {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent = {0, 0};
    VkImageUsageFlags imageUsage = 0;
    uint32_t imageCount = 0;
    std::vector<VkImage> images;
    HWND window = nullptr;
    uint32_t currentImageIndex = 0;
    std::atomic<bool> runtimeInitialized{false};
    std::atomic<uint64_t> captureHostGeneration{0};
    std::atomic<uint32_t> lastAcquireThreadId{0};
    std::atomic<uint64_t> lastAcquireTick{0};
    std::atomic<bool> asyncPresentDetected{false};
    // Graphics queue that produced the image consumed by a non-graphics
    // present queue. Learned from the first present wait semaphore and reused
    // for queue-depth control without introducing a cross-engine marker.
    std::atomic<VkQueue> prerenderProducerQueue{VK_NULL_HANDLE};
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
    void UnregisterQueue(VkQueue queue) {
        std::lock_guard<std::recursive_mutex> lock(m_Lock);
        m_Queues.erase(queue);
        m_QueueFamilies.erase(queue);
        m_QueueFlags.erase(queue);
        m_QueueLastSubmitThreadIds.erase(queue);
    }
    DeviceDispatch* GetDeviceFromQueue(VkQueue queue);
    VkDevice GetVkDeviceFromQueue(VkQueue queue);
    uint32_t GetQueueFamilyIndex(VkQueue queue);
    uint32_t GetQueueFlags(VkQueue queue);
    bool QueueSupportsGraphics(VkQueue queue);
    bool QueueSupportsCompute(VkQueue queue);
    bool QueueSupportsTransfer(VkQueue queue);
    // Any graphics-capable queue the game itself fetched on this device. Only
    // used as the last resort for overlay submission, on hardware that exposes
    // a single graphics queue and therefore leaves CE nothing to reserve.
    VkQueue FindGameGraphicsQueue(VkDevice device);
    // The graphics-capable queue the game itself most recently submitted to on
    // this device - i.e. the queue that produced the image the overlay is about
    // to draw over. Appending the overlay there is in-order; anywhere else is a
    // cross-queue wait. CE's own submissions bypass the layer's wrappers, so
    // they never appear here.
    VkQueue FindLastGameGraphicsSubmitQueue(VkDevice device);
    void NoteQueueSubmit(VkQueue queue);
    uint32_t GetLastSubmitThreadId(VkDevice device);
    uint32_t GetQueueLastSubmitThreadId(VkQueue queue);

    // One-shot present-topology learning. Which queue signals the semaphores a
    // present waits on decides whether CE's overlay - a render pass, so always
    // on a graphics queue - inserts a cross-engine round trip into a path that
    // had none. The map is only written while learning is armed, so the steady
    // state costs one relaxed atomic load per submit; a swapchain recreate
    // re-arms it, which is exactly when a game's present topology can change.
    bool IsLearningPresentTopology() const {
        return m_LearnPresentTopology.load(std::memory_order_relaxed);
    }
    void ArmPresentTopologyLearning();
    void NoteSemaphoreDependencies(VkQueue queue, const VkSemaphore* waitSemaphores, uint32_t waitCount,
                                   const VkSemaphore* signalSemaphores, uint32_t signalCount);
    void NoteSemaphoreDependencies2(VkQueue queue, const VkSemaphoreSubmitInfo* waitSemaphores, uint32_t waitCount,
                                    const VkSemaphoreSubmitInfo* signalSemaphores, uint32_t signalCount);
    VkQueue GetSemaphoreSignalQueue(VkSemaphore semaphore);
    VkQueue GetSemaphoreGraphicsProducerQueue(VkSemaphore semaphore);
    void FinishPresentTopologyLearning();

    void RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data);
    void UnregisterSwapchain(VkSwapchainKHR swapchain);
    SwapchainData* GetSwapchainData(VkSwapchainKHR swapchain);

    void RegisterSurface(VkSurfaceKHR surface, HWND window);
    void UnregisterSurface(VkSurfaceKHR surface);
    HWND GetSurfaceWindow(VkSurfaceKHR surface);

    void TrackPhysicalDevice(VkPhysicalDevice pd, VkInstance inst);
    VkInstance GetInstanceFromPhysicalDevice(VkPhysicalDevice pd);

    struct PhysicalDeviceOwner {
        VkInstance instance = VK_NULL_HANDLE;
        ce::vulkan_instance_registry::Resolution resolution = ce::vulkan_instance_registry::Resolution::kNone;
    };
    PhysicalDeviceOwner ResolveInstanceForPhysicalDevice(VkPhysicalDevice pd);

    bool IsOverlayEnabled() const {
        return m_OverlayEnabled;
    }
    bool IsCaptureEnabled() const {
        return m_CaptureEnabled;
    }
    uint32_t GetMaxAnisotropy() const {
        return m_MaxAnisotropy;
    }
    bool IsAnisotropyOverrideActive() const {
        return m_AnisotropyOverrideActive;
    }
    float GetMipLodBias() const {
        return m_MipLodBias;
    }
    bool IsMipBiasOverrideActive() const {
        return m_MipBiasOverrideActive;
    }
    bool IsForceMipBiasClampEnabled() const {
        return m_ForceMipBiasClamp;
    }
    const char* GetMipBiasMode() const {
        return m_MipBiasMode.c_str();
    }
    const char* GetMipMapping() const {
        return m_MipMapping.c_str();
    }
    bool IsAggressiveSamplerOverride() const {
        return m_SamplerOverrideMode == "aggressive";
    }
    const char* GetVsyncMode() const {
        return m_VsyncMode.c_str();
    }
    int32_t GetBackbufferCount() const {
        return m_BackbufferCount;
    }
    float GetPrerenderLimit() const {
        return m_PrerenderLimit;
    }

    void UpdateFromSharedMemory(class IPCClient* ipc);

private:
    VulkanLayerState();
    DeviceDispatch* ResolveDispatchByKey(const void* dispatchableHandle);
    mutable std::recursive_mutex m_Lock;
    std::unordered_map<VkInstance, InstanceDispatch*> m_Instances;
    std::unordered_map<VkDevice, DeviceDispatch*> m_Devices;
    std::unordered_map<VkQueue, VkDevice> m_Queues;
    std::unordered_map<VkQueue, uint32_t> m_QueueFamilies;
    std::unordered_map<VkQueue, uint32_t> m_QueueFlags;
    std::unordered_map<VkSwapchainKHR, SwapchainData*> m_Swapchains;
    std::unordered_map<VkSurfaceKHR, HWND> m_Surfaces;
    // Instance/physical-device ownership, including the loader dispatch-key
    // fallback that answers for handles no enumeration hook of ours produced.
    ce::vulkan_instance_registry::Registry m_InstanceRegistry;
    // A VkQueue carries its VkDevice's dispatch table pointer, but only once
    // the loader's trampoline has stamped it - which happens after the layer
    // chain returned from vkGetDeviceQueue. So the key is recorded per device
    // and consulted at lookup time, never captured from a fresh queue handle.
    std::unordered_map<const void*, VkDevice> m_DevicesByDispatchKey;
    std::unordered_map<VkDevice, uint32_t> m_DeviceLastSubmitThreadIds;
    std::unordered_map<VkQueue, uint32_t> m_QueueLastSubmitThreadIds;
    std::unordered_map<VkDevice, VkQueue> m_DeviceLastGraphicsSubmitQueues;
    std::atomic<bool> m_LearnPresentTopology{true};
    std::unordered_map<VkSemaphore, VkQueue> m_SignalSemaphoreQueues;
    std::unordered_map<VkSemaphore, VkQueue> m_SemaphoreGraphicsProducerQueues;
    std::unordered_map<VkQueue, VkQueue> m_QueueGraphicsProducerQueues;

    // Generation counter to invalidate TLS swapchain caches on unregister
    std::atomic<uint64_t> m_SwapchainGeneration{0};

    bool m_OverlayEnabled;
    bool m_CaptureEnabled;
    uint32_t m_MaxAnisotropy;
    bool m_AnisotropyOverrideActive;
    float m_MipLodBias;
    bool m_MipBiasOverrideActive;
    bool m_ForceMipBiasClamp;
    std::string m_MipBiasMode;
    std::string m_MipMapping;
    std::string m_SamplerOverrideMode;
    std::string m_VsyncMode;
    int32_t m_BackbufferCount;
    float m_PrerenderLimit;
};

class PerformanceMetrics;

// Exported wrapper functions (Capture_ prefixed)
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                                                                  VkPhysicalDevice* pPhysicalDevices);
VKAPI_ATTR VkResult VKAPI_CALL
Capture_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);
VKAPI_ATTR VkResult VKAPI_CALL
Capture_vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                           VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                      const VkDeviceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                    VkQueue* pQueue);
VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                                                     VkQueue* pQueue);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                                     VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit2(VkQueue queue, uint32_t submitCount,
                                                      const VkSubmitInfo2* pSubmits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                                                         const VkSubmitInfo2* pSubmits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(VkDevice device,
                                                            const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkSwapchainKHR* pSwapchain);
VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                               uint32_t* pSwapchainImageCount,
                                                               VkImage* pSwapchainImages);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                             uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                                             uint32_t* pImageIndex);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSampler* pSampler);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkSetLatencySleepModeNV(
    VkDevice device, VkSwapchainKHR swapchain, const VkLatencySleepModeInfoNV* pSleepModeInfo);
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                                                        const VkLatencySleepInfoNV* pSleepInfo);
#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                               const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks* pAllocator,
                                                               VkSurfaceKHR* pSurface);
#endif
}

// Functional entry points defined in other files but needed by hooks
void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace,
                       VkExtent2D extent, VkImageUsageFlags imageUsage,
                       uint32_t imageCount, VkImage* images, HWND window);
void CleanupOverlay(VkDevice device);
bool RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, const VkSemaphore* waitSemaphores,
                   uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore, bool gameSubmitsConcurrently,
                   int32_t* fenceWaitUs = nullptr, int32_t* overlayGpuUs = nullptr);
VkSemaphore GetOverlaySemaphore(VkDevice device, uint32_t imageIndex);
PerformanceMetrics* GetOverlayPerformanceMetrics(VkDevice device);
void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace,
                       VkExtent2D extent,
                       uint32_t imageCount);
bool RepublishCaptureTransportForHost(VkDevice device, VkSwapchainKHR swapchain);
void NoteCaptureSwapchainImagePresented(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);
void RetireCaptureSwapchain(VkDevice device, VkSwapchainKHR swapchain);
void CleanupCapture(VkDevice device);
bool CaptureFrame(VkDevice device, VkSwapchainKHR swapchain, VkQueue queue, VkImage srcImage,
                  const VkSemaphore* waitSemaphores, uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore);
VkSemaphore GetCaptureSemaphore(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageIndex);

bool TakeVulkanScreenshot(struct DeviceDispatch* disp, VkDevice device, VkQueue queue, VkImage srcImage, uint32_t width,
                          uint32_t height, VkFormat format, VkColorSpaceKHR colorSpace,
                          const VkSemaphore* waitSemaphores,
                          uint32_t waitSemaphoreCount, SharedMemoryLayout* sharedMemory, uint64_t requestId);

// Overlay submission queue ownership (layer_overlay_queue.cpp). The overlay is
// a render pass, so it needs a graphics-capable queue even when the game
// presents from a compute-only one; see overlay_submit_queue_policy.h.
struct OverlayQueueReservation {
    bool reserved = false;
    uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t queueIndex = 0;
};

bool BuildOverlayQueueReservation(InstanceDispatch* instanceDispatch, VkPhysicalDevice physicalDevice,
                                  const VkDeviceCreateInfo& createInfo,
                                  std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos,
                                  std::vector<float>& widenedPriorities, OverlayQueueReservation& reservation);
struct OverlaySubmitTarget {
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bool borrowed = false;
    bool valid = false;
};

OverlaySubmitTarget ResolveOverlaySubmitTarget(VkDevice device, DeviceDispatch* disp, VkQueue presentQueue,
                                               uint32_t presentQueueFamily, bool gameSubmitsConcurrently,
                                               bool independentOffscreenWork = false);
void ForgetBorrowedOverlaySubmitQueue(VkDevice device);
VkQueue GetBorrowedOverlaySubmitQueue();
void SetBorrowedOverlaySubmitQueue(VkQueue queue);
bool ShouldSerializeQueueSubmission(VkQueue queue);
void LockBorrowedQueueSubmission();
void UnlockBorrowedQueueSubmission();

// VkQueue is externally synchronized. CE submits on a queue the game owns only
// as the last-resort overlay path (hardware with a single graphics queue), and
// only that one queue pays for the lock; every other submission in the process
// keeps the uncontended path.
class ScopedBorrowedQueueSubmission {
public:
    explicit ScopedBorrowedQueueSubmission(VkQueue queue) : locked_(ShouldSerializeQueueSubmission(queue)) {
        if (locked_) {
            LockBorrowedQueueSubmission();
        }
    }
    ~ScopedBorrowedQueueSubmission() {
        if (locked_) {
            UnlockBorrowedQueueSubmission();
        }
    }
    ScopedBorrowedQueueSubmission(const ScopedBorrowedQueueSubmission&) = delete;
    ScopedBorrowedQueueSubmission& operator=(const ScopedBorrowedQueueSubmission&) = delete;

private:
    bool locked_;
};
