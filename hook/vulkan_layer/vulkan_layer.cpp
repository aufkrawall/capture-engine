/**
 * Vulkan Layer - Core Implementation
 */

#include "vulkan_layer.h"
#include <cstring>
#include <cstdio>

// ============================================================================
// Layer Logging
// ============================================================================

static void LayerLog(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    // TODO: Integrate with main logging system
    OutputDebugStringA("[VkLayer] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}

// ============================================================================
// VulkanLayerState Implementation
// ============================================================================

VulkanLayerState::VulkanLayerState()
    : m_OverlayEnabled(true)
    , m_CaptureEnabled(true)
    , m_MaxAnisotropy(16)
    , m_MipLodBias(0.0f)
{
    // TODO: Load config from config.ini
    LayerLog("VulkanLayerState initialized");
}

void VulkanLayerState::RegisterInstance(VkInstance instance, InstanceDispatch* dispatch) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Instances[instance] = dispatch;
    LayerLog("Registered instance %p", instance);
}

void VulkanLayerState::UnregisterInstance(VkInstance instance) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    if (it != m_Instances.end()) {
        delete it->second;
        m_Instances.erase(it);
        LayerLog("Unregistered instance %p", instance);
    }
}

InstanceDispatch* VulkanLayerState::GetInstanceDispatch(VkInstance instance) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    return (it != m_Instances.end()) ? it->second : nullptr;
}

void VulkanLayerState::RegisterDevice(VkDevice device, DeviceDispatch* dispatch) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Devices[device] = dispatch;
    LayerLog("Registered device %p", device);
}

void VulkanLayerState::UnregisterDevice(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end()) {
        delete it->second;
        m_Devices.erase(it);
        LayerLog("Unregistered device %p", device);
    }
}

DeviceDispatch* VulkanLayerState::GetDeviceDispatch(VkDevice device) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    return (it != m_Devices.end()) ? it->second : nullptr;
}

void VulkanLayerState::RegisterQueue(VkQueue queue, VkDevice device) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Queues[queue] = device;
}

DeviceDispatch* VulkanLayerState::GetDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        return GetDeviceDispatch(it->second);
    }
    return nullptr;
}

void VulkanLayerState::RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Swapchains[swapchain] = data;
    LayerLog("Registered swapchain %p", swapchain);
}

void VulkanLayerState::UnregisterSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        delete it->second;
        m_Swapchains.erase(it);
        LayerLog("Unregistered swapchain %p", swapchain);
    }
}

SwapchainData* VulkanLayerState::GetSwapchainData(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    return (it != m_Swapchains.end()) ? it->second : nullptr;
}

bool VulkanLayerState::IsAppWhitelisted(const char* appName) {
    // TODO: Check against config.ini whitelist
    // For now, always enabled
    return true;
}

