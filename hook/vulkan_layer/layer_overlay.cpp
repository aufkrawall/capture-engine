#include "vulkan_layer.h"
#include "layer_main.h"
#include "../common/overlay.h"
#include "../common/ipc_client.h"
#include "../common/performance_metrics.h"
#include "../common/system_metrics.h"
#include <vector>
#include <chrono>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include <backends/imgui_impl_win32.h>
#include "../common/input_manager.h"

// Overlay state per device
struct OverlayState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent = {0, 0};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    PerformanceMetrics* metrics = nullptr;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;
static bool g_ImGuiInitialized = false;

// Custom Vulkan function loader for ImGui
static PFN_vkVoidFunction ImGuiLoader(const char* function_name, void* user_data) {
    DeviceDispatch* disp = (DeviceDispatch*)user_data;
    if (!disp) return nullptr;
    
    // 1. Try Device Level
    PFN_vkVoidFunction fn = disp->fp_vkGetDeviceProcAddr(disp->device, function_name);
    if (fn) return fn;
    
    // 2. Try Instance Level
    VkInstance instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice);
    if (instance != VK_NULL_HANDLE) {
        InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(instance);
        if (instDisp && instDisp->fp_vkGetInstanceProcAddr) {
            fn = instDisp->fp_vkGetInstanceProcAddr(instance, function_name);
            if (fn) return fn;
        }
    }
    
    // 3. Fallback to global GetInstanceProcAddr
    static PFN_vkGetInstanceProcAddr g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetInstanceProcAddr");
    if (g_gipa) return g_gipa(VK_NULL_HANDLE, function_name);
    
    return nullptr;
}

void InitializeOverlay(VkDevice device, VkSwapchainKHR swapchain, VkFormat format, 
                       VkExtent2D extent, uint32_t imageCount, VkImage* images, HWND window)
{
    LayerLog("Vulkan Layer: InitializeOverlay(device=%p, images=%d, window=%p, size=%dx%d)", device, imageCount, window, extent.width, extent.height);
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    if (window) {
        InputManager::Get().HookWindow(window);
        g_SharedOverlay.InitImGui(window);
    } else {
        LayerLog("Vulkan Layer: [Warning] No window provided for overlay. Hooking might be incomplete.");
    }
    
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) {
        LayerLog("Vulkan Layer: [Error] No dispatch for device %p", device);
        return;
    }
    
    OverlayState state = {};
    state.device = device;
    state.physicalDevice = disp->physicalDevice;
    // Get the instance from physical device
    state.instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(disp->physicalDevice);

    // Initialize SystemMetricsCollector with GPU LUID for system stats (GPU load, VRAM)
    InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(state.instance);
    if (instDisp && instDisp->fp_vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &idProps;
        instDisp->fp_vkGetPhysicalDeviceProperties2(disp->physicalDevice, &props2);

        if (idProps.deviceLUIDValid) {
            // LUID is 8 bytes: first 4 bytes = LowPart, next 4 bytes = HighPart
            uint32_t luidLow = *(uint32_t*)&idProps.deviceLUID[0];
            uint32_t luidHigh = *(uint32_t*)&idProps.deviceLUID[4];
            SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
            LayerLog("Vulkan Layer: SystemMetricsCollector initialized with LUID: %08X%08X", luidHigh, luidLow);

            // Set VRAM Total from physical device memory properties
            VkPhysicalDeviceMemoryProperties memProps = {};
            instDisp->fp_vkGetPhysicalDeviceMemoryProperties(disp->physicalDevice, &memProps);

            // Find the device local heap with the highest memory
            VkDeviceSize maxHeapSize = 0;
            for (uint32_t i = 0; i < memProps.memoryHeapCount; i++) {
                if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                    if (memProps.memoryHeaps[i].size > maxHeapSize) {
                        maxHeapSize = memProps.memoryHeaps[i].size;
                    }
                }
            }
            if (maxHeapSize > 0) {
                SystemMetricsCollector::Get().SetVRAMTotal(maxHeapSize);
                LayerLog("Vulkan Layer: VRAM Total set to: %llu MB", maxHeapSize / (1024 * 1024));
            }
        }
    }

    state.extent = extent;
    state.format = format;
    state.swapchainImages.assign(images, images + imageCount);
    
    // Create descriptor pool
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 100 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 }
    };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1100;
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    
    if (disp->fp_vkCreateDescriptorPool(device, &pool_info, nullptr, &state.descriptorPool) != VK_SUCCESS) return;

    // Create render pass
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    
    VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    
    VkRenderPassCreateInfo rpInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    
    if (disp->fp_vkCreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) return;
    
    // Create command pool
    VkCommandPoolCreateInfo cpInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = 0; 
    
    if (disp->fp_vkCreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) return;
    
    state.imageViews.resize(imageCount);
    state.framebuffers.resize(imageCount);
    state.commandBuffers.resize(imageCount);
    
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo ivInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivInfo.image = images[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format = format;
        ivInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        disp->fp_vkCreateImageView(device, &ivInfo, nullptr, &state.imageViews[i]);
        
        VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = state.renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &state.imageViews[i];
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        disp->fp_vkCreateFramebuffer(device, &fbInfo, nullptr, &state.framebuffers[i]);
    }
    
    VkCommandBufferAllocateInfo cbInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    disp->fp_vkAllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());

    // Set overlay properties regardless of initialization state
    // (InitImGui may have been called with a window above, so IsInitialized() could be true)
    g_SharedOverlay.SetGraphicsAPI("Vulkan");
    g_SharedOverlay.SetIPCClient(&g_IPCClient);

    // Initialize headless ImGui if not already initialized with a window
    if (!g_SharedOverlay.IsInitialized()) {
        g_SharedOverlay.InitImGuiHeadless();
    }

    state.metrics = new PerformanceMetrics();
    state.initialized = true;
    g_OverlayStates[device] = state;
}

