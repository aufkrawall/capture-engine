/**
 * VK_LAYER_CE_overlay - Vulkan Hook Implementations
 * 
 * Intercepted Vulkan functions for overlay and capture.
 */

#include "layer_main.h"
#include <vector>
#include <cstring>
#include "../common/fps_limiter.h"

// Forward declarations from loader chain
static PFN_vkGetInstanceProcAddr g_fpNextGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr g_fpNextGetDeviceProcAddr = nullptr;

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
static std::unordered_map<VkQueue, uint32_t> g_QueueFamilyMap;

// vkGetDeviceQueue
void VKAPI_CALL vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue)
{
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp || !disp->GetDeviceQueue) {
        return;
    }

    disp->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_QueueMapMutex);
        g_QueueToDevice[*pQueue] = device;
        g_QueueFamilyMap[*pQueue] = queueFamilyIndex;
    }
}

// Get queue family index
uint32_t GetQueueFamilyIndex(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_QueueMapMutex);
    auto it = g_QueueFamilyMap.find(queue);
    if (it != g_QueueFamilyMap.end()) {
        return it->second;
    }
    return VK_QUEUE_FAMILY_IGNORED;
}

// vkCreateInstance
VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    // Get the layer chain info
    VkLayerInstanceCreateInfo* chainInfo = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chainInfo && 
           !(chainInfo->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && 
             chainInfo->function == VK_LAYER_LINK_INFO)) {
        chainInfo = (VkLayerInstanceCreateInfo*)chainInfo->pNext;
    }
    
    if (!chainInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Get next layer's vkGetInstanceProcAddr
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = 
        chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    
    // Advance the link info for the next layer
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
    
    // Get vkCreateInstance from next layer
    PFN_vkCreateInstance fpCreateInstance = 
        (PFN_vkCreateInstance)fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!fpCreateInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // If whitelisted, try to enable physical device properties 2 extension
    std::vector<const char*> enabledExtensions;
    VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
    
    if (g_LayerState.whitelisted) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            enabledExtensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }
        
        bool hasProp2 = false;
        for (const auto& ext : enabledExtensions) {
            if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) {
                hasProp2 = true;
                break;
            }
        }
        
        if (!hasProp2) {
            enabledExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
            modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
            modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
            LayerLog("Layer: Adding instance extension %s", VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }
    }

    // Call down the chain
    VkResult result = fpCreateInstance(&modifiedCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        // Fallback if our extension caused failure (unlikely for Prop2)
        if (g_LayerState.whitelisted && modifiedCreateInfo.enabledExtensionCount > pCreateInfo->enabledExtensionCount) {
             LayerLog("Layer: vkCreateInstance failed with added extensions, retrying original");
             result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
        }
        if (result != VK_SUCCESS) return result;
    }
    
    // Create dispatch table for this instance
    VkInstance instance = *pInstance;
    CEInstanceDispatch dispatch = {};
    dispatch.instance = instance;
    dispatch.GetInstanceProcAddr = fpGetInstanceProcAddr;
    
    // Load instance functions
    #define LOAD_INSTANCE_FUNC(name) \
        dispatch.name = (PFN_vk##name)fpGetInstanceProcAddr(instance, "vk" #name)
    
    LOAD_INSTANCE_FUNC(DestroyInstance);
    LOAD_INSTANCE_FUNC(EnumeratePhysicalDevices);
    LOAD_INSTANCE_FUNC(EnumerateDeviceExtensionProperties);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceProperties);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceProperties2);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FUNC(CreateDevice);
    LOAD_INSTANCE_FUNC(DestroySurfaceKHR);
    LOAD_INSTANCE_FUNC(CreateWin32SurfaceKHR);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE_FUNC(GetPhysicalDeviceSurfacePresentModesKHR);
    
    #undef LOAD_INSTANCE_FUNC
    
    // Store dispatch table
    {
        std::lock_guard<std::mutex> lock(g_InstanceMapMutex);
        g_InstanceMap[instance] = dispatch;
    }
    
    LayerLog("Layer: vkCreateInstance succeeded (instance=%p, whitelisted=%d)", 
             instance, g_LayerState.whitelisted);
    
    return VK_SUCCESS;
}

