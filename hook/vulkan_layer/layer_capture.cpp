/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture
 *
 * External Memory Type Priority:
 * 1. VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT - Modern D3D12 interop
 * 2. VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT - D3D11 interop (NT handles)
 * 3. VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT - Legacy fallback
 */

#include "vulkan_layer.h"
#include "layer_main.h"
#include <vector>
#include <dxgi.h>

struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    std::vector<HANDLE> exportedHandles;
    std::vector<VkDeviceMemory> exportedMemories;
    std::vector<VkImage> stagingImages;
    std::vector<VkSemaphore> copySemaphores;
    std::vector<VkFence> copyFences;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    VkExternalMemoryHandleTypeFlagBits selectedExternalMemoryType;  // Track which type we're using

    // Timeline semaphore support (Vulkan 1.2+ or VK_KHR_timeline_semaphore)
    bool useTimelineSemaphores = false;
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    uint64_t timelineValue = 0;
    std::vector<uint64_t> timelineValues;  // Per-frame timeline values
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;

// Helper to find memory type
uint32_t FindMemoryType(VkDevice device, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    auto disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) return 0;

    VkPhysicalDeviceMemoryProperties memProperties;
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
    if (!instDisp) return 0;

    instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

// Try external memory types in priority order
struct ExternalMemoryTypeInfo {
    VkExternalMemoryHandleTypeFlagBits type;
    const char* name;
};

// Zero-Copy Video Capture: Force D3D11_TEXTURE for FFmpeg D3D11VA compatibility
// All exported textures must be D3D11-compatible since the encoder uses D3D11VA
// D3D12_RESOURCE cannot be opened by D3D11's OpenSharedResource, so we skip it
static const ExternalMemoryTypeInfo g_ExternalMemoryTypes[] = {
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE (primary for D3D11VA)" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT (legacy fallback)" }
    // REMOVED: D3D12_RESOURCE - not compatible with FFmpeg D3D11VA
};
static constexpr int g_NumExternalMemoryTypes = 2;

// Check if an external memory type is supported
bool IsExternalMemoryTypeSupported(VkPhysicalDevice physicalDevice, VkExternalMemoryHandleTypeFlagBits type) {
    // For zero-copy D3D11VA compatibility, we only support D3D11 texture types
    if (type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT) {
        return true;  // Primary choice - NT handles for D3D11
    }
    if (type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT) {
        return true;  // Legacy fallback - KMT handles
    }
    // D3D12_RESOURCE is NOT supported - incompatible with D3D11VA
    return false;
}

/**
 * Check if timeline semaphores are supported
 * Requires Vulkan 1.2+ or VK_KHR_timeline_semaphore extension
 *
 * Note: This is a simplified check that just tries to create a timeline semaphore.
 * Full feature checking requires vkGetPhysicalDeviceFeatures2 which isn't
 * currently available in our dispatch table.
 */
bool IsTimelineSemaphoreSupported(VkDevice device, DeviceDispatch* disp) {
    // Just try to create a timeline semaphore - if it works, the feature is supported
    VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE,
        0  // Initial value
    };

    VkSemaphoreCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        &timelineCreateInfo
    };

    VkSemaphore testSemaphore = VK_NULL_HANDLE;
    VkResult result = disp->fp_vkCreateSemaphore(device, &createInfo, nullptr, &testSemaphore);

    if (result == VK_SUCCESS) {
        disp->fp_vkDestroySemaphore(device, testSemaphore, nullptr);
        LayerLog("Vulkan Layer: Timeline semaphores supported");
        return true;
    }

    LayerLog("Vulkan Layer: Timeline semaphores NOT supported (vk result: %d) - using binary fences", result);
    return false;
}

/**
 * Create a timeline semaphore for cross-queue synchronization
 */
