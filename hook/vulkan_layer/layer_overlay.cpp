#include "layer_main.h"
#include "../common/overlay.h"
#include "../common/ipc_client.h"
#include <vector>
#include <chrono>
#include <string>

#include "imgui.h"
// No prototype loader needed since we provide our own
#include "backends/imgui_impl_vulkan.h"

extern IPCClient g_IPCClient;

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
    
    // FPS tracking
    uint64_t frameCount = 0;
    std::chrono::steady_clock::time_point lastFpsTime;
    float currentFps = 0.0f;
    uint64_t framesThisSecond = 0;
};

static std::mutex g_OverlayMutex;
static std::unordered_map<VkDevice, OverlayState> g_OverlayStates;
static bool g_ImGuiInitialized = false;

// Custom Vulkan function loader for ImGui
static PFN_vkVoidFunction ImGuiLoader(const char* function_name, void* user_data) {
    CEDeviceDispatch* disp = (CEDeviceDispatch*)user_data;
    if (!disp) return nullptr;

    // Try device-level first
    PFN_vkVoidFunction fn = disp->GetDeviceProcAddr(disp->device, function_name);
    if (fn) return fn;

    // Try instance-level
    if (disp->instance) {
        CEInstanceDispatch* instDisp = GetInstanceDispatch(disp->instance);
        if (instDisp && instDisp->GetInstanceProcAddr) {
            return instDisp->GetInstanceProcAddr(disp->instance, function_name);
        }
    }
    return nullptr;
}

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
    state.instance = disp->instance;
    state.physicalDevice = disp->physicalDevice;
    state.extent = extent;
    state.format = format;
    state.lastFpsTime = std::chrono::steady_clock::now();
    
    // Store swapchain images
    state.swapchainImages.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        state.swapchainImages[i] = images[i];
    }
    
    // Create descriptor pool (required for ImGui)
    // Size based on typical usage
    VkDescriptorPoolSize pool_sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 100 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 100 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 100 }
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 100 * IM_ARRAYSIZE(pool_sizes);
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    
    if (disp->CreateDescriptorPool(device, &pool_info, nullptr, &state.descriptorPool) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to create descriptor pool");
        return;
    }

    // Create render pass - simple pass that loads and stores
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; 
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // Transitoned by us before RP
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Transitioned by RP end
    
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
    
    if (disp->CreateRenderPass(device, &rpInfo, nullptr, &state.renderPass) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to create render pass");
        disp->DestroyDescriptorPool(device, state.descriptorPool, nullptr);
        return;
    }
    
    // Create command pool
    VkCommandPoolCreateInfo cpInfo = {};
    cpInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpInfo.queueFamilyIndex = 0; // Usage generic queue - ideally should match presentation queue
    // Note: We don't know the exact queue family here easily without query, but typically 0 is graphics.
    // For robustness we should query it, but layer passes just queue handle. 
    // We assume the queue passed to Present is capable of graphics (which it must be for swapchain usually).
    
    if (disp->CreateCommandPool(device, &cpInfo, nullptr, &state.commandPool) != VK_SUCCESS) {
        LayerLog("Layer Overlay: Failed to create command pool");
        disp->DestroyRenderPass(device, state.renderPass, nullptr);
        disp->DestroyDescriptorPool(device, state.descriptorPool, nullptr);
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
        
        disp->CreateFramebuffer(device, &fbInfo, nullptr, &state.framebuffers[i]);
    }
    
    // Allocate command buffers
    VkCommandBufferAllocateInfo cbInfo = {};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = state.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = imageCount;
    
    disp->AllocateCommandBuffers(device, &cbInfo, state.commandBuffers.data());
    
    // Initialize ImGui Headless (Common)
    if (!g_SharedOverlay.IsInitialized()) { // Check initialized 
        g_SharedOverlay.InitImGuiHeadless();
        g_SharedOverlay.SetGraphicsAPI("Vulkan");
        if (LayerIPC_IsConnected()) { // Ensure IPC is ready if possible
             g_SharedOverlay.SetIPCClient(&g_IPCClient);
        }
    }
    
    state.initialized = true;
    g_OverlayStates[device] = state;
    
    LayerLog("Layer Overlay: Initialized for device %p (%dx%d)", device, extent.width, extent.height);
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
        
        if (g_ImGuiInitialized) {
            ImGui_ImplVulkan_Shutdown();
            g_SharedOverlay.ShutdownImGui();
            g_ImGuiInitialized = false;
        }

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
        if (state.descriptorPool && disp->DestroyDescriptorPool) {
            disp->DestroyDescriptorPool(device, state.descriptorPool, nullptr);
        }
    }
    
    g_OverlayStates.erase(it);
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

    // Lazy initialization of ImGui Vulkan backend (need Queue)
    if (!g_ImGuiInitialized) {
        LayerLog("Layer Overlay: Initializing ImGui Backend...");
        g_SharedOverlay.InitImGuiHeadless(); // Ensure context exists
        g_SharedOverlay.SetGraphicsAPI("Vulkan");
        
        // Load functions
        LayerLog("Layer Overlay: Loading ImGui functions...");
        ImGui_ImplVulkan_LoadFunctions(ImGuiLoader, disp);
        
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.Instance = state.instance;
        init_info.PhysicalDevice = state.physicalDevice;
        init_info.Device = device;
        
        uint32_t familyIndex = GetQueueFamilyIndex(queue);
        if (familyIndex == VK_QUEUE_FAMILY_IGNORED) {
             LayerLog("Layer Overlay: Loading queue family index failed!");
             // Fallback or abort? Abort ImGui init to prevent crash
             // But let's log and try 0 ?? No, 0 might be compute.
             // We can't proceed safely.
             return; 
        }
        LayerLog("Layer Overlay: Using Queue Family %d", familyIndex);
        
        init_info.QueueFamily = familyIndex;
        init_info.Queue = queue;
        init_info.DescriptorPool = state.descriptorPool;
        init_info.RenderPass = state.renderPass;
        init_info.MinImageCount = 2;
        init_info.ImageCount = (uint32_t)state.swapchainImages.size();
        init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        
        LayerLog("Layer Overlay: Calling ImGui_ImplVulkan_Init...");
        if (ImGui_ImplVulkan_Init(&init_info)) {
            LayerLog("Layer Overlay: ImGui_ImplVulkan_Init succeeded");
            // Upload fonts
            // ImGui_ImplVulkan_CreateFontsTexture() creates its own command buffer and submits it
            ImGui_ImplVulkan_CreateFontsTexture();
            
            // Note: We don't destroy font texture here, ImGui manages it or we destroy at shutdown
            // ImGui_ImplVulkan_DestroyFontsTexture(); // This destroys the CPU copy, which is fine
            
            g_ImGuiInitialized = true;
            LayerLog("Layer: ImGui Initialized");
        }
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
    
    // Update IPC values
    LayerIPC_UpdateFrameTiming(state.frameCount, state.currentFps, state.currentFps);
    
    // Start ImGui Frame
    ImGui_ImplVulkan_NewFrame();
    
    // Manually setup IO for headless
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)state.extent.width, (float)state.extent.height);
    // Calc standard delta time
    auto current_time = std::chrono::steady_clock::now();
    static auto last_time = current_time;
    float delta = std::chrono::duration<float>(current_time - last_time).count();
    last_time = current_time;
    if (delta <= 0.0f) delta = 0.001f;
    io.DeltaTime = delta;
    
    // Ensure IPC client is set (might have connected late)
    if (LayerIPC_IsConnected()) {
        g_SharedOverlay.SetIPCClient(&g_IPCClient);
    }

    g_SharedOverlay.BeginFrame(); // Calls ImGui::NewFrame() internally if headless
    g_SharedOverlay.RenderUI();
    g_SharedOverlay.EndFrame();   // Calls ImGui::Render()
    
    // Record Command Buffer
    VkCommandBuffer cmd = state.commandBuffers[imageIndex];
    if (!cmd) return;
    
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    if (disp->BeginCommandBuffer && disp->BeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS) {
        
        // Barrier: Present -> Color Att
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
        
        disp->CmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
            
        // Render Pass
        VkRenderPassBeginInfo rpBeginInfo = {};
        rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBeginInfo.renderPass = state.renderPass;
        rpBeginInfo.framebuffer = state.framebuffers[imageIndex];
        rpBeginInfo.renderArea.extent = state.extent;
        
        disp->CmdBeginRenderPass(cmd, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        
        // Draw ImGui
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        
        disp->CmdEndRenderPass(cmd);
        
        disp->EndCommandBuffer(cmd);
        
        // Submit
        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        
        disp->QueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        
        // Ensure completion before present (simple sync)
        disp->QueueWaitIdle(queue); 
    }
}