void CleanupOverlay(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end()) return;
    
    OverlayState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp) {
        disp->fp_vkDeviceWaitIdle(device);
        for (auto fb : state.framebuffers) disp->fp_vkDestroyFramebuffer(device, fb, nullptr);
        for (auto iv : state.imageViews) disp->fp_vkDestroyImageView(device, iv, nullptr);
        disp->fp_vkDestroyCommandPool(device, state.commandPool, nullptr);
        disp->fp_vkDestroyRenderPass(device, state.renderPass, nullptr);
        disp->fp_vkDestroyDescriptorPool(device, state.descriptorPool, nullptr);
    }
    delete state.metrics;
    g_OverlayStates.erase(it);
}

void RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex, 
                   VkSemaphore waitSemaphore, VkSemaphore signalSemaphore)
{
    std::lock_guard<std::mutex> lock(g_OverlayMutex);
    auto it = g_OverlayStates.find(device);
    if (it == g_OverlayStates.end() || !it->second.initialized) return;
    
    OverlayState& state = it->second;
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp) return;
    
    if (!g_ImGuiInitialized) {
        ImGui_ImplVulkan_LoadFunctions(ImGuiLoader, disp);
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = state.instance;
        init_info.PhysicalDevice = disp->physicalDevice;
        init_info.Device = device;
        init_info.QueueFamily = VulkanLayerState::Get().GetQueueFamilyIndex(queue);
        init_info.Queue = queue;
        init_info.DescriptorPool = state.descriptorPool;
        init_info.RenderPass = state.renderPass;
        init_info.MinImageCount = 2;
        init_info.ImageCount = (uint32_t)state.swapchainImages.size();
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (ImGui_ImplVulkan_Init(&init_info)) {
            ImGui_ImplVulkan_CreateFontsTexture();
            g_ImGuiInitialized = true;
        }
    }
    
    if (state.metrics) state.metrics->Update(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    g_SharedOverlay.SetMetrics(state.metrics);
    
    ImGui_ImplVulkan_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)state.extent.width, (float)state.extent.height);
    
    g_SharedOverlay.BeginFrame();
    g_SharedOverlay.RenderUI();
    g_SharedOverlay.EndFrame();
    
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    
    if (disp->fp_vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS) {
        VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.image = state.swapchainImages[imageIndex];
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        
        disp->fp_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            
        VkRenderPassBeginInfo rpBeginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rpBeginInfo.renderPass = state.renderPass;
        rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
        rpBeginInfo.renderArea.extent = state.extent;
        disp->fp_vkCmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        disp->fp_vkCmdEndRenderPass(cmd);
        disp->fp_vkEndCommandBuffer(cmd);
        
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        disp->fp_vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        // Removed vkQueueWaitIdle - it was causing CPU-GPU serialization and GPU underutilization.
        // The overlay render is asynchronous and doesn't need to complete before present returns.
    }
}
