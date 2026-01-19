/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture via D3D11 Interop
 *
 * OLD WORKING APPROACH:
 * Creates D3D11 textures with KMT sharing, imports them into Vulkan.
 * Vulkan copies to D3D11 textures, encoder opens KMT handles cross-process.
 * This is GPU zero-copy and proven to work.
 */

#include "vulkan_layer.h"
#include "layer_main.h"
#include "../../common/shared_defs.h"
#include <vector>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_4.h>

struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    
    // D3D11 intermediate path - creates D3D11 textures that Vulkan imports
    // KMT handles work cross-process with encoder
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* d3d11Textures[4] = {};
    HANDLE d3d11TextureHandles[4] = {};  // KMT handles for cross-process sharing
    uint32_t dxgiFormat = 0;
    
    // Vulkan imported images (reference D3D11 textures)
    std::vector<VkImage> importedImages;
    std::vector<VkDeviceMemory> importedMemories;
    
    std::vector<VkFence> copyFences;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    
    uint32_t currentWriteIndex = 0;
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

// Format conversion helpers
static uint32_t VkFormatToDXGI(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return 87; // DXGI_FORMAT_B8G8R8A8_UNORM
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
            return 24; // DXGI_FORMAT_R10G10B10A2_UNORM
        default:
            return 87; // Fallback to BGRA
    }
}

static VkFormat DXGIToVkFormat(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 24: return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
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

    // Get shared memory pointer for CPU staging
    state.pShmemBuffer = LayerIPC_GetShmemBuffer();
    if (!state.pShmemBuffer) {
        LayerLog("Vulkan Layer: [Error] No shared memory buffer available for CPU staging");
        return;
    }

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
            state.useTimelineSemaphores = false;
            state.timelineSemaphore = VK_NULL_HANDLE;
        }
    }

    // Create CPU staging buffers (host-visible, mappable)
    // GPU external memory doesn't work cross-process, so we copy to CPU then to shared memory
    uint32_t bytesPerPixel = GetBytesPerPixel(format);
    state.bufferSize = (VkDeviceSize)extent.width * extent.height * bytesPerPixel;
    
    state.stagingBuffers.resize(imageCount);
    state.stagingMemories.resize(imageCount);
    state.stagingMapped.resize(imageCount);

    // Create CPU staging buffers for each swapchain image
    // We only need 2 buffers for double-buffering regardless of swapchain image count
    const uint32_t numStagingBuffers = 2;
    state.stagingBuffers.resize(numStagingBuffers);
    state.stagingMemories.resize(numStagingBuffers);
    state.stagingMapped.resize(numStagingBuffers);
    
    LayerLog("Vulkan Layer: Creating %u CPU staging buffers (%llu bytes each)", numStagingBuffers, state.bufferSize);
    
    for (uint32_t i = 0; i < numStagingBuffers; i++) {
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = state.bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        VkResult bufResult = disp->fp_vkCreateBuffer(device, &bufferInfo, nullptr, &state.stagingBuffers[i]);
        if (bufResult != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to create staging buffer %u (vk result: %d)", i, bufResult);
            return;
        }
        
        VkMemoryRequirements memReqs;
        disp->fp_vkGetBufferMemoryRequirements(device, state.stagingBuffers[i], &memReqs);
        
        // Find host-visible, host-coherent memory for CPU access
        uint32_t memTypeIndex = FindMemoryType(device, memReqs.memoryTypeBits, 
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = memTypeIndex;
        
        VkResult memResult = disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &state.stagingMemories[i]);
        if (memResult != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to allocate staging memory %u (vk result: %d)", i, memResult);
            return;
        }
        
        disp->fp_vkBindBufferMemory(device, state.stagingBuffers[i], state.stagingMemories[i], 0);
        
        // Persistently map the buffer
        VkResult mapResult = disp->fp_vkMapMemory(device, state.stagingMemories[i], 0, state.bufferSize, 0, &state.stagingMapped[i]);
        if (mapResult != VK_SUCCESS) {
            LayerLog("Vulkan Layer: [Error] Failed to map staging memory %u (vk result: %d)", i, mapResult);
            return;
        }
    }
    
    LayerLog("Vulkan Layer: CPU staging buffers created successfully");
    
    // Publish dimensions to shared memory (textureIndex >= 100 indicates SHMEM mode)
    LayerIPC_SetShmemDimensions(extent.width, extent.height, VkFormatToDXGI(format));
    
    state.initialized = true;
    g_CaptureStates[device] = state;
}

