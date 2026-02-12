/**
 * Vulkan Layer - Overlay Implementation
 * 
 * Implements overlay rendering for Vulkan using the CustomOverlay system.
 * Frame management (command buffers, fences, semaphores) is handled here,
 * while content rendering is delegated to OverlayAdapter.
 */

#include "../common/ipc_client.h"
#include "../common/performance_metrics.h"
#include "../common/system_metrics.h"
#include "../common/overlay_adapter.h"
#include "../common/custom_overlay_vk.h"  // For VulkanBackend access
#include "layer_main.h"
#include "vulkan_layer.h"
#include <chrono>
#include <string>
#include <vector>

#include "../common/input_manager.h"

namespace {

// Overlay state per device - manages Vulkan frame resources
struct OverlayState {
  bool initialized = false;
  VkDevice device = VK_NULL_HANDLE;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkCommandPool commandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> commandBuffers;
  std::vector<VkFence> fences;
  std::vector<VkSemaphore> semaphores;
  std::vector<VkFramebuffer> framebuffers;
  std::vector<VkImageView> imageViews;
  std::vector<VkImage> swapchainImages;
  VkExtent2D extent = {0, 0};
  VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
  PerformanceMetrics *metrics = nullptr;
  bool needsWindowHook = false;
  uint32_t queueFamilyIndex = 0;
  
  // OverlayAdapter for content rendering
  OverlayAdapter* overlayAdapter = nullptr;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

} // anonymous namespace

// Find graphics queue family index
static uint32_t FindGraphicsQueueFamily(VkPhysicalDevice physDevice, InstanceDispatch* instDisp) {
    uint32_t queueFamilyCount = 0;
    instDisp->fp_vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);
    
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    instDisp->fp_vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());
    
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            return i;
        }
    }
    return 0; // Fallback to 0 if not found
}

void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain,
                       VkFormat format, VkExtent2D extent, uint32_t imageCount,
                       VkImage* images, HWND window) {
    LayerLog("Vulkan Layer: InitializeOverlay ENTRY(device=%p, images=%d, window=%p, "
             "size=%dx%d)", device, imageCount, window, extent.width, extent.height);
    
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    LayerLog("Vulkan Layer: InitializeOverlay - Got mutex lock");
    
    if (window) {
        LayerLog("Vulkan Layer: InitializeOverlay - Hooking window...");
        InputManager::Get().HookWindow(window);
        LayerLog("Vulkan Layer: InitializeOverlay - Window hooked");
    } else {
        LayerLog("Vulkan Layer: [Warning] No window provided for overlay. Will "
                 "attempt deferred hook.");
    }
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    LayerLog("Vulkan Layer: InitializeOverlay - Got device dispatch: %p", disp);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }
    
    LayerLog("Vulkan Layer: InitializeOverlay - Getting instance dispatch...");
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(
        VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice));
    LayerLog("Vulkan Layer: InitializeOverlay - Got instance dispatch: %p", instDisp);
    
    LayerLog("Vulkan Layer: InitializeOverlay - Creating OverlayState...");
    OverlayState state = {};
    state.device = device;
    state.physicalDevice = disp->physicalDevice;
    state.instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice);
    state.format = format;
    state.extent = extent;
    state.swapchainImages.assign(images, images + imageCount);
    state.needsWindowHook = (window == nullptr);
    
    LayerLog("Vulkan Layer: InitializeOverlay - Initializing SystemMetricsCollector...");
    if (instDisp && instDisp->fp_vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idProps = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        instDisp->fp_vkGetPhysicalDeviceProperties2(disp->physicalDevice, &props2);
        
        if (idProps.deviceLUIDValid) {
            uint32_t luidLow = *(uint32_t*)&idProps.deviceLUID[0];
            uint32_t luidHigh = *(uint32_t*)&idProps.deviceLUID[4];
            SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
            
            VkPhysicalDeviceMemoryProperties memProps = {};
            instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);
            
            VkDeviceSize maxHeapSize = 0;
            for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    maxHeapSize = std::max(maxHeapSize, memProps.memoryHeaps[i].size);
                }
            }
            if (maxHeapSize > 0) {
                SystemMetricsCollector::Get().SetVRAMTotal(maxHeapSize);
            }
        }
    }
    LayerLog("Vulkan Layer: InitializeOverlay - SystemMetricsCollector init done");
    
    // Find graphics queue family
    uint32_t graphicsQueueFamily = FindGraphicsQueueFamily(state.physicalDevice, instDisp);
    state.queueFamilyIndex = graphicsQueueFamily;
    LayerLog("Vulkan Layer: InitializeOverlay - Using graphics queue family %d", graphicsQueueFamily);
    
    // Create render pass (load existing content, don't clear)
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    
    VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    
    LayerLog("Vulkan Layer: InitializeOverlay - Creating render pass...");
    if (disp->fp_vkCreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create render pass");
        return;
    }
    LayerLog("Vulkan Layer: InitializeOverlay - Render pass created");
    
    // Create command pool
    LayerLog("Vulkan Layer: InitializeOverlay - Creating command pool (queueFamily=%d)...", graphicsQueueFamily);
    VkCommandPoolCreateInfo cpInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = graphicsQueueFamily;
    
    if (disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create command pool");
        return;
    }
    LayerLog("Vulkan Layer: InitializeOverlay - Command pool created");
    
    // Create framebuffers, image views, fences, semaphores
    LayerLog("Vulkan Layer: InitializeOverlay - Creating %d framebuffers...", imageCount);
    state.imageViews.resize(imageCount);
    state.framebuffers.resize(imageCount);
    state.commandBuffers.resize(imageCount);
    state.fences.resize(imageCount);
    state.semaphores.resize(imageCount);
    
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo ivInfo = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivInfo.image = images[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = format;
        ivInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        disp->fp_vkCreateImageView(device, &ivInfo, nullptr, &state.imageViews[i]);
        
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = state.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &state.imageViews[i];
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        disp->fp_vkCreateFramebuffer(device, &fbInfo, nullptr, &state.framebuffers[i]);
        
        VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.fences[i]);
        
        VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.semaphores[i]);
    }
    
    LayerLog("Vulkan Layer: InitializeOverlay - Allocating command buffers...");
    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());
    
    // Create OverlayAdapter for this device
    LayerLog("Vulkan Layer: InitializeOverlay - Creating OverlayAdapter...");
    state.overlayAdapter = new OverlayAdapter();
    
    // Get queue for Vulkan backend
    VkQueue queue = VK_NULL_HANDLE;
    disp->fp_vkGetDeviceQueue(device, graphicsQueueFamily, 0, &queue);
    
    // Initialize with dispatch tables
    if (!state.overlayAdapter->InitVulkan(device, disp->physicalDevice, queue,
                                          graphicsQueueFamily, disp, instDisp)) {
        LayerLog("Vulkan Layer: Failed to initialize OverlayAdapter");
        delete state.overlayAdapter;
        state.overlayAdapter = nullptr;
        return;
    }
    
    // Set up the adapter with metrics and IPC
    state.metrics = new PerformanceMetrics();
    state.overlayAdapter->SetMetrics(state.metrics);
    state.overlayAdapter->SetIPCClient(&g_IPCClient);
    state.overlayAdapter->SetGraphicsAPI("Vulkan");
    
    // Create pipeline for the render pass
    auto* vkBackend = static_cast<CustomOverlay::VulkanBackend*>(state.overlayAdapter->GetBackend());
    if (vkBackend) {
        vkBackend->CreatePipelineForRenderPass(state.renderPass);
    }
    
    state.initialized = true;
    g_OverlayStates[device] = state;
    
    LayerLog("Vulkan Layer: Overlay initialized successfully");
}