// vkDestroyInstance
void VKAPI_CALL vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
{
    CEInstanceDispatch* disp = GetInstanceDispatch(instance);
    if (disp && disp->DestroyInstance) {
        disp->DestroyInstance(instance, pAllocator);
    }
    
    // Remove from map
    {
        std::lock_guard<std::mutex> lock(g_InstanceMapMutex);
        g_InstanceMap.erase(instance);
    }
    
    LayerLog("Layer: vkDestroyInstance (instance=%p)", instance);
}

// vkCreateDevice
VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    // Get the layer chain info
    VkLayerDeviceCreateInfo* chainInfo = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chainInfo && 
           !(chainInfo->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && 
             chainInfo->function == VK_LAYER_LINK_INFO)) {
        chainInfo = (VkLayerDeviceCreateInfo*)chainInfo->pNext;
    }
    
    if (!chainInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Get next layer's proc addrs
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = 
        chainInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = 
        chainInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    
    // Advance the link info
    chainInfo->u.pLayerInfo = chainInfo->u.pLayerInfo->pNext;
    
    // Find the instance for this physical device
    VkInstance instance = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_InstanceMapMutex);
        for (auto& pair : g_InstanceMap) {
            instance = pair.first;
            break; // Use first instance (should only be one typically)
        }
    }
    
    // Get vkCreateDevice from next layer
    PFN_vkCreateDevice fpCreateDevice = 
        (PFN_vkCreateDevice)fpGetInstanceProcAddr(instance, "vkCreateDevice");
    if (!fpCreateDevice) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // If whitelisted, try to enable external memory extensions for zero-copy capture
    std::vector<const char*> enabledExtensions;
    VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;
    
    if (g_LayerState.whitelisted) {
        // Copy existing extensions
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            enabledExtensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }
        
        // Extensions we want to enable for zero-copy capture
        const char* externalMemExtensions[] = {
            "VK_KHR_external_memory",
            "VK_KHR_external_memory_win32",
            "VK_KHR_external_semaphore",
            "VK_KHR_external_semaphore_win32",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_get_memory_requirements2",
        };
        
        // Check if extensions are already enabled, if not add them
        for (const char* ext : externalMemExtensions) {
            bool alreadyEnabled = false;
            for (const auto& existing : enabledExtensions) {
                if (strcmp(existing, ext) == 0) {
                    alreadyEnabled = true;
                    break;
                }
            }
            if (!alreadyEnabled) {
                enabledExtensions.push_back(ext);
                LayerLog("Layer: Adding extension %s for zero-copy capture", ext);
            }
        }
        
        modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
        modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
    }
    
    // Call down the chain with potentially modified extensions
    VkResult result = fpCreateDevice(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        // If failed with our extensions, try without them
        if (g_LayerState.whitelisted && modifiedCreateInfo.enabledExtensionCount > pCreateInfo->enabledExtensionCount) {
            LayerLog("Layer: vkCreateDevice failed with external memory extensions, retrying without");
            result = fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
        }
        if (result != VK_SUCCESS) {
            return result;
        }
    }
    
    // Create dispatch table for this device
    VkDevice device = *pDevice;
    CEDeviceDispatch dispatch = {};
    dispatch.device = device;
    dispatch.physicalDevice = physicalDevice;
    dispatch.instance = instance;
    dispatch.GetDeviceProcAddr = fpGetDeviceProcAddr;
    
    // Load device functions
    #define LOAD_DEVICE_FUNC(name) \
        dispatch.name = (PFN_vk##name)fpGetDeviceProcAddr(device, "vk" #name)
    
    LOAD_DEVICE_FUNC(DestroyDevice);
    LOAD_DEVICE_FUNC(GetDeviceQueue);
    LOAD_DEVICE_FUNC(QueueSubmit);
    LOAD_DEVICE_FUNC(QueueSubmit2);
    LOAD_DEVICE_FUNC(QueuePresentKHR);
    LOAD_DEVICE_FUNC(QueueWaitIdle);
    LOAD_DEVICE_FUNC(DeviceWaitIdle);
    LOAD_DEVICE_FUNC(CreateSwapchainKHR);
    LOAD_DEVICE_FUNC(DestroySwapchainKHR);
    LOAD_DEVICE_FUNC(GetSwapchainImagesKHR);
    LOAD_DEVICE_FUNC(AcquireNextImageKHR);
    LOAD_DEVICE_FUNC(CreateImageView);
    LOAD_DEVICE_FUNC(DestroyImageView);
    LOAD_DEVICE_FUNC(CreateRenderPass);
    LOAD_DEVICE_FUNC(DestroyRenderPass);
    LOAD_DEVICE_FUNC(CreateFramebuffer);
    LOAD_DEVICE_FUNC(DestroyFramebuffer);
    LOAD_DEVICE_FUNC(CreateCommandPool);
    LOAD_DEVICE_FUNC(DestroyCommandPool);
    LOAD_DEVICE_FUNC(AllocateCommandBuffers);
    LOAD_DEVICE_FUNC(FreeCommandBuffers);
    LOAD_DEVICE_FUNC(BeginCommandBuffer);
    LOAD_DEVICE_FUNC(EndCommandBuffer);
    LOAD_DEVICE_FUNC(CmdBeginRenderPass);
    LOAD_DEVICE_FUNC(CmdEndRenderPass);
    LOAD_DEVICE_FUNC(CmdPipelineBarrier);
    LOAD_DEVICE_FUNC(CmdCopyImage);
    LOAD_DEVICE_FUNC(CmdClearAttachments);
    LOAD_DEVICE_FUNC(CreateSampler);
    LOAD_DEVICE_FUNC(DestroySampler);
    LOAD_DEVICE_FUNC(CreateDescriptorPool);
    LOAD_DEVICE_FUNC(DestroyDescriptorPool);
    LOAD_DEVICE_FUNC(CreateFence);
    LOAD_DEVICE_FUNC(DestroyFence);
    LOAD_DEVICE_FUNC(WaitForFences);
    LOAD_DEVICE_FUNC(ResetFences);
    LOAD_DEVICE_FUNC(CreateSemaphore);
    LOAD_DEVICE_FUNC(DestroySemaphore);
    LOAD_DEVICE_FUNC(AllocateMemory);
    LOAD_DEVICE_FUNC(FreeMemory);
    LOAD_DEVICE_FUNC(CreateImage);
    LOAD_DEVICE_FUNC(DestroyImage);
    LOAD_DEVICE_FUNC(GetImageMemoryRequirements);
    LOAD_DEVICE_FUNC(BindImageMemory);
    // External memory (may be null if not supported)
    LOAD_DEVICE_FUNC(GetMemoryWin32HandleKHR);
    LOAD_DEVICE_FUNC(GetSemaphoreWin32HandleKHR);
    
    #undef LOAD_DEVICE_FUNC
    
    // Store dispatch table
    {
        std::lock_guard<std::mutex> lock(g_DeviceMapMutex);
        g_DeviceMap[device] = dispatch;
    }
    
    LayerLog("Layer: vkCreateDevice succeeded (device=%p)", device);

    // Query Device LUID (Critical for Multi-GPU Shared Resources)
    if (g_LayerState.whitelisted && instance != VK_NULL_HANDLE) {
        VkPhysicalDeviceIDPropertiesKHR idProps = {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR;
        
        VkPhysicalDeviceProperties2KHR props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2_KHR;
        props2.pNext = &idProps;

        // Try to get GetPhysicalDeviceProperties2 from instance dispatch
        CEInstanceDispatch* instDisp = GetInstanceDispatch(instance);
        if (instDisp && instDisp->GetPhysicalDeviceProperties2) {
             instDisp->GetPhysicalDeviceProperties2(physicalDevice, &props2);
             
             if (idProps.deviceLUIDValid) {
                  // Propagate LUID to capture engine
                  LUID luid;
                  memcpy(&luid, idProps.deviceLUID, sizeof(LUID));
                  LayerLog("Layer: Device LUID: %08x-%08x", luid.HighPart, luid.LowPart);
                  LayerIPC_SetLUID(luid.LowPart, luid.HighPart);
             } else {
                  LayerLog("Layer: Device LUID not valid");
             }
        }
    }
    
    return VK_SUCCESS;
}

// vkDestroyDevice
void VKAPI_CALL vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    // Cleanup overlay and capture resources
    if (g_LayerState.whitelisted) {
        CleanupOverlay(device);
        CleanupCapture(device);
    }
    
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (disp && disp->DestroyDevice) {
        disp->DestroyDevice(device, pAllocator);
    }
    
    // Remove queues for this device
    {
        std::lock_guard<std::mutex> lock(g_QueueMapMutex);
        for (auto it = g_QueueToDevice.begin(); it != g_QueueToDevice.end();) {
            if (it->second == device) {
                it = g_QueueToDevice.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Remove from map
    {
        std::lock_guard<std::mutex> lock(g_DeviceMapMutex);
        g_DeviceMap.erase(device);
    }
    
    LayerLog("Layer: vkDestroyDevice (device=%p)", device);
}

// vkCreateSwapchainKHR
VkResult VKAPI_CALL vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp || !disp->CreateSwapchainKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Ensure swapchain images can be used as transfer source for capture
    VkSwapchainCreateInfoKHR modifiedInfo = *pCreateInfo;
    modifiedInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    
    // Call down the chain
    VkResult result = disp->CreateSwapchainKHR(device, &modifiedInfo, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) {
        return result;
    }
    
    VkSwapchainKHR swapchain = *pSwapchain;
    
    // Get swapchain images
    uint32_t imageCount = 0;
    disp->GetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    disp->GetSwapchainImagesKHR(device, swapchain, &imageCount, images.data());
    
    // Track swapchain
    SwapchainState state = {};
    state.swapchain = swapchain;
    state.device = device;
    state.format = pCreateInfo->imageFormat;
    state.extent = pCreateInfo->imageExtent;
    state.imageCount = imageCount;
    state.images = images;
    
    {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        g_SwapchainMap[swapchain] = state;
    }
    
    LayerLog("Layer: vkCreateSwapchainKHR (swapchain=%p, %dx%d, %d images)", 
             swapchain, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, imageCount);
    
    // Initialize overlay and capture for this swapchain
    if (g_LayerState.overlayEnabled) {
        InitializeOverlay(device, swapchain, pCreateInfo->imageFormat, 
                         pCreateInfo->imageExtent, imageCount, images.data());
    }
    if (g_LayerState.captureEnabled) {
        InitializeCapture(device, swapchain, pCreateInfo->imageFormat,
                         pCreateInfo->imageExtent, imageCount);
    }
    
    return VK_SUCCESS;
}

// vkDestroySwapchainKHR
void VKAPI_CALL vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    // Cleanup before destroying
    {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        auto it = g_SwapchainMap.find(swapchain);
        if (it != g_SwapchainMap.end()) {
            if (it->second.overlayInitialized) {
                CleanupOverlay(device);
            }
            if (it->second.captureInitialized) {
                CleanupCapture(device);
            }
            g_SwapchainMap.erase(it);
        }
    }
    
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (disp && disp->DestroySwapchainKHR) {
        disp->DestroySwapchainKHR(device, swapchain, pAllocator);
    }
    
    LayerLog("Layer: vkDestroySwapchainKHR (swapchain=%p)", swapchain);
}

// vkAcquireNextImageKHR - track current image index
VkResult VKAPI_CALL vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex)
{
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp || !disp->AcquireNextImageKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkResult result = disp->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        auto it = g_SwapchainMap.find(swapchain);
        if (it != g_SwapchainMap.end()) {
            it->second.currentImageIndex = *pImageIndex;
        }
    }
    
    return result;
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
        // Can't do anything without device, just chain
        // This shouldn't happen in normal operation
        return VK_ERROR_DEVICE_LOST;
    }
    
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp || !disp->QueuePresentKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    static int presentCount = 0;
    presentCount++;
    
    // Log periodically
    if (presentCount % 600 == 0) {
        LayerLog("Layer: vkQueuePresentKHR (frame %d, queue=%p)", presentCount, queue);
    }

    // Apply FPS Limiter
    g_SharedFpsLimiter.Apply();
    
    // Render overlay and capture before present
    if (g_LayerState.overlayEnabled && pPresentInfo->swapchainCount > 0) {
        uint32_t imageIndex = pPresentInfo->pImageIndices[0];
        
        // Render overlay to swapchain image
        RenderOverlay(device, queue, imageIndex, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    
    if (g_LayerState.captureEnabled && pPresentInfo->swapchainCount > 0) {
        std::lock_guard<std::mutex> lock(g_SwapchainMapMutex);
        auto it = g_SwapchainMap.find(pPresentInfo->pSwapchains[0]);
            if (it->second.currentImageIndex < it->second.images.size()) {
                VkImage srcImage = it->second.images[it->second.currentImageIndex];
                CaptureFrame(device, queue, srcImage, it->second.currentImageIndex);
            }
    }
    
    // Call the real present
    return disp->QueuePresentKHR(queue, pPresentInfo);
}

// vkCreateSampler - override anisotropic filtering
VkResult VKAPI_CALL vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler)
{
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp || !disp->CreateSampler) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // TODO: Read AF settings from config and override pCreateInfo if needed
    // For now, just pass through
    
    return disp->CreateSampler(device, pCreateInfo, pAllocator, pSampler);
}
