/**
 * Vulkan Layer - Overlay Implementation
 *
 * Implements overlay rendering for Vulkan using the CustomOverlay system.
 * Frame management (command buffers, fences, semaphores) is handled here,
 * while content rendering is delegated to OverlayAdapter.
 */

#include "../common/custom_overlay_vk.h" // For VulkanBackend access
#include "../common/ipc_client.h"
#include "../common/overlay_adapter.h"
#include "../common/performance_metrics.h"
#include "../common/system_metrics.h"
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
  OverlayAdapter *overlayAdapter = nullptr;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

} // anonymous namespace

// Find graphics queue family index
static uint32_t FindGraphicsQueueFamily(VkPhysicalDevice physDevice,
                                        InstanceDispatch *instDisp) {
  uint32_t queueFamilyCount = 0;
  instDisp->fp_vkGetPhysicalDeviceQueueFamilyProperties(
      physDevice, &queueFamilyCount, nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  instDisp->fp_vkGetPhysicalDeviceQueueFamilyProperties(
      physDevice, &queueFamilyCount, queueFamilies.data());

  for (uint32_t i = 0; i < queueFamilyCount; i++) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      return i;
    }
  }
  return 0; // Fallback to 0 if not found
}

// Helper function to cleanup partially initialized state
static void CleanupOverlayState(OverlayState &state, VkDevice device,
                                DeviceDispatch *disp) {
  LayerLog("Vulkan Layer: Cleaning up partially initialized overlay state");

  if (disp && device != VK_NULL_HANDLE) {
    disp->fp_vkDeviceWaitIdle(device);

    // Cleanup framebuffers
    for (auto fb : state.framebuffers) {
      if (fb != VK_NULL_HANDLE) {
        disp->fp_vkDestroyFramebuffer(device, fb, nullptr);
      }
    }

    // Cleanup image views
    for (auto iv : state.imageViews) {
      if (iv != VK_NULL_HANDLE) {
        disp->fp_vkDestroyImageView(device, iv, nullptr);
      }
    }

    // Cleanup fences
    for (auto f : state.fences) {
      if (f != VK_NULL_HANDLE) {
        disp->fp_vkDestroyFence(device, f, nullptr);
      }
    }

    // Cleanup semaphores
    for (auto s : state.semaphores) {
      if (s != VK_NULL_HANDLE) {
        disp->fp_vkDestroySemaphore(device, s, nullptr);
      }
    }

    // Cleanup command pool
    if (state.commandPool != VK_NULL_HANDLE) {
      disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
    }

    // Cleanup render pass
    if (state.renderPass != VK_NULL_HANDLE) {
      disp->fp_vkDestroyRenderPass(device, state.renderPass, nullptr);
    }
  }

  // Cleanup adapter and metrics
  delete state.overlayAdapter;
  state.overlayAdapter = nullptr;

  delete state.metrics;
  state.metrics = nullptr;

  LayerLog("Vulkan Layer: Partial cleanup complete");
}