void CleanupCapture(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end()) return;

    VulkanCaptureState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        // Clean up CPU staging buffers
        for (uint32_t i = 0; i < state.stagingBuffers.size(); i++) {
            if (state.stagingMapped[i] && state.stagingMemories[i]) {
                disp->fp_vkUnmapMemory(device, state.stagingMemories[i]);
            }
            if (state.stagingBuffers[i]) disp->fp_vkDestroyBuffer(device, state.stagingBuffers[i], nullptr);
            if (state.stagingMemories[i]) disp->fp_vkFreeMemory(device, state.stagingMemories[i], nullptr);
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

    // Use double-buffering for staging buffers (0 or 1)
    uint32_t bufferIndex = state.currentShmemSlot % 2;
    
    // Validate staging buffer exists
    if (bufferIndex >= state.stagingBuffers.size() || !state.stagingBuffers[bufferIndex]) {
        return;
    }

    // Check for device lost state
    static std::atomic<bool> s_deviceLostReported{false};

    // Use fence from imageIndex (we have fences per swapchain image)
    uint32_t fenceIndex = imageIndex % state.copyFences.size();
    VkFence fence = state.copyFences[fenceIndex];

    // Use 100ms timeout instead of UINT64_MAX to prevent potential hangs
    constexpr uint64_t FENCE_TIMEOUT_NS = 100000000;
    VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);

    if (waitResult != VK_SUCCESS) {
        if (waitResult == VK_ERROR_DEVICE_LOST) {
            LayerLog("Vulkan Layer: [Critical] DEVICE_LOST detected during fence wait");
            s_deviceLostReported.store(true);
            CleanupCapture(device);
            return;
        }
        if (waitResult == VK_TIMEOUT) {
            return; // Skip this frame
        }
        return;
    }

    VkResult resetResult = disp->fp_vkResetFences(device, 1, &fence);
    if (resetResult == VK_ERROR_DEVICE_LOST) {
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }

    uint32_t cmdIndex = imageIndex % state.commandBuffers.size();
    VkCommandBuffer cmd = state.commandBuffers[cmdIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult beginResult = disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    if (beginResult != VK_SUCCESS) {
        if (beginResult == VK_ERROR_DEVICE_LOST) {
            s_deviceLostReported.store(true);
            CleanupCapture(device);
        }
        return;
    }

    // Transition source image to transfer source
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
                                   0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

    // Copy image to buffer (GPU -> CPU staging buffer)
    VkBufferImageCopy region = {};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;  // Tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { state.extent.width, state.extent.height, 1 };
    
    disp->fp_vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 
                                     state.stagingBuffers[bufferIndex], 1, &region);

    // Transition source image back to present
    srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 
                                   0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

    VkResult endResult = disp->fp_vkEndCommandBuffer(cmd);
    if (endResult != VK_SUCCESS) {
        if (endResult == VK_ERROR_DEVICE_LOST) {
            s_deviceLostReported.store(true);
            CleanupCapture(device);
        }
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
        LayerLog("Vulkan Layer: [Critical] DEVICE_LOST during QueueSubmit");
        s_deviceLostReported.store(true);
        CleanupCapture(device);
        return;
    }
    if (submitResult != VK_SUCCESS) {
        return;
    }

    // Wait for GPU copy to complete (synchronous - required for CPU access)
    waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NS);
    if (waitResult != VK_SUCCESS) {
        return;
    }

    // Copy from staging buffer to shared memory
    if (state.pShmemBuffer && state.stagingMapped[bufferIndex]) {
        ShmemBuffer* shmem = static_cast<ShmemBuffer*>(state.pShmemBuffer);
        uint32_t shmemSlot = state.currentShmemSlot % ShmemBuffer::SLOT_COUNT;
        
        // Calculate copy size (clamp to max supported resolution)
        uint32_t copyWidth = (state.extent.width <= ShmemBuffer::MAX_WIDTH) ? state.extent.width : ShmemBuffer::MAX_WIDTH;
        uint32_t copyHeight = (state.extent.height <= ShmemBuffer::MAX_HEIGHT) ? state.extent.height : ShmemBuffer::MAX_HEIGHT;
        size_t copySize = (size_t)copyWidth * copyHeight * 4;  // RGBA
        
        // Copy to shared memory slot
        memcpy(shmem->data[shmemSlot], state.stagingMapped[bufferIndex], copySize);
        
        // Update shmem metadata
        shmem->validWidth = copyWidth;
        shmem->validHeight = copyHeight;
        shmem->pitch = copyWidth * 4;
        shmem->slotReady[shmemSlot].store(true, std::memory_order_release);
        shmem->writeSlot.store(shmemSlot, std::memory_order_release);
        
        // Signal frame ready with textureIndex = 100 + slot (indicates SHMEM mode)
        LayerIPC_SignalFrameReady(100 + shmemSlot);
        
        state.currentShmemSlot++;
    }

    s_deviceLostReported.store(false);
}
