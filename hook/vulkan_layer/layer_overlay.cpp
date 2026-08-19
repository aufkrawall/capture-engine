/**
 * Vulkan Layer - Overlay Implementation
 *
 * Implements overlay rendering for Vulkan using the CustomOverlay system.
 * Frame management (command buffers, fences, semaphores) is handled here,
 * while content rendering is delegated to OverlayAdapter.
 */

#include "layer_overlay_internal.h"

#include "../common/dxgi_shared.h"
#include "../common/system_metrics.h"
#include "vulkan_presentation_color.h"

// Detect if a DLL is loaded from outside System32 (i.e. a DXVK replacement).
// NOTE: IsDllFromProject() from layer_main.h provides a more reliable version-
// verified check; this local helper is kept only as a fallback for callers that
// don't have the full version-check path available.
static bool IsDXVKDll(const char* dllName) {
    return IsDllFromProject(dllName, "dxvk");
}

static bool IsVKD3DDll(const char* dllName) {
    return IsDllFromProject(dllName, "vkd3d");
}

static const char* DetectTranslatedGraphicsAPIName() {
    const bool hasDxvkD3D11 = IsDXVKDll("d3d11.dll");
    const bool hasDxvkD3D9 = IsDXVKDll("d3d9.dll");
    const bool hasVkd3dD3D12 = IsVKD3DDll("d3d12.dll");
    const bool hasDX10 = (GetModuleHandleA("d3d10.dll") != nullptr || GetModuleHandleA("d3d10_1.dll") != nullptr);
    return DXGIShared::SelectTranslatedGraphicsAPIName(hasDxvkD3D11, hasDxvkD3D9, hasVkd3dD3D12, hasDX10);
}

std::mutex g_OverlayMutex;
std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

void SyncOverlayActiveFlagLocked() {
    bool overlayActive = false;
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - only existence of any initialized state matters
    for (const auto& entry : g_OverlayStates) {
        if (entry.second.initialized) {
            overlayActive = true;
            break;
        }
    }
    LayerIPC_SetOverlayActive(overlayActive);
}

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
    return 0;  // Fallback to 0 if not found
}