void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain,
                       VkFormat format, VkExtent2D extent, uint32_t imageCount,
                       VkImage *images, HWND window) {
  LayerLog(
      "Vulkan Layer: InitializeOverlay ENTRY(device=%p, images=%d, window=%p, "
      "size=%dx%d)",
      device, imageCount, window, extent.width, extent.height);

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

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  LayerLog("Vulkan Layer: InitializeOverlay - Got device dispatch: %p", disp);
  if (!disp) {
    LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
    return;
  }

  LayerLog("Vulkan Layer: InitializeOverlay - Getting instance dispatch...");
  InstanceDispatch *instDisp = VulkanLayerState::Get().GetInstanceDispatch(
      VulkanLayerState::Get().GetInstanceFromPhysicalDevice(
          disp->physicalDevice));
  LayerLog("Vulkan Layer: InitializeOverlay - Got instance dispatch: %p",
           instDisp);

  LayerLog("Vulkan Layer: InitializeOverlay - Creating OverlayState...");
  OverlayState state = {};
  state.device = device;
  state.physicalDevice = disp->physicalDevice;
  state.instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(
      disp->physicalDevice);
  state.format = format;
  state.extent = extent;
  state.swapchainImages.assign(images, images + imageCount);
  state.needsWindowHook = (window == nullptr);

  // NOTE: SystemMetricsCollector initialization is deferred until AFTER
  // overlay initialization succeeds to avoid race conditions
  uint32_t luidLow = 0, luidHigh = 0;
  VkDeviceSize vramTotal = 0;
  bool hasLuid = false;

  if (instDisp && instDisp->fp_vkGetPhysicalDeviceProperties2) {
    VkPhysicalDeviceIDProperties idProps = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &idProps;
    instDisp->fp_vkGetPhysicalDeviceProperties2(disp->physicalDevice, &props2);

    if (idProps.deviceLUIDValid) {
      luidLow = *(uint32_t *)&idProps.deviceLUID[0];
      luidHigh = *(uint32_t *)&idProps.deviceLUID[4];
      hasLuid = true;

      VkPhysicalDeviceMemoryProperties memProps = {};
      instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice,
                                                       &memProps);

      for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          vramTotal = std::max(vramTotal, memProps.memoryHeaps[i].size);
        }
      }
    }
  }

  // Find graphics queue family
  uint32_t graphicsQueueFamily =
      FindGraphicsQueueFamily(state.physicalDevice, instDisp);
  state.queueFamilyIndex = graphicsQueueFamily;
  LayerLog("Vulkan Layer: InitializeOverlay - Using graphics queue family %d",
           graphicsQueueFamily);

  // Create render pass (load existing content, don't clear)
  VkAttachmentDescription attachment = {};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  
  LayerLog("Vulkan Layer: InitializeOverlay - Render pass format=%d (VK_FORMAT_B8G8R8A8_UNORM=%d)", 
           format, 44);  // 44 is VK_FORMAT_B8G8R8A8_UNORM

  VkAttachmentReference colorRef = {0,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  // Add subpass dependencies for proper synchronization
  VkSubpassDependency dependency = {};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments = &attachment;
  rpInfo.subpassCount = 1;
  rpInfo.pSubpasses = &subpass;
  rpInfo.dependencyCount = 1;
  rpInfo.pDependencies = &dependency;

  LayerLog("Vulkan Layer: InitializeOverlay - Creating render pass...");
  if (disp->fp_vkCreateRenderPass(device, &rpInfo, nullptr,
                                  &state.renderPass) != VK_SUCCESS) {
    LayerLog("Vulkan Layer: Failed to create render pass");
    return;
  }
  LayerLog("Vulkan Layer: InitializeOverlay - Render pass created");

  // Create command pool
  LayerLog("Vulkan Layer: InitializeOverlay - Creating command pool "
           "(queueFamily=%d)...",
           graphicsQueueFamily);
  VkCommandPoolCreateInfo cpInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpInfo.queueFamilyIndex = graphicsQueueFamily;

  if (disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr,
                                   &state.commandPool) != VK_SUCCESS) {
    LayerLog("Vulkan Layer: Failed to create command pool");
    return;
  }
  LayerLog("Vulkan Layer: InitializeOverlay - Command pool created");

  // Create framebuffers, image views, fences, semaphores
  LayerLog("Vulkan Layer: InitializeOverlay - Creating %d framebuffers...",
           imageCount);
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

    VkFramebufferCreateInfo fbInfo = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = state.renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &state.imageViews[i];
    fbInfo.width = extent.width;
    fbInfo.height = extent.height;
    fbInfo.layers = 1;
    disp->fp_vkCreateFramebuffer(device, &fbInfo, nullptr,
                                 &state.framebuffers[i]);

    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &state.fences[i]);

    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    disp->fp_vkCreateSemaphore(device, &semInfo, nullptr, &state.semaphores[i]);
  }

  LayerLog("Vulkan Layer: InitializeOverlay - Allocating command buffers...");
  VkCommandBufferAllocateInfo cbInfo = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbInfo.commandPool = state.commandPool;
  cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbInfo.commandBufferCount = imageCount;
  disp->fp_vkAllocateCommandBuffers(device, &cbInfo,
                                    state.commandBuffers.data());

  // Create OverlayAdapter for this device
  LayerLog("Vulkan Layer: InitializeOverlay - Creating OverlayAdapter...");
  state.overlayAdapter = new OverlayAdapter();

  // Get queue for Vulkan backend
  VkQueue queue = VK_NULL_HANDLE;
  disp->fp_vkGetDeviceQueue(device, graphicsQueueFamily, 0, &queue);

  // Initialize with dispatch tables
  LayerLog("Vulkan Layer: InitializeOverlay - Calling InitVulkan...");
  bool initResult = state.overlayAdapter->InitVulkan(device, disp->physicalDevice, queue,
                                                     graphicsQueueFamily, disp, instDisp);
  LayerLog("Vulkan Layer: InitializeOverlay - InitVulkan returned %d", initResult);
  if (!initResult) {
    LayerLog("Vulkan Layer: [Error] Failed to initialize OverlayAdapter");
    CleanupOverlayState(state, device, disp);
    return;
  }
  LayerLog("Vulkan Layer: InitializeOverlay - InitVulkan succeeded");

  // Set up the adapter with metrics and IPC
  LayerLog("Vulkan Layer: InitializeOverlay - Creating PerformanceMetrics...");
  state.metrics = new PerformanceMetrics();
  LayerLog("Vulkan Layer: InitializeOverlay - PerformanceMetrics created, setting metrics...");
  state.overlayAdapter->SetMetrics(state.metrics);
  LayerLog("Vulkan Layer: InitializeOverlay - Metrics set, setting IPC client...");
  state.overlayAdapter->SetIPCClient(&g_IPCClient);
  LayerLog("Vulkan Layer: InitializeOverlay - IPC client set, setting graphics API...");
  state.overlayAdapter->SetGraphicsAPI("Vulkan");
  LayerLog("Vulkan Layer: InitializeOverlay - Graphics API set");

  // Create pipeline for the render pass - MUST succeed before marking
  // initialized
  LayerLog("Vulkan Layer: InitializeOverlay - Checking backend type...");
  LayerLog("Vulkan Layer: InitializeOverlay - overlayAdapter=%p", state.overlayAdapter);
  if (!state.overlayAdapter) {
    LayerLog("Vulkan Layer: [Error] overlayAdapter is null!");
    CleanupOverlayState(state, device, disp);
    return;
  }
  
  OverlayBackendType backendType = state.overlayAdapter->GetBackendType();
  LayerLog("Vulkan Layer: InitializeOverlay - Backend type=%d (expected Vulkan=%d)", 
           (int)backendType, (int)OverlayBackendType::Vulkan);
  
  if (backendType != OverlayBackendType::Vulkan) {
    LayerLog("Vulkan Layer: [Error] Backend type mismatch - expected Vulkan");
    CleanupOverlayState(state, device, disp);
    return;
  }

  LayerLog("Vulkan Layer: InitializeOverlay - Getting Vulkan backend...");
  auto *vkBackend = static_cast<CustomOverlay::VulkanBackend *>(
      state.overlayAdapter->GetBackend());
  LayerLog("Vulkan Layer: InitializeOverlay - vkBackend=%p", vkBackend);
  if (!vkBackend) {
    LayerLog("Vulkan Layer: [Error] Failed to get Vulkan backend");
    CleanupOverlayState(state, device, disp);
    return;
  }

  LayerLog("Vulkan Layer: InitializeOverlay - Creating pipeline for render pass...");
  if (!vkBackend->CreatePipelineForRenderPass(state.renderPass)) {
    LayerLog("Vulkan Layer: [Error] Failed to create pipeline for render pass");
    CleanupOverlayState(state, device, disp);
    return;
  }

  LayerLog("Vulkan Layer: InitializeOverlay - Pipeline created successfully");

  // Only mark as initialized AFTER all resources are created successfully
  state.initialized = true;
  g_OverlayStates[device] = state;

  // DEFERRED: Initialize SystemMetricsCollector only after overlay is ready
  // This prevents race conditions between the background thread and overlay
  // init
  if (hasLuid) {
    LayerLog(
        "Vulkan Layer: InitializeOverlay - Starting SystemMetricsCollector...");
    SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
    if (vramTotal > 0) {
      SystemMetricsCollector::Get().SetVRAMTotal(vramTotal);
    }
    LayerLog(
        "Vulkan Layer: InitializeOverlay - SystemMetricsCollector started");
  }

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
  LayerLog("Vulkan Layer: CleanupOverlay for device %p", device);

  // Shutdown metrics collector first
  SystemMetricsCollector::Get().Shutdown();

  DeviceDispatch *disp = VulkanLayerState::Get().GetDeviceDispatch(device);
  if (disp && device != VK_NULL_HANDLE) {
    LayerLog("Vulkan Layer: Waiting for device idle...");
    disp->fp_vkDeviceWaitIdle(device);

    // Safely cleanup all Vulkan resources
    for (auto fb : state.framebuffers) {
      if (fb != VK_NULL_HANDLE) {
        disp->fp_vkDestroyFramebuffer(device, fb, nullptr);
      }
    }
    for (auto iv : state.imageViews) {
      if (iv != VK_NULL_HANDLE) {
        disp->fp_vkDestroyImageView(device, iv, nullptr);
      }
    }
    for (auto f : state.fences) {
      if (f != VK_NULL_HANDLE) {
        disp->fp_vkDestroyFence(device, f, nullptr);
      }
    }
    for (auto s : state.semaphores) {
      if (s != VK_NULL_HANDLE) {
        disp->fp_vkDestroySemaphore(device, s, nullptr);
      }
    }

    if (state.commandPool != VK_NULL_HANDLE) {
      disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
    }
    if (state.renderPass != VK_NULL_HANDLE) {
      disp->fp_vkDestroyRenderPass(device, state.renderPass, nullptr);
    }
    LayerLog("Vulkan Layer: Vulkan resources destroyed");
  }

  // Cleanup adapter and metrics (adapter shutdown must happen after device
  // idle)
  delete state.overlayAdapter;
  state.overlayAdapter = nullptr;

  delete state.metrics;
  state.metrics = nullptr;

  g_OverlayStates.erase(it);
  LayerLog("Vulkan Layer: CleanupOverlay complete");
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
        LayerLog("Vulkan Layer: Deferred window hook successful (hwnd=%p)",
                 hwnd);
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

  VkCommandBufferBeginInfo beginInfo = {
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
    return;

  // Transition image to COLOR_ATTACHMENT_OPTIMAL
  // Use UNDEFINED as oldLayout since we don't know the previous state
  // The image will be fully overwritten by the render pass
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask =
      0; // No source access needed when coming from UNDEFINED
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout =
      VK_IMAGE_LAYOUT_UNDEFINED; // Safe assumption for swapchain
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = state.swapchainImages[imageIndex];
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                0, 0, nullptr, 0, nullptr, 1, &barrier);

  // Begin render pass
  VkRenderPassBeginInfo rpBeginInfo = {
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpBeginInfo.renderPass = state.renderPass;
  rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
  rpBeginInfo.renderArea.extent = state.extent;
  disp->fp_vkCmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  // Set render context for VulkanBackend and render via OverlayAdapter
  if (state.overlayAdapter && state.overlayAdapter->IsInitialized()) {
    // Verify backend type before casting
    if (state.overlayAdapter->GetBackendType() != OverlayBackendType::Vulkan) {
      LayerLog("Vulkan Layer: [Error] Backend type mismatch in RenderOverlay");
    } else {
      auto *vkBackend = static_cast<CustomOverlay::VulkanBackend *>(
          state.overlayAdapter->GetBackend());
      if (vkBackend) {
        vkBackend->SetRenderContext(cmd, state.renderPass,
                                    state.framebuffers[imageIndex],
                                    state.extent);
        // RenderOverlay will check if pipeline is ready
        state.overlayAdapter->RenderOverlay(state.extent.width,
                                            state.extent.height);
      } else {
        LayerLog("Vulkan Layer: [Error] Vulkan backend is null");
      }
    }
  } else {
    LayerLog(
        "Vulkan Layer: [Warning] OverlayAdapter not ready, skipping render");
  }

  // End render pass
  disp->fp_vkCmdEndRenderPass(cmd);

  // Transition to present
  VkImageMemoryBarrier presentBarrier = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  presentBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  presentBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  presentBarrier.image = state.swapchainImages[imageIndex];
  presentBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  disp->fp_vkCmdPipelineBarrier(cmd,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                nullptr, 0, nullptr, 1, &presentBarrier);

  disp->fp_vkEndCommandBuffer(cmd);

  // Submit
  VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;

  VkPipelineStageFlags waitStage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
