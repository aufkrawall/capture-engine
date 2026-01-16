/**
 * VK_LAYER_CE_overlay - Overlay Rendering
 * 
 * Simple overlay rendering for Vulkan layer.
 * Renders FPS text directly to swapchain images before present.
 */

#include "layer_main.h"
#include <vector>
#include <chrono>

// Overlay state per device
struct OverlayState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent = {0, 0};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    
    // FPS tracking
    uint64_t frameCount = 0;
    std::chrono::steady_clock::time_point lastFpsTime;
    float currentFps = 0.0f;
    uint64_t framesThisSecond = 0;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

// Initialize overlay for a swapchain
void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, 
                       VkExtent2D extent, uint32_t imageCount, VkImage* images)
{
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Layer Overlay: No dispatch table for device %p", device);
        return;
    }
    
    OverlayState state = {};
    state.device = device;
    state.extent = extent;
    state.format = format;
    state.lastFpsTime = std::chrono::steady_clock::now();
    
    // Store swapchain images
    state.swapchainImages.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        state.swapchainImages[i] = images[i];
    }
    
    // Create render pass - simple pass that loads and stores
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentReference colorRef = {};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = 0;
    
    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2;
    rpInfo.pDependencies = deps;
    
    if (!disp->CreateRenderPass) {
        LayerLog("Layer Overlay: CreateRenderPass not available");
        return;
    }
    
    if (disp->CreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to create render pass");
        return;
    }
    
    // Create command pool
    VkCommandPoolCreateInfo cpInfo = {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = 0;
    
    if (!disp->CreateCommandPool) {
        LayerLog("Layer Overlay: CreateCommandPool not available");
        return;
    }
    
    if (disp->CreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to create command pool");
        disp->DestroyRenderPass(device, state.renderPass, nullptr);
        return;
    }
    
    // Create image views and framebuffers
    state.imageViews.resize(imageCount);
    state.framebuffers.resize(imageCount);
    state.commandBuffers.resize(imageCount);
    
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo ivInfo = {};
        ivInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image = images[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = format;
        ivInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivInfo.subresourceRange.levelCount = 1;
        ivInfo.subresourceRange.layerCount = 1;
        
        if (disp->CreateImageView(device, &ivInfo, nullptr, &state.imageViews[i]) != VK_SUCCESS) {
            LayerLog("Layer Overlay: Failed to create image view %d", i);
            continue;
        }
        
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = state.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &state.imageViews[i];
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        
        if (disp->CreateFramebuffer(device, &fbInfo, nullptr, &state.framebuffers[i]) != VK_SUCCESS) {
            LayerLog("Layer Overlay: Failed to create framebuffer %d", i);
        }
    }
    
    // Allocate command buffers
    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    
    if (disp->AllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data()) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to allocate command buffers");
    }
    
    state.initialized = true;
    g_OverlayStates[device] = state;
    
    LayerLog("Layer Overlay: Initialized for device %p (%dx%d, %d images)", 
             device, extent.width, extent.height, imageCount);
}

// Cleanup overlay resources
void CleanupOverlay(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end()) {
        return;
    }
    
    OverlayState& state = it->second;
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    
    if (disp) {
        if (disp->DeviceWaitIdle) disp->DeviceWaitIdle(device);
        
        for (auto fb : state.framebuffers) {
            if (fb && disp->DestroyFramebuffer) disp->DestroyFramebuffer(device, fb, nullptr);
        }
        for (auto iv : state.imageViews) {
            if (iv && disp->DestroyImageView) disp->DestroyImageView(device, iv, nullptr);
        }
        if (state.commandPool && disp->DestroyCommandPool) {
            disp->DestroyCommandPool(device, state.commandPool, nullptr);
        }
        if (state.renderPass && disp->DestroyRenderPass) {
            disp->DestroyRenderPass(device, state.renderPass, nullptr);
        }
    }
    
    g_OverlayStates.erase(it);
    LayerLog("Layer Overlay: Cleaned up for device %p", device);
}

// Render overlay - called from vkQueuePresentKHR
void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, 
                   VkSemaphore waitSemaphore, VkSemaphore signalSemaphore)
{
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end() || !it->second.initialized) {
        return;
    }
    
    OverlayState& state = it->second;
    CEDeviceDispatch* disp = GetDeviceDispatch(device);
    if (!disp) return;
    
    if (imageIndex >= state.commandBuffers.size() || imageIndex >= state.swapchainImages.size()) {
        return;
    }
    
    // Update FPS calculation
    state.frameCount++;
    state.framesThisSecond++;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastFpsTime).count();
    if (elapsed >= 1000) {
        state.currentFps = (float)state.framesThisSecond * 1000.0f / (float)elapsed;
        state.framesThisSecond = 0;
        state.lastFpsTime = now;
    }
    
    // Update IPC frame timing
    LayerIPC_UpdateFrameTiming(state.frameCount, state.currentFps, state.currentFps);
    
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    if (!cmd) return;
    
    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (!disp->BeginCommandBuffer) return;
    disp->BeginCommandBuffer(cmd, &beginInfo);
    
    // Transition to color attachment
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = state.swapchainImages[imageIndex];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    
    if (disp->CmdPipelineBarrier) {
        disp->CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    
    // Begin render pass and draw overlay indicator
    if (state.renderPass && state.framebuffers[imageIndex] && disp->CmdBeginRenderPass) {
        VkRenderPassBeginInfo rpBeginInfo = {};
        rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBeginInfo.renderPass = state.renderPass;
        rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
        rpBeginInfo.renderArea.extent = state.extent;
        
        disp->CmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Draw a green colored box in top-left corner as overlay indicator
        if (disp->CmdClearAttachments) {
            VkClearAttachment clearAtt = {};
            clearAtt.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clearAtt.colorAttachment = 0;
            clearAtt.clearValue.color.float32[0] = 0.0f;  // R
            clearAtt.clearValue.color.float32[1] = 1.0f;  // G (green)
            clearAtt.clearValue.color.float32[2] = 0.0f;  // B
            clearAtt.clearValue.color.float32[3] = 0.8f;  // A
            
            VkClearRect clearRect = {};
            clearRect.rect.offset = {10, 10};
            clearRect.rect.extent = {100, 40};  // Small green box top-left
            clearRect.baseArrayLayer = 0;
            clearRect.layerCount = 1;
            
            disp->CmdClearAttachments(cmd, 1, &clearAtt, 1, &clearRect);
        }
        
        if (disp->CmdEndRenderPass) disp->CmdEndRenderPass(cmd);
    }
    
    if (disp->EndCommandBuffer) disp->EndCommandBuffer(cmd);
    
    // Submit
    if (disp->QueueSubmit) {
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        
        disp->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        
        // Wait for completion
        if (disp->QueueWaitIdle) disp->QueueWaitIdle(queue);
    }
}