bool RecreateOverlayCommandResources(OverlayState& state, DeviceDispatch* disp, uint32_t queueFamilyIndex) {
    if (!disp || queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED || state.commandBuffers.empty()) {
        return false;
    }

    if (state.commandPool != VK_NULL_HANDLE) {
        if (disp->fp_vkDeviceWaitIdle) {
            disp->fp_vkDeviceWaitIdle(state.device);
        }
        disp->fp_vkDestroyCommandPool(state.device, state.commandPool, nullptr);
        state.commandPool = VK_NULL_HANDLE;
    }

    VkCommandPoolCreateInfo cpInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = queueFamilyIndex;

    if (disp->fp_vkCreateCommandPool(state.device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create overlay command pool for queue family %u", queueFamilyIndex);
        return false;
    }

    VkCommandBufferAllocateInfo cbInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = static_cast<uint32_t>(state.commandBuffers.size());
    if (disp->fp_vkAllocateCommandBuffers(state.device, &cbInfo, state.commandBuffers.data()) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to allocate overlay command buffers for queue family %u", queueFamilyIndex);
        disp->fp_vkDestroyCommandPool(state.device, state.commandPool, nullptr);
        state.commandPool = VK_NULL_HANDLE;
        return false;
    }

    state.queueFamilyIndex = queueFamilyIndex;
    return true;
}

// Helper function to cleanup partially initialized state
static void CleanupOverlayState(OverlayState& state, VkDevice device, DeviceDispatch* disp) {
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

void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, VkColorSpaceKHR colorSpace,
                       VkExtent2D extent,
                       uint32_t imageCount, VkImage* images, HWND window) {
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
        LayerLog(
            "Vulkan Layer: [Warning] No window provided for overlay. Will "
            "attempt deferred hook.");
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    LayerLog("Vulkan Layer: InitializeOverlay - Got device dispatch: %p", disp);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }

    auto existing = g_OverlayStates.find(device);
    if (existing != g_OverlayStates.end()) {
        LayerLog(
            "Vulkan Layer: InitializeOverlay - Existing state found, cleaning "
            "it before re-init");
        CleanupOverlayState(existing->second, device, disp);
        g_OverlayStates.erase(existing);
        SyncOverlayActiveFlagLocked();
    }
    const auto presentationEncoding = ce::presentation_color::ResolveVulkan(format, colorSpace);
    if (presentationEncoding == ce::presentation_color::Encoding::Unsupported) {
        LayerLog("Vulkan Layer: [Error] Unsupported overlay presentation contract format=%d colorSpace=%d", format,
                 colorSpace);
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
    state.colorSpace = colorSpace;
    state.extent = extent;
    state.swapchainImages.assign(images, images + imageCount);
    state.needsWindowHook = (window == nullptr);

    // NOTE: SystemMetricsCollector initialization is deferred until AFTER
    // overlay initialization succeeds to avoid race conditions
    uint32_t luidLow = 0, luidHigh = 0;
    VkDeviceSize vramTotal = 0;
    bool hasLuid = false;

    if (instDisp && instDisp->fp_vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        instDisp->fp_vkGetPhysicalDeviceProperties2(disp->physicalDevice, &props2);

        if (idProps.deviceLUIDValid) {
            luidLow = *(uint32_t*)&idProps.deviceLUID[0];
            luidHigh = *(uint32_t*)&idProps.deviceLUID[4];
            hasLuid = true;

            VkPhysicalDeviceMemoryProperties memProps = {};
            instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);

            for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    vramTotal = std::max(vramTotal, memProps.memoryHeaps[i].size);
                }
            }
        }
    }

    // Find graphics queue family. Prefer the family CE reserved its own queue
    // in, so the command pool built here is already the one the per-present
    // submit will use even when the game presents from a non-graphics queue.
    uint32_t graphicsQueueFamily = (disp->overlayQueue != VK_NULL_HANDLE &&
                                    disp->overlayQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED)
                                       ? disp->overlayQueueFamilyIndex
                                       : FindGraphicsQueueFamily(state.physicalDevice, instDisp);
    state.queueFamilyIndex = graphicsQueueFamily;
    LayerLog("Vulkan Layer: InitializeOverlay - Using graphics queue family %d (reservedOverlayQueue=%d)",
             graphicsQueueFamily, disp->overlayQueue != VK_NULL_HANDLE ? 1 : 0);

    // Create render pass (load existing content, don't clear)
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    LayerLog(
        "Vulkan Layer: InitializeOverlay - Render pass format=%d "
        "(VK_FORMAT_B8G8R8A8_UNORM=%d)",
        format, 44);  // 44 is VK_FORMAT_B8G8R8A8_UNORM

    VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
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
    if (disp->fp_vkCreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        LayerLog("Vulkan Layer: Failed to create render pass");
        return;
    }
    LayerLog("Vulkan Layer: InitializeOverlay - Render pass created");

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

    LayerLog("Vulkan Layer: InitializeOverlay - Creating command pool (queueFamily=%d)...", graphicsQueueFamily);
    if (!RecreateOverlayCommandResources(state, disp, graphicsQueueFamily)) {
        CleanupOverlayState(state, device, disp);
        return;
    }
    LayerLog("Vulkan Layer: InitializeOverlay - Command pool created");

    // Create OverlayAdapter for this device
    LayerLog("Vulkan Layer: InitializeOverlay - Creating OverlayAdapter...");
    state.overlayAdapter = new OverlayAdapter();

    // Get queue for Vulkan backend. CE's reserved queue is the only graphics
    // queue CE owns outright; the game's queue 0 is externally synchronized by
    // the game and must not be submitted to from the layer.
    VkQueue queue = disp->overlayQueue;
    uint32_t backendQueueFamily = disp->overlayQueueFamilyIndex;
    if (queue == VK_NULL_HANDLE) {
        disp->fp_vkGetDeviceQueue(device, graphicsQueueFamily, 0, &queue);
        backendQueueFamily = graphicsQueueFamily;
    }

    // Initialize with dispatch tables
    LayerLog("Vulkan Layer: InitializeOverlay - Calling InitVulkan...");
    bool initResult =
        state.overlayAdapter->InitVulkan(device, disp->physicalDevice, queue, backendQueueFamily, disp, instDisp);
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
    LayerLog(
        "Vulkan Layer: InitializeOverlay - PerformanceMetrics created, "
        "setting metrics...");
    state.overlayAdapter->SetMetrics(state.metrics);
    LayerLog("Vulkan Layer: InitializeOverlay - Metrics set, setting IPC client...");
    state.overlayAdapter->SetIPCClient(&g_IPCClient);
    LayerLog(
        "Vulkan Layer: InitializeOverlay - IPC client set, setting graphics "
        "API...");
    // Prefer the active translated API over merely-present helper DLLs. Some DXVK
    // game folders ship multiple wrapper DLLs, but DX11/VKD3D should still label
    // according to the path that actually owns rendering.
    const char* apiName = DetectTranslatedGraphicsAPIName();
    state.overlayAdapter->SetGraphicsAPI(apiName);

    const bool isHDR = ce::presentation_color::IsHDR(presentationEncoding);
    int rtvFormat = 0;
    if (state.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || state.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
        rtvFormat = 24;  // Maps to DXGI_FORMAT_R10G10B10A2_UNORM for HDR10/PQ
    state.overlayAdapter->SetHDR(isHDR, rtvFormat);
    LayerLog("Vulkan Layer: Overlay presentation contract format=%d colorSpace=%d encoding=%s", state.format,
             state.colorSpace, ce::presentation_color::Describe(presentationEncoding));

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
    LayerLog("Vulkan Layer: InitializeOverlay - Backend type=%d (expected Vulkan=%d)", (int)backendType,
             (int)OverlayBackendType::Vulkan);

    if (backendType != OverlayBackendType::Vulkan) {
        LayerLog("Vulkan Layer: [Error] Backend type mismatch - expected Vulkan");
        CleanupOverlayState(state, device, disp);
        return;
    }

    LayerLog("Vulkan Layer: InitializeOverlay - Getting Vulkan backend...");
    auto* vkBackend = static_cast<CustomOverlay::VulkanBackend*>(state.overlayAdapter->GetBackend());
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
    SyncOverlayActiveFlagLocked();

    // DEFERRED: Initialize SystemMetricsCollector only after overlay is ready
    // This prevents race conditions between the background thread and overlay
    // init
    if (hasLuid) {
        LayerLog("Vulkan Layer: InitializeOverlay - Starting SystemMetricsCollector...");
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
        if (vramTotal > 0) {
            SystemMetricsCollector::Get().SetVRAMTotal(vramTotal);
        }
        LayerLog("Vulkan Layer: InitializeOverlay - SystemMetricsCollector started");
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

PerformanceMetrics* GetOverlayPerformanceMetrics(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it != g_OverlayStates.end() && it->second.initialized) {
        return it->second.metrics;
    }
    return nullptr;
}

void CleanupOverlay(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end())
        return;

    OverlayState& state = it->second;
    LayerLog("Vulkan Layer: CleanupOverlay for device %p", device);

    // Shutdown metrics collector first
    SystemMetricsCollector::Get().Shutdown();

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
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
    SyncOverlayActiveFlagLocked();
    LayerLog("Vulkan Layer: CleanupOverlay complete");
}