// ============================================================================
// Layer Negotiation
// ============================================================================

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* pVersionStruct) {
    
    if (!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;
    
    if (pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->pfnGetInstanceProcAddr = Capture_vkGetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = Capture_vkGetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    
    if (pVersionStruct->loaderLayerInterfaceVersion > CURRENT_LOADER_LAYER_INTERFACE_VERSION) {
        pVersionStruct->loaderLayerInterfaceVersion = CURRENT_LOADER_LAYER_INTERFACE_VERSION;
    }
    
    LayerLog("Layer negotiated version %d", pVersionStruct->loaderLayerInterfaceVersion);
    return VK_SUCCESS;
}

// ============================================================================
// Layer Enumeration
// ============================================================================

extern "C" VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    
    if (!pProperties) {
        *pPropertyCount = 1;
        return VK_SUCCESS;
    }
    
    if (*pPropertyCount == 0) {
        return VK_INCOMPLETE;
    }
    
    strncpy(pProperties->layerName, LAYER_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
    strncpy(pProperties->description, LAYER_DESCRIPTION, VK_MAX_DESCRIPTION_SIZE - 1);
    pProperties->specVersion = LAYER_API_VERSION;
    pProperties->implementationVersion = LAYER_IMPLEMENTATION_VERSION;
    
    *pPropertyCount = 1;
    return VK_SUCCESS;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    
    (void)physicalDevice;
    return Capture_vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

// ============================================================================
// Get Proc Addr Implementation
// ============================================================================

#define GETPROCADDR(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction)Capture_##name

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Capture_vkGetInstanceProcAddr(
    VkInstance instance, const char* pName) {
    
    // Global functions
    GETPROCADDR(vkEnumerateInstanceLayerProperties);
    GETPROCADDR(vkEnumerateDeviceLayerProperties);
    
    // Instance functions
    GETPROCADDR(vkCreateInstance);
    GETPROCADDR(vkDestroyInstance);
    GETPROCADDR(vkCreateDevice);
    
    // If we have an instance, forward to next layer
    if (instance) {
        auto* dispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
        if (dispatch && dispatch->GetInstanceProcAddr) {
            return dispatch->GetInstanceProcAddr(instance, pName);
        }
    }
    
    return nullptr;
}

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Capture_vkGetDeviceProcAddr(
    VkDevice device, const char* pName) {
    
    // Device functions we intercept
    GETPROCADDR(vkDestroyDevice);
    GETPROCADDR(vkCreateSampler);
    GETPROCADDR(vkCreateSwapchainKHR);
    GETPROCADDR(vkDestroySwapchainKHR);
    GETPROCADDR(vkGetSwapchainImagesKHR);
    GETPROCADDR(vkAcquireNextImageKHR);
    GETPROCADDR(vkQueuePresentKHR);
    GETPROCADDR(vkQueueSubmit);
    GETPROCADDR(vkGetDeviceQueue);
    
    // Forward to next layer
    if (device) {
        auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
        if (dispatch && dispatch->GetDeviceProcAddr) {
            return dispatch->GetDeviceProcAddr(device, pName);
        }
    }
    
    return nullptr;
}

#undef GETPROCADDR

// ============================================================================
// Helper to populate dispatch tables
// ============================================================================

static void PopulateInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance,
                                      PFN_vkGetInstanceProcAddr gipa) {
    dispatch->instance = instance;
    dispatch->GetInstanceProcAddr = gipa;
    
    #define LOAD_INSTANCE(name) dispatch->name = (PFN_vk##name)gipa(instance, "vk" #name)
    
    LOAD_INSTANCE(DestroyInstance);
    LOAD_INSTANCE(EnumeratePhysicalDevices);
    LOAD_INSTANCE(GetPhysicalDeviceProperties);
    LOAD_INSTANCE(GetPhysicalDeviceProperties2);
    LOAD_INSTANCE(GetPhysicalDeviceFeatures);
    LOAD_INSTANCE(GetPhysicalDeviceFeatures2);
    LOAD_INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE(GetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE(CreateDevice);
    LOAD_INSTANCE(EnumerateDeviceExtensionProperties);
    LOAD_INSTANCE(DestroySurfaceKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceSupportKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceCapabilitiesKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfaceFormatsKHR);
    LOAD_INSTANCE(GetPhysicalDeviceSurfacePresentModesKHR);
    
#ifdef VK_USE_PLATFORM_WIN32_KHR
    LOAD_INSTANCE(CreateWin32SurfaceKHR);
#endif
    
    #undef LOAD_INSTANCE
}

static void PopulateDeviceDispatch(DeviceDispatch* dispatch, VkDevice device,
                                    PFN_vkGetDeviceProcAddr gdpa) {
    dispatch->device = device;
    dispatch->GetDeviceProcAddr = gdpa;
    
    #define LOAD_DEVICE(name) dispatch->name = (PFN_vk##name)gdpa(device, "vk" #name)
    
    LOAD_DEVICE(DestroyDevice);
    LOAD_DEVICE(GetDeviceQueue);
    LOAD_DEVICE(QueueSubmit);
    LOAD_DEVICE(QueueWaitIdle);
    LOAD_DEVICE(DeviceWaitIdle);
    LOAD_DEVICE(AllocateMemory);
    LOAD_DEVICE(FreeMemory);
    LOAD_DEVICE(MapMemory);
    LOAD_DEVICE(UnmapMemory);
    LOAD_DEVICE(BindBufferMemory);
    LOAD_DEVICE(BindImageMemory);
    LOAD_DEVICE(CreateBuffer);
    LOAD_DEVICE(DestroyBuffer);
    LOAD_DEVICE(CreateImage);
    LOAD_DEVICE(DestroyImage);
    LOAD_DEVICE(GetImageMemoryRequirements);
    LOAD_DEVICE(GetBufferMemoryRequirements);
    LOAD_DEVICE(CreateImageView);
    LOAD_DEVICE(DestroyImageView);
    LOAD_DEVICE(CreateSampler);
    LOAD_DEVICE(DestroySampler);
    LOAD_DEVICE(CreateFramebuffer);
    LOAD_DEVICE(DestroyFramebuffer);
    LOAD_DEVICE(CreateRenderPass);
    LOAD_DEVICE(DestroyRenderPass);
    LOAD_DEVICE(CreateCommandPool);
    LOAD_DEVICE(DestroyCommandPool);
    LOAD_DEVICE(AllocateCommandBuffers);
    LOAD_DEVICE(FreeCommandBuffers);
    LOAD_DEVICE(BeginCommandBuffer);
    LOAD_DEVICE(EndCommandBuffer);
    LOAD_DEVICE(ResetCommandBuffer);
    LOAD_DEVICE(CmdBeginRenderPass);
    LOAD_DEVICE(CmdEndRenderPass);
    LOAD_DEVICE(CmdBindPipeline);
    LOAD_DEVICE(CmdDraw);
    LOAD_DEVICE(CmdDrawIndexed);
    LOAD_DEVICE(CmdCopyImage);
    LOAD_DEVICE(CmdBlitImage);
    LOAD_DEVICE(CmdPipelineBarrier);
    LOAD_DEVICE(CreateFence);
    LOAD_DEVICE(DestroyFence);
    LOAD_DEVICE(WaitForFences);
    LOAD_DEVICE(ResetFences);
    LOAD_DEVICE(CreateSemaphore);
    LOAD_DEVICE(DestroySemaphore);
    LOAD_DEVICE(CreateSwapchainKHR);
    LOAD_DEVICE(DestroySwapchainKHR);
    LOAD_DEVICE(GetSwapchainImagesKHR);
    LOAD_DEVICE(AcquireNextImageKHR);
    LOAD_DEVICE(QueuePresentKHR);
    LOAD_DEVICE(CreateDescriptorSetLayout);
    LOAD_DEVICE(DestroyDescriptorSetLayout);
    LOAD_DEVICE(CreateDescriptorPool);
    LOAD_DEVICE(DestroyDescriptorPool);
    LOAD_DEVICE(AllocateDescriptorSets);
    LOAD_DEVICE(FreeDescriptorSets);
    LOAD_DEVICE(UpdateDescriptorSets);
    LOAD_DEVICE(CreatePipelineLayout);
    LOAD_DEVICE(DestroyPipelineLayout);
    LOAD_DEVICE(CreateGraphicsPipelines);
    LOAD_DEVICE(DestroyPipeline);
    LOAD_DEVICE(CreateShaderModule);
    LOAD_DEVICE(DestroyShaderModule);
    
#ifdef VK_KHR_external_memory_win32
    LOAD_DEVICE(GetMemoryWin32HandleKHR);
#endif
    
    #undef LOAD_DEVICE
}

// ============================================================================
// Instance Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    
    LayerLog("vkCreateInstance called");
    
    // Find the next layer's vkCreateInstance
    VkLayerInstanceCreateInfo* layerInfo = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                         layerInfo->function != VK_LAYER_LINK_INFO)) {
        layerInfo = (VkLayerInstanceCreateInfo*)layerInfo->pNext;
    }
    
    if (!layerInfo) {
        LayerLog("Failed to find layer chain info");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    PFN_vkGetInstanceProcAddr gipa = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    
    if (!createInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Advance the link info for next layer
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;
    
    // Create the instance
    VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
    
    if (result != VK_SUCCESS) {
        return result;
    }
    
    // Create and populate dispatch table
    auto* dispatch = new InstanceDispatch();
    PopulateInstanceDispatch(dispatch, *pInstance, gipa);
    VulkanLayerState::Get().RegisterInstance(*pInstance, dispatch);
    
    LayerLog("vkCreateInstance succeeded: %p", *pInstance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
    
    LayerLog("vkDestroyInstance: %p", instance);
    
    auto* dispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (dispatch && dispatch->DestroyInstance) {
        dispatch->DestroyInstance(instance, pAllocator);
    }
    
    VulkanLayerState::Get().UnregisterInstance(instance);
}

// ============================================================================
// Device Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    
    LayerLog("vkCreateDevice called");
    
    // Find the next layer's vkCreateDevice
    VkLayerDeviceCreateInfo* layerInfo = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                         layerInfo->function != VK_LAYER_LINK_INFO)) {
        layerInfo = (VkLayerDeviceCreateInfo*)layerInfo->pNext;
    }
    
    if (!layerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    PFN_vkGetInstanceProcAddr gipa = layerInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = layerInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice createDevice = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    
    if (!createDevice) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Advance the link info
    layerInfo->u.pLayerInfo = layerInfo->u.pLayerInfo->pNext;
    
    // Create the device
    VkResult result = createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    
    if (result != VK_SUCCESS) {
        return result;
    }
    
    // Create and populate dispatch table
    auto* dispatch = new DeviceDispatch();
    dispatch->physicalDevice = physicalDevice;
    PopulateDeviceDispatch(dispatch, *pDevice, gdpa);
    VulkanLayerState::Get().RegisterDevice(*pDevice, dispatch);
    
    LayerLog("vkCreateDevice succeeded: %p", *pDevice);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    
    LayerLog("vkDestroyDevice: %p", device);
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (dispatch && dispatch->DestroyDevice) {
        dispatch->DestroyDevice(device, pAllocator);
    }
    
    VulkanLayerState::Get().UnregisterDevice(device);
}

// ============================================================================
// Sampler Creation (AF/Mip Override)
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!dispatch || !dispatch->CreateSampler) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    auto& state = VulkanLayerState::Get();
    
    // Apply AF/mip overrides if configured
    VkSamplerCreateInfo modifiedInfo = *pCreateInfo;
    
    uint32_t maxAnisotropy = state.GetMaxAnisotropy();
    if (maxAnisotropy > 1 && modifiedInfo.anisotropyEnable == VK_FALSE) {
        modifiedInfo.anisotropyEnable = VK_TRUE;
        modifiedInfo.maxAnisotropy = (float)maxAnisotropy;
        LayerLog("Sampler: Enabled anisotropy %u", maxAnisotropy);
    } else if (maxAnisotropy > 1 && modifiedInfo.maxAnisotropy < maxAnisotropy) {
        modifiedInfo.maxAnisotropy = (float)maxAnisotropy;
    }
    
    float mipBias = state.GetMipLodBias();
    if (mipBias != 0.0f) {
        modifiedInfo.mipLodBias = mipBias;
    }
    
    return dispatch->CreateSampler(device, &modifiedInfo, pAllocator, pSampler);
}

