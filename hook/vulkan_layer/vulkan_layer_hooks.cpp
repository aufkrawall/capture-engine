#include "vulkan_layer_internal.h"
#include "nv_lod_spread_override.h"
#include "vulkan_reflex_limiter.h"

namespace {

void ApplyConfiguredNvLodSpreadFix() {
    if (!g_LayerState.whitelisted) {
        return;
    }
    const SharedMemoryLayout* shared = g_IPCClient.GetSharedMem();
    const bool enabled = shared && shared->graphicsConfig.nvLodSpreadFix;
    ce::nv_lod_spread::Install(enabled ? ce::nv_lod_spread::Mode::kOn : ce::nv_lod_spread::Mode::kOff);
}

}  // namespace

void PopulateInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    dispatch->instance = instance;
    dispatch->fp_vkGetInstanceProcAddr = gipa;
    dispatch->fp_vkDestroyInstance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    dispatch->fp_vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    dispatch->fp_vkEnumeratePhysicalDeviceGroups =
        (PFN_vkEnumeratePhysicalDeviceGroups)gipa(instance, "vkEnumeratePhysicalDeviceGroups");
    dispatch->fp_vkEnumeratePhysicalDeviceGroupsKHR =
        (PFN_vkEnumeratePhysicalDeviceGroups)gipa(instance, "vkEnumeratePhysicalDeviceGroupsKHR");
    dispatch->fp_vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    dispatch->fp_vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    dispatch->fp_vkGetPhysicalDeviceFeatures =
        (PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures");
    dispatch->fp_vkGetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    dispatch->fp_vkGetPhysicalDeviceFormatProperties2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(instance, "vkGetPhysicalDeviceFormatProperties2");
    if (!dispatch->fp_vkGetPhysicalDeviceFormatProperties2) {
        dispatch->fp_vkGetPhysicalDeviceFormatProperties2 =
            (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(instance, "vkGetPhysicalDeviceFormatProperties2KHR");
    }
    dispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    dispatch->fp_vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    dispatch->fp_vkCreateDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    dispatch->fp_vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    dispatch->fp_vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfaceSupportKHR =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PopulatePresentTimingInstanceDispatch(dispatch, instance, gipa);
    dispatch->fp_vkGetPhysicalDeviceSurfaceFormatsKHR =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfacePresentModesKHR =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    dispatch->fp_vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)gipa(instance, "vkCreateWin32SurfaceKHR");
#endif
}

