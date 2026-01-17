/**
 * VK_LAYER_CE_overlay - Zero-Copy Capture
 * 
 * Video capture using VK_KHR_external_memory for zero-copy
 * export to D3D11/D3D12 for encoding.
 */

#include "layer_main.h"
#include <vector>
#include <dxgi.h>
#include <d3d11.h>

#ifndef DXGI_SHARED_RESOURCE_READ
#define DXGI_SHARED_RESOURCE_READ 0x80000000L
#define DXGI_SHARED_RESOURCE_WRITE 1
#endif

// Capture state per device
struct VulkanCaptureState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t imageCount = 0;
    
    // Export handles for zero-copy to D3D11
    std::vector<HANDLE> exportedHandles;
    std::vector<VkDeviceMemory> exportedMemories;
    std::vector<VkImage> stagingImages;
    
    // Synchronization
    std::vector<VkSemaphore> copySemaphores;
    std::vector<VkFence> copyFences;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    
    // Capture state
    uint32_t captureIndex = 0;
    bool captureActive = false;
};

static std::mutex g_CaptureMutex;
static std::unordered_map<VkDevice, VulkanCaptureState> g_CaptureStates;

// Initialize capture for a swapchain
void InitializeCapture(VkDevice device, VkSwapchainKHR swapchain, VkFormat format,
                       VkExtent2D extent, uint32_t imageCount)
{
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Layer Capture: No dispatch table for device %p", device);
        return;
    }
    
    // Check if external memory is available
    if (!disp->GetMemoryWin32HandleKHR) {
        LayerLog("Layer Capture: VK_KHR_external_memory_win32 not available, using fallback");
        // Could implement a staging buffer fallback here
    }
    
    VulkanCaptureState state = {};
    state.device = device;
    state.format = format;
    state.extent = extent;
    state.imageCount = imageCount;
    
    // Create command pool for copy operations
    VkCommandPoolCreateInfo cpInfo = {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = 0; // TODO: Use proper queue family
    
    if (disp->CreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Layer Capture: Failed to create command pool");
        return;
    }
    
    // Allocate command buffers
    state.commandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    
    if (disp->AllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data()) != VK_SUCCESS) {
        LayerLog("Layer Capture: Failed to allocate command buffers");
        disp->DestroyCommandPool(device, state.commandPool, nullptr);
        return;
    }
    
    // Create fences for synchronization
    state.copyFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    
    for (uint32_t i = 0; i < imageCount; i++) {
        if (disp->CreateFence(device, &fenceInfo, nullptr, &state.copyFences[i]) != VK_SUCCESS) {
            LayerLog("Layer Capture: Failed to create fence %d", i);
        }
    }
    
    // Create semaphores
    state.copySemaphores.resize(imageCount);
    VkSemaphoreCreateInfo semInfo = {};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    
    for (uint32_t i = 0; i < imageCount; i++) {
        if (disp->CreateSemaphore(device, &semInfo, nullptr, &state.copySemaphores[i]) != VK_SUCCESS) {
            LayerLog("Layer Capture: Failed to create semaphore %d", i);
        }
    }
    
    // Get physical device memory properties for choosing memory type
    VkPhysicalDeviceMemoryProperties memProps;
    CEInstanceDispatch* instDisp = GetInstanceDispatch(disp->instance);
    if (!instDisp || !instDisp->GetPhysicalDeviceMemoryProperties) {
        LayerLog("Layer Capture: Cannot get physical device memory properties");
        return;
    }
    instDisp->GetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);

    auto FindMemoryType = [&](uint32_t typeFilter, VkMemoryPropertyFlags properties) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return 0; // Fallback
    };

    // Check capabilities for external memory export
    {
        VkPhysicalDeviceExternalImageFormatInfoKHR externalInfoReq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO_KHR };
        externalInfoReq.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageFormatProperties2 formatProps = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        VkExternalImageFormatPropertiesKHR externalProps = { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES_KHR };
        formatProps.pNext = &externalProps;

        VkPhysicalDeviceImageFormatInfo2 formatInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
        formatInfo.format = format;
        formatInfo.type = VK_IMAGE_TYPE_2D;
        formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        formatInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        formatInfo.pNext = &externalInfoReq;

        PFN_vkGetPhysicalDeviceImageFormatProperties2KHR pfnGetProps2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)
            instDisp->GetInstanceProcAddr(disp->instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");

        if (pfnGetProps2) {
            VkResult res = pfnGetProps2(disp->physicalDevice, &formatInfo, &formatProps);
            if (res == VK_SUCCESS) {
                LayerLog("Layer Capture: Handle type OPAQUE_WIN32_BIT supported. Export flags: %x, Compatible handles: %x",
                         externalProps.externalMemoryProperties.externalMemoryFeatures,
                         externalProps.externalMemoryProperties.compatibleHandleTypes);
            } else {
                LayerLog("Layer Capture: WARNING: vkGetPhysicalDeviceImageFormatProperties2KHR returned %d for OPAQUE_WIN32_BIT", res);
            }
            // Check KMT Support
             externalInfoReq.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
             res = pfnGetProps2(disp->physicalDevice, &formatInfo, &formatProps);
             if (res == VK_SUCCESS) {
                LayerLog("Layer Capture: Handle type D3D11_TEXTURE_KMT_BIT supported. Export flags: %x",
                         externalProps.externalMemoryProperties.externalMemoryFeatures);
             } else {
                LayerLog("Layer Capture: D3D11_TEXTURE_KMT_BIT query failed: %d", res);
             }
        }
    }

    // Create staging images with external memory for zero-copy export
    if (disp->GetMemoryWin32HandleKHR) {
        state.stagingImages.resize(imageCount);
        state.exportedMemories.resize(imageCount);
        state.exportedHandles.resize(imageCount);
        
        for (uint32_t i = 0; i < imageCount; i++) {
            // Create image with external memory flag
            VkExternalMemoryImageCreateInfo extMemInfo = {};
            extMemInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
            extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
            
            VkImageCreateInfo imageInfo = {};
            imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            imageInfo.pNext = &extMemInfo;
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {extent.width, extent.height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            
            if (disp->CreateImage(device, &imageInfo, nullptr, &state.stagingImages[i]) != VK_SUCCESS) {
                LayerLog("Layer Capture: Failed to create staging image %d", i);
                continue;
            }
            
            // Get memory requirements and allocate with export flag
            VkMemoryRequirements memReqs;
            disp->GetImageMemoryRequirements(device, state.stagingImages[i], &memReqs);
            
            // NT Handle Export Info
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = nullptr;

            VkExportMemoryAllocateInfo exportInfo = {};
            exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            exportInfo.pNext = nullptr; // KMT doesn't use Win32HandleInfo
            exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
            
            // Dedicated Allocation Info
            VkMemoryDedicatedAllocateInfoKHR dedicatedInfo = {};
            dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR;
            dedicatedInfo.pNext = &exportInfo;
            dedicatedInfo.image = state.stagingImages[i];
            dedicatedInfo.buffer = VK_NULL_HANDLE;

            VkMemoryAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocInfo.pNext = &dedicatedInfo; // Chain dedicated -> export -> nt_export
            allocInfo.allocationSize = memReqs.size;
            allocInfo.memoryTypeIndex = FindMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            
            if (disp->AllocateMemory(device, &allocInfo, nullptr, &state.exportedMemories[i]) != VK_SUCCESS) {
                LayerLog("Layer Capture: Failed to allocate memory for image %d", i);
                continue;
            }
            
            disp->BindImageMemory(device, state.stagingImages[i], state.exportedMemories[i], 0);
            
            // Get Win32 KMT handle
            VkMemoryGetWin32HandleInfoKHR handleInfo = {};
            handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
            handleInfo.memory = state.exportedMemories[i];
            handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
            
            if (disp->GetMemoryWin32HandleKHR(device, &handleInfo, &state.exportedHandles[i]) != VK_SUCCESS) {
                LayerLog("Layer Capture: Failed to get Win32 KMT handle for image %d", i);
            } else {
                LayerLog("Layer Capture: Got export KMT handle %p for image %d", state.exportedHandles[i], i);
            }
        }
    }
    
    state.initialized = true;
    g_CaptureStates[device] = state;
    
    // Publish texture handles to IPC for captureengine to import
    if (!state.exportedHandles.empty()) {
        LayerIPC_SetTextures(state.exportedHandles.data(), 
                            static_cast<uint32_t>(state.exportedHandles.size()),
                            extent.width, extent.height, VkFormatToDXGI(format));
    }
    
    LayerLog("Layer Capture: Initialized for device %p (%dx%d, %d images)", 
             device, extent.width, extent.height, imageCount);
}