// ============================================================================
// Queue Functions
// ============================================================================

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!dispatch || !dispatch->GetDeviceQueue) {
        return;
    }
    
    dispatch->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    
    // Track queue to device mapping
    VulkanLayerState::Get().RegisterQueue(*pQueue, device);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    if (!dispatch || !dispatch->QueueSubmit) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // TODO: Track submissions for capture synchronization
    return dispatch->QueueSubmit(queue, submitCount, pSubmits, fence);
}

// ============================================================================
// Swapchain Functions
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {
    
    LayerLog("vkCreateSwapchainKHR: %ux%u", pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!dispatch || !dispatch->CreateSwapchainKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // TODO: Apply present mode override for VSync
    VkResult result = dispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    
    if (result == VK_SUCCESS && pSwapchain) {
        // Create swapchain tracking data
        auto* data = new SwapchainData();
        data->swapchain = *pSwapchain;
        data->device = device;
        data->format = pCreateInfo->imageFormat;
        data->extent = pCreateInfo->imageExtent;
        data->captureEnabled = false;
        
        VulkanLayerState::Get().RegisterSwapchain(*pSwapchain, data);
        LayerLog("Created swapchain %p", *pSwapchain);
    }
    
    return result;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {
    
    LayerLog("vkDestroySwapchainKHR: %p", swapchain);
    
    // Clean up overlay resources
    auto* data = VulkanLayerState::Get().GetSwapchainData(swapchain);
    if (data) {
        // TODO: Clean up overlay framebuffers, command buffers, etc.
        VulkanLayerState::Get().UnregisterSwapchain(swapchain);
    }
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (dispatch && dispatch->DestroySwapchainKHR) {
        dispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!dispatch || !dispatch->GetSwapchainImagesKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkResult result = dispatch->GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    
    // Track swapchain images for capture
    if (result == VK_SUCCESS && pSwapchainImages) {
        auto* data = VulkanLayerState::Get().GetSwapchainData(swapchain);
        if (data) {
            data->imageCount = *pSwapchainImageCount;
            data->images.resize(*pSwapchainImageCount);
            for (uint32_t i = 0; i < *pSwapchainImageCount; i++) {
                data->images[i] = pSwapchainImages[i];
            }
            LayerLog("Tracked %u swapchain images", *pSwapchainImageCount);
        }
    }
    
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    if (!dispatch || !dispatch->QueuePresentKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // TODO: Draw overlay before present
    if (VulkanLayerState::Get().IsOverlayEnabled()) {
        // DrawOverlayOnSwapchains(pPresentInfo);
    }
    
    // TODO: Trigger capture frame copy if enabled
    if (VulkanLayerState::Get().IsCaptureEnabled()) {
        // CaptureSwapchainFrames(pPresentInfo);
    }
    
    return dispatch->QueuePresentKHR(queue, pPresentInfo);
}

// ============================================================================
// AcquireNextImageKHR (forward)
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex) {
    
    auto* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!dispatch || !dispatch->AcquireNextImageKHR) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    VkResult result = dispatch->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    
    // Track the current image index for overlay/capture
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        auto* data = VulkanLayerState::Get().GetSwapchainData(swapchain);
        if (data) {
            data->currentImageIndex = *pImageIndex;
        }
    }
    
    return result;
}