VkSemaphore GetOverlaySemaphore(VkDevice device, uint32_t imageIndex) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it != g_OverlayStates.end() && it->second.initialized) {
        if (imageIndex < it->second.semaphores.size()) {
            return it->second.semaphores[imageIndex];
        }
    }
    return VK_NULL_HANDLE;
}

void CleanupOverlay(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end())
        return;
    
    OverlayState &state = it->second;
    DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        
        for (auto fb : state.framebuffers)
            disp->fp_vkDestroyFramebuffer(device, fb, nullptr);
        for (auto iv : state.imageViews)
            disp->fp_vkDestroyImageView(device, iv, nullptr);
        for (auto f : state.fences)
            disp->fp_vkDestroyFence(device, f, nullptr);
        for (auto s : state.semaphores)
            disp->fp_vkDestroySemaphore(device, s, nullptr);
        
        disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
        disp->fp_vkDestroyRenderPass(device, state.renderPass, nullptr);
    }
    
    delete state.overlayAdapter;
    delete state.metrics;
    g_OverlayStates.erase(it);
}

// Render overlay using OverlayAdapter
void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex,
                   VkSemaphore waitSemaphore, VkSemaphore signalSemaphore) {
    // Early out if overlay is disabled
    if (g_IPCClient.GetSharedMem() &&
        !g_IPCClient.GetSharedMem()->overlayConfig.showOverlay) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end() || !it->second.initialized)
        return;
    
    OverlayState &state = it->second;
    DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp)
        return;
    
    // Deferred window hook
    if (state.needsWindowHook) {
        HWND hwnd = GetForegroundWindow();
        if (hwnd) {
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(hwnd, &foregroundPid);
            if (foregroundPid == GetCurrentProcessId()) {
                InputManager::Get().HookWindow(hwnd);
                state.needsWindowHook = false;
                LayerLog("Vulkan Layer: Deferred window hook successful (hwnd=%p)", hwnd);
            }
        }
    }
    
    // Update metrics
    if (state.metrics) {
        state.metrics->Update(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
    
    // Wait for fence
    VkFence fence = state.fences[imageIndex];
    disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000);
    disp->fp_vkResetFences(device, 1, &fence);
    
    // Record command buffer
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    disp->fp_vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        return;
    
    // Transition image from PRESENT_SRC to COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.image = state.swapchainImages[imageIndex];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &barrier);
    
    // Begin render pass
    VkRenderPassBeginInfo rpBeginInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBeginInfo.renderPass = state.renderPass;
    rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
    rpBeginInfo.renderArea.extent = state.extent;
    disp->fp_vkCmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // Set render context for VulkanBackend and render via OverlayAdapter
    if (state.overlayAdapter) {
        auto* vkBackend = static_cast<CustomOverlay::VulkanBackend*>(state.overlayAdapter->GetBackend());
        if (vkBackend) {
            vkBackend->SetRenderContext(cmd, state.renderPass, 
                                        state.framebuffers[imageIndex], state.extent);
        }
        state.overlayAdapter->RenderOverlay(state.extent.width, state.extent.height);
    }
    
    // End render pass
    disp->fp_vkCmdEndRenderPass(cmd);
    
    // Transition to present
    VkImageMemoryBarrier presentBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    presentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    presentBarrier.image = state.swapchainImages[imageIndex];
    presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    
    disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                                  0, nullptr, 1, &presentBarrier);
    
    disp->fp_vkEndCommandBuffer(cmd);
    
    // Submit
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (waitSemaphore != VK_NULL_HANDLE) {
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
    }
    
    if (signalSemaphore != VK_NULL_HANDLE) {
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;
    }
    
    disp->fp_vkQueueSubmit(queue, 1, &submitInfo, fence);
}