bool CreateTimelineSemaphore(VkDevice device, DeviceDispatch* disp, VkSemaphore* pSemaphore) {
    VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        nullptr,
        VK_SEMAPHORE_TYPE_TIMELINE,
        0  // Initial value
    };

    VkSemaphoreCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        &timelineCreateInfo
    };

    VkResult result = disp->fp_vkCreateSemaphore(device, &createInfo, nullptr, pSemaphore);
    if (result == VK_SUCCESS) {
        LayerLog("Vulkan Layer: Created timeline semaphore");
        return true;
    }

    LayerLog("Vulkan Layer: Failed to create timeline semaphore (vk result: %d)", result);
    return false;
}

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format,
                       VkExtent2D extent, uint32_t imageCount)
{
    LayerLog("Vulkan Layer: InitializeCapture(device=%p, images=%d, size=%dx%d, vkFormat=%d)",
             device, imageCount, extent.width, extent.height, format);

    // Check format compatibility first
    if (!IsVkFormatCompatibleWithDXGI(format)) {
        LayerLog("Vulkan Layer: [Warning] Format %d may not be compatible with DXGI shared resources", format);
        // Continue anyway - some formats may work despite not being explicitly supported
    }

    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }

    VulkanCaptureState state = {};
    state.device = device;
    state.format = format;
    state.extent = extent;
    state.imageCount = imageCount;
    state.selectedExternalMemoryType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;  // Invalid initially

    VkCommandPoolCreateInfo cpInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    if (disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: [Error] Failed to create command pool");
        return;
    }

    state.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo cbInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state.commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, imageCount };
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());

    state.copyFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT };
    for (uint32_t i = 0; i < imageCount; i++) disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]);

    // Try to use timeline semaphores for better synchronization
    state.useTimelineSemaphores = IsTimelineSemaphoreSupported(device, disp);
    if (state.useTimelineSemaphores) {
        state.timelineValues.resize(imageCount, 0);
        if (!CreateTimelineSemaphore(device, disp, &state.timelineSemaphore)) {
            state.useTimelineSemaphores = false;  // Fallback to binary fences
            state.timelineSemaphore = VK_NULL_HANDLE;
        }
    }

    state.stagingImages.resize(imageCount);
    state.exportedMemories.resize(imageCount);
    state.exportedHandles.resize(imageCount);

    // Try each external memory type in priority order
    bool resourceCreated = false;
    for (int typeIdx = 0; typeIdx < g_NumExternalMemoryTypes; typeIdx++) {
        const ExternalMemoryTypeInfo& typeInfo = g_ExternalMemoryTypes[typeIdx];
        VkExternalMemoryHandleTypeFlagBits externalType = typeInfo.type;

        // Check if this type is supported
        VkPhysicalDevice physicalDevice = disp->physicalDevice;
        if (!IsExternalMemoryTypeSupported(physicalDevice, externalType)) {
            continue;
        }

        LayerLog("Vulkan Layer: Trying external memory type %s...", typeInfo.name);

        // Try to create resources with this type
        bool success = true;
        for (uint32_t i = 0; i < imageCount; i++) {
            VkExternalMemoryImageCreateInfo extMemInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, externalType };
            VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extMemInfo };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {extent.width, extent.height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

            VkImage testImage = VK_NULL_HANDLE;
            VkResult imageResult = disp->fp_vkCreateImage(device, &imageInfo, nullptr, &testImage);

            if (imageResult != VK_SUCCESS || testImage == VK_NULL_HANDLE) {
                LayerLog("Vulkan Layer: Failed to create image with %s (vk result: %d) - trying next type", typeInfo.name, imageResult);
                success = false;
                break;
            }

            VkMemoryRequirements memReqs;
            disp->fp_vkGetImageMemoryRequirements(device, testImage, &memReqs);

            VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, externalType };
            VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo, memReqs.size, 0 };
            allocInfo.memoryTypeIndex = FindMemoryType(device, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            VkDeviceMemory testMemory = VK_NULL_HANDLE;
            VkResult memResult = disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &testMemory);

            if (memResult != VK_SUCCESS || testMemory == VK_NULL_HANDLE) {
                LayerLog("Vulkan Layer: Failed to allocate memory with %s (vk result: %d) - trying next type", typeInfo.name, memResult);
                disp->fp_vkDestroyImage(device, testImage, nullptr);
                success = false;
                break;
            }

            // If we get here, this external memory type works!
            state.stagingImages[i] = testImage;
            state.exportedMemories[i] = testMemory;
            disp->fp_vkBindImageMemory(device, state.stagingImages[i], state.exportedMemories[i], 0);

            // Get the export handle
            if (disp->fp_vkGetMemoryWin32HandleKHR) {
                VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, state.exportedMemories[i], externalType };
                VkResult handleResult = disp->fp_vkGetMemoryWin32HandleKHR(device, &handleInfo, &state.exportedHandles[i]);

                if (handleResult != VK_SUCCESS || state.exportedHandles[i] == NULL) {
                    LayerLog("Vulkan Layer: Failed to get handle with %s (vk result: %d) - trying next type", typeInfo.name, handleResult);
                    success = false;
                    break;
                }
            } else {
                LayerLog("Vulkan Layer: vkGetMemoryWin32HandleKHR not available!");
                success = false;
                break;
            }
        }

        if (success) {
            state.selectedExternalMemoryType = externalType;
            LayerLog("Vulkan Layer: Successfully created resources with %s", typeInfo.name);
            resourceCreated = true;
            break;  // Success! Stop trying other types
        } else {
            // Clean up any partial resources from this attempt
            for (uint32_t i = 0; i < imageCount; i++) {
                if (state.exportedMemories[i]) {
                    disp->fp_vkFreeMemory(device, state.exportedMemories[i], nullptr);
                    state.exportedMemories[i] = VK_NULL_HANDLE;
                }
                if (state.stagingImages[i]) {
                    disp->fp_vkDestroyImage(device, state.stagingImages[i], nullptr);
                    state.stagingImages[i] = VK_NULL_HANDLE;
                }
                state.exportedHandles[i] = NULL;
            }
        }
    }

    if (!resourceCreated) {
        LayerLog("Vulkan Layer: [Error] Failed to create resources with any external memory type!");
        return;
    }

    state.initialized = true;
    g_CaptureStates[device] = state;
    if (!state.exportedHandles.empty()) {
        LayerIPC_SetTextures(state.exportedHandles.data(), (uint32_t)state.exportedHandles.size(), extent.width, extent.height, VkFormatToDXGI(format));
    }
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end()) return;

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        for (uint32_t i = 0; i < state.stagingImages.size(); i++) {
            if (state.stagingImages[i]) disp->fp_vkDestroyImage(device, state.stagingImages[i], nullptr);
            if (state.exportedMemories[i]) disp->fp_vkFreeMemory(device, state.exportedMemories[i], nullptr);
            if (state.exportedHandles[i]) CloseHandle(state.exportedHandles[i]);
        }
        for (auto fence : state.copyFences) disp->fp_vkDestroyFence(device, fence, nullptr);
        if (state.timelineSemaphore) {
            disp->fp_vkDestroySemaphore(device, state.timelineSemaphore, nullptr);
        }
        disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
    }
    g_CaptureStates.erase(it);
}

