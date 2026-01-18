/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture
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

void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format,
                       VkExtent2D extent, uint32_t imageCount)
{
    LayerLog("Vulkan Layer: InitializeCapture(device=%p, images=%d, size=%dx%d)", device, imageCount, extent.width, extent.height);
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
    
    state.stagingImages.resize(imageCount);
    state.exportedMemories.resize(imageCount);
    state.exportedHandles.resize(imageCount);
    
    for (uint32_t i = 0; i < imageCount; i++) {
        VkExternalMemoryImageCreateInfo extMemInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
        VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &extMemInfo };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {extent.width, extent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        
        if (disp->fp_vkCreateImage(device, &imageInfo, nullptr, &state.stagingImages[i]) == VK_SUCCESS) {
            VkMemoryRequirements memReqs;
            disp->fp_vkGetImageMemoryRequirements(device, state.stagingImages[i], &memReqs);
            
            VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
            VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo, memReqs.size, 0 };
            allocInfo.memoryTypeIndex = FindMemoryType(device, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); 
            
            if (disp->fp_vkAllocateMemory(device, &allocInfo, nullptr, &state.exportedMemories[i]) == VK_SUCCESS) {
                disp->fp_vkBindImageMemory(device, state.stagingImages[i], state.exportedMemories[i], 0);
                if (disp->fp_vkGetMemoryWin32HandleKHR) {
                    VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, state.exportedMemories[i], VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT };
                    disp->fp_vkGetMemoryWin32HandleKHR(device, &handleInfo, &state.exportedHandles[i]);
                } else {
                    LayerLog("Vulkan Layer: [Warning] vkGetMemoryWin32HandleKHR not found!");
                }
            } else {
                LayerLog("Vulkan Layer: [Error] Failed to allocate exported memory for image %d", i);
            }
        } else {
             LayerLog("Vulkan Layer: [Error] Failed to create staging image %d", i);
        }
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
    if (!disp || imageIndex >= state.stagingImages.size()) return;
    
    VkFence fence = state.copyFences[imageIndex];
    disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    disp->fp_vkResetFences(device, 1, &fence);
    
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    disp->fp_vkBeginCommandBuffer(cmd, &beginInfo);
    
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
    disp->fp_vkEndCommandBuffer(cmd);
    
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd };
    disp->fp_vkQueueSubmit(queue, 1, &submit, fence);
    LayerIPC_IncrementWriteIndex(0);
}