void PopulateDeviceDispatch(DeviceDispatch* dispatch, VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    dispatch->device = device;
    dispatch->fp_vkGetDeviceProcAddr = gdpa;
    dispatch->fp_vkDestroyDevice = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
    dispatch->fp_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
    dispatch->fp_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)gdpa(device, "vkGetDeviceQueue2");
    dispatch->fp_vkQueueSubmit = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
    dispatch->fp_vkQueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(device, "vkQueueSubmit2");
    dispatch->fp_vkQueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)gdpa(device, "vkQueueSubmit2KHR");
    dispatch->fp_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)gdpa(device, "vkQueueWaitIdle");
    dispatch->fp_vkCreateQueryPool = (PFN_vkCreateQueryPool)gdpa(device, "vkCreateQueryPool");
    dispatch->fp_vkDestroyQueryPool = (PFN_vkDestroyQueryPool)gdpa(device, "vkDestroyQueryPool");
    dispatch->fp_vkCmdResetQueryPool = (PFN_vkCmdResetQueryPool)gdpa(device, "vkCmdResetQueryPool");
    dispatch->fp_vkCmdWriteTimestamp = (PFN_vkCmdWriteTimestamp)gdpa(device, "vkCmdWriteTimestamp");
    dispatch->fp_vkGetQueryPoolResults = (PFN_vkGetQueryPoolResults)gdpa(device, "vkGetQueryPoolResults");
    dispatch->fp_vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");
    dispatch->fp_vkAllocateMemory = (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    dispatch->fp_vkFreeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    dispatch->fp_vkMapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    dispatch->fp_vkUnmapMemory = (PFN_vkUnmapMemory)gdpa(device, "vkUnmapMemory");
    dispatch->fp_vkBindBufferMemory = (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
    dispatch->fp_vkBindImageMemory = (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
    dispatch->fp_vkCreateBuffer = (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
    dispatch->fp_vkDestroyBuffer = (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
    dispatch->fp_vkCreateImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
    dispatch->fp_vkDestroyImage = (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
    dispatch->fp_vkGetImageMemoryRequirements =
        (PFN_vkGetImageMemoryRequirements)gdpa(device, "vkGetImageMemoryRequirements");
    dispatch->fp_vkGetBufferMemoryRequirements =
        (PFN_vkGetBufferMemoryRequirements)gdpa(device, "vkGetBufferMemoryRequirements");
    dispatch->fp_vkCreateImageView = (PFN_vkCreateImageView)gdpa(device, "vkCreateImageView");
    dispatch->fp_vkDestroyImageView = (PFN_vkDestroyImageView)gdpa(device, "vkDestroyImageView");
    dispatch->fp_vkCreateSampler = (PFN_vkCreateSampler)gdpa(device, "vkCreateSampler");
    dispatch->fp_vkDestroySampler = (PFN_vkDestroySampler)gdpa(device, "vkDestroySampler");
    dispatch->fp_vkCreateFramebuffer = (PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer");
    dispatch->fp_vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)gdpa(device, "vkDestroyFramebuffer");
    dispatch->fp_vkCreateRenderPass = (PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass");
    dispatch->fp_vkDestroyRenderPass = (PFN_vkDestroyRenderPass)gdpa(device, "vkDestroyRenderPass");
    dispatch->fp_vkCreateCommandPool = (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
    dispatch->fp_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
    dispatch->fp_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
    dispatch->fp_vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(device, "vkFreeCommandBuffers");
    dispatch->fp_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
    dispatch->fp_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
    dispatch->fp_vkResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(device, "vkResetCommandBuffer");
    dispatch->fp_vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)gdpa(device, "vkCmdBeginRenderPass");
    dispatch->fp_vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)gdpa(device, "vkCmdEndRenderPass");
    dispatch->fp_vkCmdBindPipeline = (PFN_vkCmdBindPipeline)gdpa(device, "vkCmdBindPipeline");
    dispatch->fp_vkCmdDraw = (PFN_vkCmdDraw)gdpa(device, "vkCmdDraw");
    dispatch->fp_vkCmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(device, "vkCmdDrawIndexed");
    dispatch->fp_vkCmdPushConstants = (PFN_vkCmdPushConstants)gdpa(device, "vkCmdPushConstants");
    dispatch->fp_vkCmdSetViewport = (PFN_vkCmdSetViewport)gdpa(device, "vkCmdSetViewport");
    dispatch->fp_vkCmdSetScissor = (PFN_vkCmdSetScissor)gdpa(device, "vkCmdSetScissor");
    dispatch->fp_vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)gdpa(device, "vkCmdBindVertexBuffers");
    dispatch->fp_vkCmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)gdpa(device, "vkCmdBindIndexBuffer");
    dispatch->fp_vkCmdCopyImage = (PFN_vkCmdCopyImage)gdpa(device, "vkCmdCopyImage");
    dispatch->fp_vkCmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)gdpa(device, "vkCmdCopyImageToBuffer");
    dispatch->fp_vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)gdpa(device, "vkCmdCopyBufferToImage");
    dispatch->fp_vkCmdBlitImage = (PFN_vkCmdBlitImage)gdpa(device, "vkCmdBlitImage");
    dispatch->fp_vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)gdpa(device, "vkCmdBindDescriptorSets");
    dispatch->fp_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier");
    dispatch->fp_vkCmdClearAttachments = (PFN_vkCmdClearAttachments)gdpa(device, "vkCmdClearAttachments");
    dispatch->fp_vkCreateFence = (PFN_vkCreateFence)gdpa(device, "vkCreateFence");
    dispatch->fp_vkDestroyFence = (PFN_vkDestroyFence)gdpa(device, "vkDestroyFence");
    dispatch->fp_vkWaitForFences = (PFN_vkWaitForFences)gdpa(device, "vkWaitForFences");
    dispatch->fp_vkResetFences = (PFN_vkResetFences)gdpa(device, "vkResetFences");
    dispatch->fp_vkWaitSemaphores = (PFN_vkWaitSemaphores)gdpa(device, "vkWaitSemaphores");
    if (!dispatch->fp_vkWaitSemaphores) {
        dispatch->fp_vkWaitSemaphores = (PFN_vkWaitSemaphores)gdpa(device, "vkWaitSemaphoresKHR");
    }
    dispatch->fp_vkCreateSemaphore = (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
    dispatch->fp_vkDestroySemaphore = (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
    dispatch->fp_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
    dispatch->fp_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
    dispatch->fp_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)gdpa(device, "vkGetSwapchainImagesKHR");
    dispatch->fp_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(device, "vkAcquireNextImageKHR");
    dispatch->fp_vkAcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR)gdpa(device, "vkAcquireNextImage2KHR");
    dispatch->fp_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
    PopulatePresentTimingDeviceDispatch(dispatch, device, gdpa);
    dispatch->fp_vkSetLatencySleepModeNV =
        (PFN_vkSetLatencySleepModeNV)gdpa(device, "vkSetLatencySleepModeNV");
    dispatch->fp_vkLatencySleepNV = (PFN_vkLatencySleepNV)gdpa(device, "vkLatencySleepNV");
    dispatch->fp_vkCreateDescriptorSetLayout =
        (PFN_vkCreateDescriptorSetLayout)gdpa(device, "vkCreateDescriptorSetLayout");
    dispatch->fp_vkDestroyDescriptorSetLayout =
        (PFN_vkDestroyDescriptorSetLayout)gdpa(device, "vkDestroyDescriptorSetLayout");
    dispatch->fp_vkCreateDescriptorPool = (PFN_vkCreateDescriptorPool)gdpa(device, "vkCreateDescriptorPool");
    dispatch->fp_vkDestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)gdpa(device, "vkDestroyDescriptorPool");
    dispatch->fp_vkAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)gdpa(device, "vkAllocateDescriptorSets");
    dispatch->fp_vkFreeDescriptorSets = (PFN_vkFreeDescriptorSets)gdpa(device, "vkFreeDescriptorSets");
    dispatch->fp_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)gdpa(device, "vkUpdateDescriptorSets");
    dispatch->fp_vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)gdpa(device, "vkCreatePipelineLayout");
    dispatch->fp_vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)gdpa(device, "vkDestroyPipelineLayout");
    dispatch->fp_vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)gdpa(device, "vkCreateGraphicsPipelines");
    dispatch->fp_vkCreateComputePipelines = (PFN_vkCreateComputePipelines)gdpa(device, "vkCreateComputePipelines");
    dispatch->fp_vkDestroyPipeline = (PFN_vkDestroyPipeline)gdpa(device, "vkDestroyPipeline");
    dispatch->fp_vkCmdDispatch = (PFN_vkCmdDispatch)gdpa(device, "vkCmdDispatch");
    dispatch->fp_vkCreateShaderModule = (PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule");
    dispatch->fp_vkDestroyShaderModule = (PFN_vkDestroyShaderModule)gdpa(device, "vkDestroyShaderModule");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    dispatch->fp_vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)gdpa(device, "vkGetMemoryWin32HandleKHR");
    dispatch->fp_vkGetMemoryWin32HandlePropertiesKHR =
        (PFN_vkGetMemoryWin32HandlePropertiesKHR)gdpa(device, "vkGetMemoryWin32HandlePropertiesKHR");
    dispatch->fp_vkGetSemaphoreWin32HandleKHR =
        (PFN_vkGetSemaphoreWin32HandleKHR)gdpa(device, "vkGetSemaphoreWin32HandleKHR");
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkInstance* pInstance) {
    LayerLog("Vulkan Layer: Capture_vkCreateInstance BEGIN");

    if (!pCreateInfo) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateInstance called with NULL pCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pInstance) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateInstance called with NULL pInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t apiVersion = pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0;
    LayerLog(
        "Vulkan Layer: Instance create info - apiVersion=%u.%u.%u, "
        "enabledLayerCount=%u, enabledExtensionCount=%u",
        VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion),
        pCreateInfo->enabledLayerCount, pCreateInfo->enabledExtensionCount);

    // LayerIPC_Init publishes PID-scoped ownership before this entry point is
    // active. Do not refresh it here: a resident layer from an older target
    // must never overwrite the current target's claim during late teardown.

    // The ICD may already be mapped before the layer sees vkCreateInstance. If
    // so, patch it now; otherwise the second call below catches the module that
    // the next layer maps while creating the instance.
    ApplyConfiguredNvLodSpreadFix();

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)NULL;
    VkLayerInstanceCreateInfo* chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    LayerLog("Vulkan Layer: Searching for VK_LAYER_LINK_INFO in pNext chain...");
    uint32_t chainDepth = 0;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        LayerLog("Vulkan Layer:   chain[%u] sType=%u, function=%u", chainDepth, chain_info->sType,
                 chain_info->function);
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
        chainDepth++;
    }

    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);
    gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    LayerLog("Vulkan Layer: pfnNextGetInstanceProcAddr=%p", (void*)gipa);
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_fn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

    VkResult res = VK_SUCCESS;
    bool presentTimingSurfaceQueriesEnabled = false;

    if (!g_LayerState.whitelisted) {
        // Passthrough: call next layer directly without modification
        res = create_fn(pCreateInfo, pAllocator, pInstance);
    } else {
        // Inject required extensions
        std::vector<const char*> extensions;
        extensions.reserve(pCreateInfo->enabledExtensionCount + 3);
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }

        bool hasProps2 = false;
        bool hasExtMemCaps = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
                hasProps2 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME) == 0)
                hasExtMemCaps = true;
        }

        if (!hasProps2)
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (!hasExtMemCaps)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);
        const size_t extensionCountBeforePresentTiming = extensions.size();
        bool presentTimingSurfaceQueryExtensionAdded = false;
        presentTimingSurfaceQueriesEnabled = EnablePresentTimingSurfaceQueries(
            gipa, VulkanLayerState::Get().WantsVblankPacedPresentation(), extensions,
            &presentTimingSurfaceQueryExtensionAdded);

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        // Enable validation layers in debug builds
#ifdef _DEBUG
        const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
        bool hasValidationLayer = false;

        // Check if already requested
        for (uint32_t i = 0; i < modifiedCreateInfo.enabledLayerCount; i++) {
            if (strcmp(modifiedCreateInfo.ppEnabledLayerNames[i], validationLayerName) == 0) {
                hasValidationLayer = true;
                break;
            }
        }

        if (!hasValidationLayer) {
            // Query available layers
            uint32_t layerCount = 0;
            PFN_vkEnumerateInstanceLayerProperties enumerateLayers =
                (PFN_vkEnumerateInstanceLayerProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
            if (enumerateLayers) {
                enumerateLayers(&layerCount, nullptr);
                std::vector<VkLayerProperties> availableLayers(layerCount);
                enumerateLayers(&layerCount, availableLayers.data());

                // Check if validation layer is available
                for (const auto& layer : availableLayers) {
                    if (strcmp(layer.layerName, validationLayerName) == 0) {
                        static const char* validationLayers[] = {validationLayerName};
                        modifiedCreateInfo.enabledLayerCount = 1;
                        modifiedCreateInfo.ppEnabledLayerNames = validationLayers;
                        LayerLog("Vulkan Layer: Enabling validation layer: %s", validationLayerName);
                        break;
                    }
                }
            }
        }
#endif

        LayerLog("Vulkan Layer: Calling next vkCreateInstance...");
        res = create_fn(&modifiedCreateInfo, pAllocator, pInstance);
        if (res != VK_SUCCESS && presentTimingSurfaceQueryExtensionAdded) {
            LayerLog("Vulkan Layer: vkCreateInstance rejected CE's optional native present-timing query "
                     "extension (result=%d); retrying without it", res);
            extensions.resize(extensionCountBeforePresentTiming);
            modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();
            presentTimingSurfaceQueriesEnabled = false;
            res = create_fn(&modifiedCreateInfo, pAllocator, pInstance);
        }
    }

    LayerLog("Vulkan Layer: next vkCreateInstance returned %d", res);
    if (res != VK_SUCCESS)
        return res;

    // This is the decisive DXVK/native-Vulkan boundary: the ICD is now mapped,
    // but vkCreateDevice and its filtering-state initialization have not run.
    ApplyConfiguredNvLodSpreadFix();

    auto* dispatch = new InstanceDispatch();
    PopulateInstanceDispatch(dispatch, *pInstance, gipa);
    dispatch->presentTimingSurfaceQueriesEnabled = presentTimingSurfaceQueriesEnabled;
    VulkanLayerState::Get().RegisterInstance(*pInstance, dispatch);

    LayerLog("Vulkan Layer: Capture_vkCreateInstance END - success, instance=%p", (void*)*pInstance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkDestroyInstance)
        disp->fp_vkDestroyInstance(instance, pAllocator);
    VulkanLayerState::Get().UnregisterInstance(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                                                                  VkPhysicalDevice* pPhysicalDevices) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (!disp || !disp->fp_vkEnumeratePhysicalDevices) {
        // CE has no chain to forward to, so it also has no business reporting a
        // failure the application would not otherwise have seen. Reporting zero
        // devices is the only honest answer this layer can give here.
        LayerLog("Vulkan Layer: [Warn] vkEnumeratePhysicalDevices with no dispatch for instance %p", (void*)instance);
        if (pPhysicalDeviceCount)
            *pPhysicalDeviceCount = 0;
        return VK_SUCCESS;
    }

    VkResult res = disp->fp_vkEnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (res >= VK_SUCCESS && pPhysicalDevices && pPhysicalDeviceCount && *pPhysicalDeviceCount > 0) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            VulkanLayerState::Get().TrackPhysicalDevice(pPhysicalDevices[i], instance);
        }
    }

    return res;
}