// Cleanup capture resources
void CleanupCapture(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end()) {
        return;
    }
    
    VulkanCaptureState& state = it->second;
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    
    if (disp) {
        disp->DeviceWaitIdle(device);
        
        // Destroy staging images and memory
        for (uint32_t i = 0; i < state.stagingImages.size(); i++) {
            if (state.stagingImages[i]) {
                disp->DestroyImage(device, state.stagingImages[i], nullptr);
            }
            if (state.exportedMemories[i]) {
                disp->FreeMemory(device, state.exportedMemories[i], nullptr);
            }
            if (state.exportedHandles[i]) {
                CloseHandle(state.exportedHandles[i]);
            }
        }
        
        // Destroy sync objects
        for (auto fence : state.copyFences) {
            if (fence) disp->DestroyFence(device, fence, nullptr);
        }
        for (auto sem : state.copySemaphores) {
            if (sem) disp->DestroySemaphore(device, sem, nullptr);
        }
        
        if (state.commandPool) {
            disp->DestroyCommandPool(device, state.commandPool, nullptr);
        }
    }
    
    g_CaptureStates.erase(it);
    LayerLog("Layer Capture: Cleaned up for device %p", device);
}

// Capture a frame
void CaptureFrame(VkDevice device, VkQueue queue, VkImage srcImage, uint32_t imageIndex)
{
    std::lock_guard<std::mutex> lock(g_CaptureMutex);
    
    auto it = g_CaptureStates.find(device);
    if (it == g_CaptureStates.end() || !it->second.initialized) {
        return;
    }
    
    VulkanCaptureState& state = it->second;
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp) return;
    
    if (state.stagingImages.empty() || imageIndex >= state.stagingImages.size()) {
        return;
    }
    
    // Wait for previous copy to complete
    VkFence fence = state.copyFences[imageIndex];
    disp->WaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    disp->ResetFences(device, 1, &fence);
    
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    VkImage dstImage = state.stagingImages[imageIndex];
    
    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    disp->BeginCommandBuffer(cmd, &beginInfo);
    
    // Transition source image to transfer src
    VkImageMemoryBarrier srcBarrier = {};
    srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    srcBarrier.subresourceRange.levelCount = 1;
    srcBarrier.subresourceRange.layerCount = 1;
    
    // Transition destination image to transfer dst
    VkImageMemoryBarrier dstBarrier = {};
    dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.image = dstImage;
    dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    dstBarrier.subresourceRange.levelCount = 1;
    dstBarrier.subresourceRange.layerCount = 1;
    
    VkImageMemoryBarrier barriers[] = { srcBarrier, dstBarrier };
    disp->CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, barriers);
    
    // Copy image
    VkImageCopy copyRegion = {};
    copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.srcSubresource.layerCount = 1;
    copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.dstSubresource.layerCount = 1;
    copyRegion.extent = {state.extent.width, state.extent.height, 1};
    
    disp->CmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
    
    // Transition destination image to SHARED layout (must be ready for D3D11)
    // For NT handles, we usually transition to TRANSFER_DST_OPTIMAL or SHADER_READ_ONLY
    // but D3D11 usually expects it to be ready.
    // Actually, for External Memory, the layout transitions are tricky.
    // Let's use GENERAL or SHADER_READ_ONLY_OPTIMAL.
    
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    // Transition source back to present
    srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkImageMemoryBarrier postBarriers[] = { srcBarrier, dstBarrier };
    disp->CmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 2, postBarriers);
    
    disp->EndCommandBuffer(cmd);
    
    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    
    disp->QueueSubmit(queue, 1, &submitInfo, fence);
    
    // Signal IPC that a new frame is available
    // We use the imageIndex to tell captureengine which buffer to read
    LayerIPC_IncrementWriteIndex(0); // TODO: Pass actual timestamp if available
}