void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized) return;

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }

    // Validate image index bounds
    if (imageIndex >= state.stagingImages.size()) {
        static int indexErrorLogCount = 0;
        if (indexErrorLogCount++ % 60 == 0) {
            LayerLog("Vulkan Layer: [Warning] Image index %u out of bounds (max: %zu)",
                     imageIndex, state.stagingImages.size());
        }
        return;
    }

    // Check for device lost state
    static std::atomic<bool> s_deviceLostReported{false};
    if (state.initialized && !s_deviceLostReported.load()) {
        // Try a lightweight operation to check device health
        // (actual check happens via error returns below)
    }

    VkFence fence = state.copyFences[imageIndex];

    // Use 100ms timeout instead of UINT64_MAX to prevent potential hangs
    // 100ms in nanoseconds = 100,000,000
    constexpr uint64_t FENCE_TIMEOUT_NS = 100000000;
    VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);

    if (waitResult != VK_SUCCESS) {
        if (waitResult == VK_ERROR_DEVICE_LOST) {
            LayerLog("Vulkan Layer: [Critical] DEVICE_LOST detected during fence wait - cleaning up capture");
            s_deviceLostReported.store(true);
            CleanupCapture(device);
            return;
        }
        if (waitResult == VK_TIMEOUT) {
            static int timeoutLogCount = 0;
            if (timeoutLogCount++ % 60 == 0) {
                LayerLog("Vulkan Layer: [Warning] Fence timeout (100ms) on image %u - skipping capture frame", imageIndex);
            }
            return; // Skip this frame if fence is still signaled
        } else {
            static int waitErrorLogCount = 0;
            if (waitErrorLogCount++ % 60 == 0) {
                LayerLog("Vulkan Layer: [Error] vkWaitForFences failed: %d - skipping capture frame", waitResult);
            }
            return;
        }
    }

    VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult == VK_ERROR_DEVICE_LOST) {
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during fence reset - cleaning up capture");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }

    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult beginResult = disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult == VK_ERROR_DEVICE_LOST) {
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during BeginCommandBuffer - cleaning up capture");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }
    if (beginResult != VK_SUCCESS) {
        static int beginErrorLogCount = 0;
        if (beginErrorLogCount++ % 60 == 0) {
            LayerLog("Vulkan Layer: [Error] vkBeginCommandBuffer failed: %d", beginResult);
        }
        return;
    }

    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier dstBarrier = srcBarrier;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = state.stagingImages[imageIndex];

    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageCopy copy = { {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0,0,0}, {state.extent.width, state.extent.height, 1} };
    disp->fp_vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.stagingImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkResult endResult = disp->fp_vkEndCommandBuffer(cmd);
    if (endResult == VK_ERROR_DEVICE_LOST) {
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during EndCommandBuffer - cleaning up capture");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }
    if (endResult != VK_SUCCESS) {
        static int endErrorLogCount = 0;
        if (endErrorLogCount++ % 60 == 0) {
            LayerLog("Vulkan Layer: [Error] vkEndCommandBuffer failed: %d", endResult);
        }
        return;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd };
    VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submit, fence);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during QueueSubmit - cleaning up capture");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }
    if (submitResult != VK_SUCCESS) {
        static int submitErrorLogCount = 0;
        if (submitErrorLogCount++ % 60 == 0) {
            LayerLog("Vulkan Layer: [Error] vkQueueSubmit failed: %d", submitResult);
        }
        return;
    }

    // Reset device lost flag on successful capture
    s_deviceLostReported.store(false);
    LayerIPC_IncrementWriteIndex(0);
}