// vkEnumeratePhysicalDeviceGroups is the other way an application can obtain
// VkPhysicalDevice handles, and a Vulkan 1.1 engine with multi-GPU support may
// use it exclusively (Red Dead Redemption 2 does). Every handle it produces has
// to reach the ownership map, otherwise vkCreateDevice below cannot find the
// instance whose chain it must call into.
namespace {

void TrackPhysicalDeviceGroups(VkInstance instance, uint32_t groupCount,
                               const VkPhysicalDeviceGroupProperties* pGroups) {
    if (!pGroups)
        return;
    uint32_t tracked = 0;
    for (uint32_t group = 0; group < groupCount; ++group) {
        const uint32_t deviceCount = pGroups[group].physicalDeviceCount > VK_MAX_DEVICE_GROUP_SIZE
                                         ? static_cast<uint32_t>(VK_MAX_DEVICE_GROUP_SIZE)
                                         : pGroups[group].physicalDeviceCount;
        for (uint32_t index = 0; index < deviceCount; ++index) {
            VkPhysicalDevice physicalDevice = pGroups[group].physicalDevices[index];
            if (physicalDevice == VK_NULL_HANDLE)
                continue;
            VulkanLayerState::Get().TrackPhysicalDevice(physicalDevice, instance);
            ++tracked;
        }
    }
    if (tracked > 0) {
        LayerLog("Vulkan Layer: Tracked %u physical device(s) from %u device group(s) on instance %p", tracked,
                 groupCount, (void*)instance);
    }
}

VkResult EnumeratePhysicalDeviceGroupsCommon(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                             VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties,
                                             bool khrAlias) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    PFN_vkEnumeratePhysicalDeviceGroups next =
        disp ? (khrAlias ? disp->fp_vkEnumeratePhysicalDeviceGroupsKHR : disp->fp_vkEnumeratePhysicalDeviceGroups)
             : nullptr;
    if (!next && disp) {
        // The loader aliases the two entry points, so either pointer answers.
        next = khrAlias ? disp->fp_vkEnumeratePhysicalDeviceGroups : disp->fp_vkEnumeratePhysicalDeviceGroupsKHR;
    }
    if (!next) {
        LayerLog("Vulkan Layer: [Warn] vkEnumeratePhysicalDeviceGroups%s with no dispatch for instance %p",
                 khrAlias ? "KHR" : "", (void*)instance);
        if (pPhysicalDeviceGroupCount)
            *pPhysicalDeviceGroupCount = 0;
        return VK_SUCCESS;
    }

    VkResult res = next(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if (res >= VK_SUCCESS && pPhysicalDeviceGroupProperties && pPhysicalDeviceGroupCount) {
        TrackPhysicalDeviceGroups(instance, *pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    }
    return res;
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL
Capture_vkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                        VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    return EnumeratePhysicalDeviceGroupsCommon(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties,
                                               false);
}

VKAPI_ATTR VkResult VKAPI_CALL
Capture_vkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                           VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    return EnumeratePhysicalDeviceGroupsCommon(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties,
                                               true);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                      const VkDeviceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    LayerLog("Vulkan Layer: Capture_vkCreateDevice BEGIN");

    if (!pCreateInfo) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pDevice) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Defensive coverage for loaders that mapped the ICD after instance return.
    ApplyConfiguredNvLodSpreadFix();

    LayerLog(
        "Vulkan Layer: Device create info - queueCreateInfoCount=%u, "
        "enabledExtensionCount=%u, enabledLayerCount=%u",
        pCreateInfo->queueCreateInfoCount, pCreateInfo->enabledExtensionCount, pCreateInfo->enabledLayerCount);

    LogRequestedPresentationExtensions(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->enabledExtensionCount);

    const auto owner = VulkanLayerState::Get().ResolveInstanceForPhysicalDevice(physicalDevice);
    VkInstance instance = owner.instance;
    if (instance == VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: [Warn] No instance known for physical device %p; creating the device untouched",
                 (void*)physicalDevice);
    } else {
        LayerLog("Vulkan Layer: Found instance %p for physical device %p (%s)", (void*)instance, (void*)physicalDevice,
                 ce::vulkan_instance_registry::ToString(owner.resolution));
    }

    const PFN_vkSetDeviceLoaderData setDeviceLoaderData = FindDeviceLoaderDataCallback(*pCreateInfo);
    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    LayerLog("Vulkan Layer: Searching for VK_LAYER_LINK_INFO in device pNext chain...");
    uint32_t chainDepth = 0;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        LayerLog("Vulkan Layer:   chain[%u] sType=%u, function=%u", chainDepth, chain_info->sType,
                 chain_info->function);
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
        chainDepth++;
    }
    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in device pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);

    PFN_vkGetDeviceProcAddr gdpa = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_fn = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create_fn && instance != VK_NULL_HANDLE) {
        create_fn = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    }
    if (!create_fn) {
        LayerLog(
            "Vulkan Layer: [Error] Failed to get next vkCreateDevice from "
            "instance %p",
            instance);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // An unresolvable instance means CE cannot query the physical device or
    // reserve its overlay queue, but it must still not be the reason the game
    // fails to create a device: hand the application's own request straight to
    // the next link.
    const bool passthroughOnly = instance == VK_NULL_HANDLE;

    VkResult result = VK_SUCCESS;
    bool captureInteropEnabled = false;
    bool samplerAnisotropyEnabled = false;
    bool formatFeatureFlags2Available = false;
    bool storageImageReadWithoutFormatAvailable = false;
    bool storageImageWriteWithoutFormatAvailable = false;
    PresentTimingDeviceEnablement presentTimingEnablement;
    float maxSamplerAnisotropy = 1.0f;
    float maxSamplerLodBias = 0.0f;
    const VkPhysicalDeviceFeatures* requestedCoreFeatures = pCreateInfo->pEnabledFeatures;
    if (!requestedCoreFeatures) {
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                requestedCoreFeatures = &reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features;
                break;
            }
        }
    }
    if (requestedCoreFeatures) {
        samplerAnisotropyEnabled = requestedCoreFeatures->samplerAnisotropy == VK_TRUE;
        storageImageReadWithoutFormatAvailable =
            requestedCoreFeatures->shaderStorageImageReadWithoutFormat == VK_TRUE;
        storageImageWriteWithoutFormatAvailable =
            requestedCoreFeatures->shaderStorageImageWriteWithoutFormat == VK_TRUE;
    }
    // Must outlive the vkCreateDevice call below: modifiedCreateInfo points at
    // them when CE reserves its own overlay submission queue.
    OverlayQueueReservation overlayQueueReservation;
    std::vector<VkDeviceQueueCreateInfo> overlayQueueCreateInfos;
    std::vector<float> overlayQueuePriorities;

    if (!g_LayerState.whitelisted || passthroughOnly) {
        // Passthrough: call next layer directly without modification
        result = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    } else {
        InstanceDispatch* instanceDispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
        std::vector<VkExtensionProperties> availableExtensions;
        if (instanceDispatch && instanceDispatch->fp_vkEnumerateDeviceExtensionProperties) {
            uint32_t extensionCount = 0;
            if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                                                          nullptr) == VK_SUCCESS &&
                extensionCount > 0) {
                availableExtensions.resize(extensionCount);
                if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(
                        physicalDevice, nullptr, &extensionCount, availableExtensions.data()) != VK_SUCCESS) {
                    availableExtensions.clear();
                }
            }
        }
        auto extensionAvailable = [&](const char* name) {
            return std::any_of(availableExtensions.begin(), availableExtensions.end(),
                               [&](const VkExtensionProperties& ext) { return strcmp(ext.extensionName, name) == 0; });
        };

        VkPhysicalDeviceProperties physicalProperties = {};
        if (instanceDispatch && instanceDispatch->fp_vkGetPhysicalDeviceProperties)
            instanceDispatch->fp_vkGetPhysicalDeviceProperties(physicalDevice, &physicalProperties);
        maxSamplerAnisotropy = physicalProperties.limits.maxSamplerAnisotropy;
        maxSamplerLodBias = physicalProperties.limits.maxSamplerLodBias;
        const bool externalMemoryCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool externalSemaphoreCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool timelineCore = physicalProperties.apiVersion >= VK_API_VERSION_1_2;
        formatFeatureFlags2Available = physicalProperties.apiVersion >= VK_API_VERSION_1_3 ||
                                       extensionAvailable(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
        // Vulkan 1.3 and VK_KHR_format_feature_flags2 make the SPIR-V
        // read/write-without-format capabilities available independently of
        // the legacy core feature booleans. Per-format bits still decide
        // whether a particular swapchain format may use them.
        storageImageReadWithoutFormatAvailable |= formatFeatureFlags2Available;
        storageImageWriteWithoutFormatAvailable |= formatFeatureFlags2Available;

        PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
            instanceDispatch ? instanceDispatch->fp_vkGetPhysicalDeviceFeatures2 : nullptr;
        if (!getFeatures2) {
            getFeatures2 =
                reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (getFeatures2) {
            VkPhysicalDeviceFeatures2 supportedFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            supportedFeatures.pNext = &supportedTimeline;
            getFeatures2(physicalDevice, &supportedFeatures);
        }

        const bool requiredExtensionsAvailable =
            (externalMemoryCore || extensionAvailable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
            (externalSemaphoreCore || extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) &&
            (timelineCore || extensionAvailable(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));

        const VkPhysicalDeviceTimelineSemaphoreFeatures* requestedTimeline = nullptr;
        const VkPhysicalDeviceVulkan12Features* requestedVulkan12 = nullptr;
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                requestedTimeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                requestedVulkan12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node);
            }
        }
        const bool canEnableTimeline = supportedTimeline.timelineSemaphore == VK_TRUE;
        const bool appSpecifiedTimeline = requestedTimeline || requestedVulkan12;
        const bool appAlreadyEnabledTimeline = (requestedTimeline && requestedTimeline->timelineSemaphore == VK_TRUE) ||
                                               (requestedVulkan12 && requestedVulkan12->timelineSemaphore == VK_TRUE);
        captureInteropEnabled =
            requiredExtensionsAvailable && canEnableTimeline && (!appSpecifiedTimeline || appAlreadyEnabledTimeline);

        // Inject capture extensions only when the physical device actually
        // advertises the complete Win32 external-memory/fence contract. Never
        // make the game's vkCreateDevice fail merely because capture is absent.
        //
        // Preserve every application-requested extension, including NVIDIA's
        // generated-frame metering signal. Native relative timing adds the
        // independent display-rate ceiling below.
        std::vector<const char*> extensions;
        extensions.reserve(pCreateInfo->enabledExtensionCount + 9);
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i)
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);

        bool hasExtMem = false;
        bool hasExtMemWin32 = false;
        bool hasExtSem = false;
        bool hasExtSemWin32 = false;
        bool hasTimeline = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
                hasExtMem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
                hasExtMemWin32 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) == 0)
                hasExtSem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0)
                hasExtSemWin32 = true;
            if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
                hasTimeline = true;
        }

        if (captureInteropEnabled && !hasExtMem && !externalMemoryCore)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtMemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSem && !externalSemaphoreCore)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasTimeline && !timelineCore)
            extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

        VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        VkPhysicalDeviceFeatures enabledFeatures = {};
        if (pCreateInfo->pEnabledFeatures) {
            enabledFeatures = *pCreateInfo->pEnabledFeatures;
            modifiedCreateInfo.pEnabledFeatures = &enabledFeatures;
        }

        // Enable timeline semaphores only when the app did not already include
        // the feature structure. Duplicating an sType in pNext is invalid.
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timelineFeatures.timelineSemaphore = VK_TRUE;
        if (captureInteropEnabled && !appSpecifiedTimeline) {
            timelineFeatures.pNext = (void*)modifiedCreateInfo.pNext;
            modifiedCreateInfo.pNext = &timelineFeatures;
        }

        PreparePresentTimingDevice(instanceDispatch, physicalDevice, *pCreateInfo, availableExtensions,
                                   extensions, modifiedCreateInfo, presentTimingEnablement);

        if (!captureInteropEnabled) {
            LayerLog(
                "Vulkan Layer: Win32 external capture unavailable; creating device without capture-only "
                "extensions (extensions=%d timelineSupported=%d timelineRequested=%d)",
                requiredExtensionsAvailable ? 1 : 0, canEnableTimeline ? 1 : 0,
                appSpecifiedTimeline ? (appAlreadyEnabledTimeline ? 1 : 0) : -1);
        }

        // The overlay is a render pass, so it needs a graphics-capable queue.
        // A game that presents from a compute-only queue (DOOM Eternal presents
        // from queue family 2) leaves CE nowhere to submit unless it asked for
        // a queue of its own here, while the device is still being created.
        if (BuildOverlayQueueReservation(instanceDispatch, physicalDevice, *pCreateInfo, overlayQueueCreateInfos,
                                         overlayQueuePriorities, overlayQueueReservation)) {
            modifiedCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(overlayQueueCreateInfos.size());
            modifiedCreateInfo.pQueueCreateInfos = overlayQueueCreateInfos.data();
        }

        LayerLog("Vulkan Layer: Calling next vkCreateDevice...");
        result = CreateDeviceWithPresentTimingFallback(create_fn, physicalDevice, extensions,
                                                       modifiedCreateInfo, presentTimingEnablement,
                                                       pAllocator, pDevice);
        if (result != VK_SUCCESS && overlayQueueReservation.reserved) {
            // Never let CE's extra queue be the reason a game fails to start.
            LayerLog(
                "Vulkan Layer: vkCreateDevice failed with the reserved overlay queue (result=%d); retrying with "
                "the game's own queue request",
                result);
            overlayQueueReservation = OverlayQueueReservation{};
            modifiedCreateInfo.queueCreateInfoCount = pCreateInfo->queueCreateInfoCount;
            modifiedCreateInfo.pQueueCreateInfos = pCreateInfo->pQueueCreateInfos;
            result = CreateDeviceWithPresentTimingFallback(create_fn, physicalDevice, extensions,
                                                           modifiedCreateInfo, presentTimingEnablement,
                                                           pAllocator, pDevice);
        }
    }

    LayerLog("Vulkan Layer: next vkCreateDevice returned %d", result);
    if (result != VK_SUCCESS)
        return result;

    auto* dispatch = new DeviceDispatch();
    dispatch->physicalDevice = physicalDevice;
    dispatch->captureInteropEnabled = captureInteropEnabled;
    dispatch->samplerAnisotropyEnabled = samplerAnisotropyEnabled;
    dispatch->formatFeatureFlags2Available = formatFeatureFlags2Available;
    dispatch->storageImageReadWithoutFormatAvailable = storageImageReadWithoutFormatAvailable;
    dispatch->storageImageWriteWithoutFormatAvailable = storageImageWriteWithoutFormatAvailable;
    dispatch->relativePresentTimingEnabled = presentTimingEnablement.enabled;
    dispatch->maxSamplerAnisotropy = maxSamplerAnisotropy;
    dispatch->maxSamplerLodBias = maxSamplerLodBias;
    PopulateDeviceDispatch(dispatch, *pDevice, gdpa);
    VulkanLayerState::Get().RegisterDevice(*pDevice, dispatch);
    g_VulkanReflexLimiter.SetDevice(*pDevice, dispatch);

    InitializeReservedOverlayQueue(*pDevice, dispatch, overlayQueueReservation, setDeviceLoaderData);

    LayerLog("Vulkan Layer: Capture_vkCreateDevice END - success, device=%p", (void*)*pDevice);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    // Before UnregisterDevice: the queue-to-device mapping the release check
    // needs is still live at this point.
    ForgetBorrowedOverlaySubmitQueue(device);
    CleanupOverlay(device);
    CleanupCapture(device);
    CleanupPrerenderFences(device);
    g_VulkanReflexLimiter.ShutdownDevice(device);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkDestroyDevice)
        disp->fp_vkDestroyDevice(device, pAllocator);
    VulkanLayerState::Get().UnregisterDevice(device);
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                    VkQueue* pQueue) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceQueue) {
        disp->fp_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE) {
            VulkanLayerState::Get().RegisterQueue(*pQueue, device, queueFamilyIndex);
        }
    }
}
