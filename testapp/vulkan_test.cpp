// Vulkan Test App for Capture + FPS Limiter Testing
// Uses DYNAMIC function loading like real games
//
// Settings read from testappconfig.ini (next to exe)
// Command line overrides: vulkan_test.exe [width] [height] [gpu_load_passes]
//
// Build with: python build.py

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <shellscalingapi.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testapp_common.h"

#pragma comment(lib, "shcore.lib")

// Configurable settings (set from config file or command line)
static int g_WindowWidth = 3840;
static int g_WindowHeight = 2160;
static int g_GpuLoadPasses = 40;  // Default ~70% GPU at 4K 120fps
static int g_VSync = 0;           // 0 = off (IMMEDIATE), 1 = on (FIFO)

// Read config from testappconfig.ini
void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
}

const wchar_t* WINDOW_CLASS = L"CaptureTestVulkan";
const int MAX_FRAMES_IN_FLIGHT = 2;

// Dynamically loaded Vulkan functions
static PFN_vkGetInstanceProcAddr pfn_vkGetInstanceProcAddr = nullptr;
static PFN_vkCreateInstance pfn_vkCreateInstance = nullptr;
static PFN_vkDestroyInstance pfn_vkDestroyInstance = nullptr;
static PFN_vkEnumeratePhysicalDevices pfn_vkEnumeratePhysicalDevices = nullptr;
static PFN_vkGetPhysicalDeviceProperties pfn_vkGetPhysicalDeviceProperties = nullptr;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pfn_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
static PFN_vkCreateDevice pfn_vkCreateDevice = nullptr;
static PFN_vkDestroyDevice pfn_vkDestroyDevice = nullptr;
static PFN_vkGetDeviceQueue pfn_vkGetDeviceQueue = nullptr;
static PFN_vkDeviceWaitIdle pfn_vkDeviceWaitIdle = nullptr;

// Surface/Swapchain functions
static PFN_vkCreateWin32SurfaceKHR pfn_vkCreateWin32SurfaceKHR = nullptr;
static PFN_vkDestroySurfaceKHR pfn_vkDestroySurfaceKHR = nullptr;
static PFN_vkCreateSwapchainKHR pfn_vkCreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR pfn_vkDestroySwapchainKHR = nullptr;
static PFN_vkGetSwapchainImagesKHR pfn_vkGetSwapchainImagesKHR = nullptr;
static PFN_vkAcquireNextImageKHR pfn_vkAcquireNextImageKHR = nullptr;
static PFN_vkQueuePresentKHR pfn_vkQueuePresentKHR = nullptr;

// Device functions (loaded via vkGetDeviceProcAddr)
static PFN_vkGetDeviceProcAddr pfn_vkGetDeviceProcAddr = nullptr;
static PFN_vkCreateImageView pfn_vkCreateImageView = nullptr;
static PFN_vkDestroyImageView pfn_vkDestroyImageView = nullptr;
static PFN_vkCreateRenderPass pfn_vkCreateRenderPass = nullptr;
static PFN_vkDestroyRenderPass pfn_vkDestroyRenderPass = nullptr;
static PFN_vkCreateFramebuffer pfn_vkCreateFramebuffer = nullptr;
static PFN_vkDestroyFramebuffer pfn_vkDestroyFramebuffer = nullptr;
static PFN_vkCreateCommandPool pfn_vkCreateCommandPool = nullptr;
static PFN_vkDestroyCommandPool pfn_vkDestroyCommandPool = nullptr;
static PFN_vkAllocateCommandBuffers pfn_vkAllocateCommandBuffers = nullptr;
static PFN_vkBeginCommandBuffer pfn_vkBeginCommandBuffer = nullptr;
static PFN_vkEndCommandBuffer pfn_vkEndCommandBuffer = nullptr;
static PFN_vkResetCommandBuffer pfn_vkResetCommandBuffer = nullptr;
static PFN_vkCmdBeginRenderPass pfn_vkCmdBeginRenderPass = nullptr;
static PFN_vkCmdEndRenderPass pfn_vkCmdEndRenderPass = nullptr;
static PFN_vkCmdSetViewport pfn_vkCmdSetViewport = nullptr;
static PFN_vkCmdSetScissor pfn_vkCmdSetScissor = nullptr;
static PFN_vkCreateSemaphore pfn_vkCreateSemaphore = nullptr;
static PFN_vkDestroySemaphore pfn_vkDestroySemaphore = nullptr;
static PFN_vkCreateFence pfn_vkCreateFence = nullptr;
static PFN_vkDestroyFence pfn_vkDestroyFence = nullptr;
static PFN_vkWaitForFences pfn_vkWaitForFences = nullptr;
static PFN_vkResetFences pfn_vkResetFences = nullptr;
static PFN_vkQueueSubmit pfn_vkQueueSubmit = nullptr;

// Vulkan objects
static HMODULE g_VulkanLib = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static VkQueue g_GraphicsQueue = VK_NULL_HANDLE;
static VkSurfaceKHR g_Surface = VK_NULL_HANDLE;
static VkSwapchainKHR g_Swapchain = VK_NULL_HANDLE;
static std::vector<VkImage> g_SwapchainImages;
static std::vector<VkImageView> g_SwapchainImageViews;
static std::vector<VkFramebuffer> g_Framebuffers;
static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
static VkCommandPool g_CommandPool = VK_NULL_HANDLE;
static std::vector<VkCommandBuffer> g_CommandBuffers;
static std::vector<VkSemaphore> g_ImageAvailableSemaphores;
static std::vector<VkSemaphore> g_RenderFinishedSemaphores;
static std::vector<VkFence> g_InFlightFences;
static uint32_t g_GraphicsQueueFamily = 0;
static VkFormat g_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
static VkExtent2D g_SwapchainExtent = {0, 0};  // Set in main() from command line args

// Animation state
static auto g_StartTime = std::chrono::high_resolution_clock::now();
static bool g_Running = true;
static int g_CurrentFrame = 0;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hWnd);
            }
            return 0;
        default:
            break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

bool LoadVulkanLibrary() {
    printf("Loading vulkan-1.dll dynamically...\n");
    g_VulkanLib = LoadLibraryA("vulkan-1.dll");
    if (!g_VulkanLib) {
        printf("Failed to load vulkan-1.dll\n");
        return false;
    }

    // Get the entry point - this is where hooks intercept!
    pfn_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(g_VulkanLib, "vkGetInstanceProcAddr");
    if (!pfn_vkGetInstanceProcAddr) {
        printf("Failed to get vkGetInstanceProcAddr\n");
        return false;
    }

    printf("Got vkGetInstanceProcAddr: %p\n", (void*)pfn_vkGetInstanceProcAddr);

    // Get pre-instance functions
    pfn_vkCreateInstance = (PFN_vkCreateInstance)pfn_vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!pfn_vkCreateInstance) {
        printf("Failed to get vkCreateInstance\n");
        return false;
    }

    printf("Vulkan library loaded successfully\n");
    return true;
}

bool LoadInstanceFunctions() {
    printf("Loading instance-level functions via vkGetInstanceProcAddr...\n");

#define LOAD_INSTANCE_FUNC(name)                                           \
    pfn_##name = (PFN_##name)pfn_vkGetInstanceProcAddr(g_Instance, #name); \
    if (!pfn_##name) {                                                     \
        printf("Failed to get " #name "\n");                               \
        return false;                                                      \
    }

    LOAD_INSTANCE_FUNC(vkDestroyInstance);
    LOAD_INSTANCE_FUNC(vkEnumeratePhysicalDevices);
    LOAD_INSTANCE_FUNC(vkGetPhysicalDeviceProperties);
    LOAD_INSTANCE_FUNC(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FUNC(vkCreateDevice);
    LOAD_INSTANCE_FUNC(vkGetDeviceProcAddr);
    LOAD_INSTANCE_FUNC(vkCreateWin32SurfaceKHR);
    LOAD_INSTANCE_FUNC(vkDestroySurfaceKHR);

#undef LOAD_INSTANCE_FUNC

    printf("Instance functions loaded\n");
    return true;
}

bool LoadDeviceFunctions() {
    printf("Loading device-level functions via vkGetDeviceProcAddr...\n");

#define LOAD_DEVICE_FUNC(name)                                         \
    pfn_##name = (PFN_##name)pfn_vkGetDeviceProcAddr(g_Device, #name); \
    if (!pfn_##name) {                                                 \
        printf("Failed to get " #name "\n");                           \
        return false;                                                  \
    }

    LOAD_DEVICE_FUNC(vkDestroyDevice);
    LOAD_DEVICE_FUNC(vkGetDeviceQueue);
    LOAD_DEVICE_FUNC(vkDeviceWaitIdle);
    LOAD_DEVICE_FUNC(vkCreateSwapchainKHR);
    LOAD_DEVICE_FUNC(vkDestroySwapchainKHR);
    LOAD_DEVICE_FUNC(vkGetSwapchainImagesKHR);
    LOAD_DEVICE_FUNC(vkAcquireNextImageKHR);
    LOAD_DEVICE_FUNC(vkQueuePresentKHR);
    LOAD_DEVICE_FUNC(vkCreateImageView);
    LOAD_DEVICE_FUNC(vkDestroyImageView);
    LOAD_DEVICE_FUNC(vkCreateRenderPass);
    LOAD_DEVICE_FUNC(vkDestroyRenderPass);
    LOAD_DEVICE_FUNC(vkCreateFramebuffer);
    LOAD_DEVICE_FUNC(vkDestroyFramebuffer);
    LOAD_DEVICE_FUNC(vkCreateCommandPool);
    LOAD_DEVICE_FUNC(vkDestroyCommandPool);
    LOAD_DEVICE_FUNC(vkAllocateCommandBuffers);
    LOAD_DEVICE_FUNC(vkBeginCommandBuffer);
    LOAD_DEVICE_FUNC(vkEndCommandBuffer);
    LOAD_DEVICE_FUNC(vkResetCommandBuffer);
    LOAD_DEVICE_FUNC(vkCmdBeginRenderPass);
    LOAD_DEVICE_FUNC(vkCmdEndRenderPass);
    LOAD_DEVICE_FUNC(vkCmdSetViewport);
    LOAD_DEVICE_FUNC(vkCmdSetScissor);
    LOAD_DEVICE_FUNC(vkCreateSemaphore);
    LOAD_DEVICE_FUNC(vkDestroySemaphore);
    LOAD_DEVICE_FUNC(vkCreateFence);
    LOAD_DEVICE_FUNC(vkDestroyFence);
    LOAD_DEVICE_FUNC(vkWaitForFences);
    LOAD_DEVICE_FUNC(vkResetFences);
    LOAD_DEVICE_FUNC(vkQueueSubmit);

#undef LOAD_DEVICE_FUNC

    printf("Device functions loaded\n");
    return true;
}

bool InitVulkan(HWND hwnd) {
    // Create instance
    printf("Creating Vulkan instance...\n");
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Capture Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;

    if (pfn_vkCreateInstance(&createInfo, nullptr, &g_Instance) != VK_SUCCESS) {
        printf("Failed to create Vulkan instance\n");
        return false;
    }
    printf("Instance created: %p\n", (void*)g_Instance);

    // Load instance functions
    if (!LoadInstanceFunctions())
        return false;

    // Create surface
    printf("Creating Win32 surface...\n");
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hwnd = hwnd;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    pfn_vkCreateWin32SurfaceKHR(g_Instance, &surfaceInfo, nullptr, &g_Surface);

    // Pick physical device
    uint32_t deviceCount = 0;
    pfn_vkEnumeratePhysicalDevices(g_Instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    pfn_vkEnumeratePhysicalDevices(g_Instance, &deviceCount, devices.data());
    g_PhysDevice = devices[0];

    VkPhysicalDeviceProperties props;
    pfn_vkGetPhysicalDeviceProperties(g_PhysDevice, &props);
    printf("Using GPU: %s\n", props.deviceName);

    // Find graphics queue family
    uint32_t queueFamilyCount = 0;
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    pfn_vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_GraphicsQueueFamily = i;
            break;
        }
    }

    // Create logical device
    printf("Creating logical device...\n");
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = g_GraphicsQueueFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 1;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

    if (pfn_vkCreateDevice(g_PhysDevice, &deviceCreateInfo, nullptr, &g_Device) != VK_SUCCESS) {
        printf("Failed to create logical device\n");
        return false;
    }
    printf("Device created: %p\n", (void*)g_Device);

    // Load device functions
    if (!LoadDeviceFunctions())
        return false;

    pfn_vkGetDeviceQueue(g_Device, g_GraphicsQueueFamily, 0, &g_GraphicsQueue);

    // Create swapchain
    printf("Creating swapchain...\n");
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    VkSwapchainCreateInfoKHR swapchainInfo = {};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = g_Surface;
    swapchainInfo.minImageCount = 2;
    swapchainInfo.imageFormat = g_SwapchainFormat;
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = g_SwapchainExtent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode =
        g_VSync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;  // VSync controlled by config
    swapchainInfo.clipped = VK_TRUE;

    if (pfn_vkCreateSwapchainKHR(g_Device, &swapchainInfo, nullptr, &g_Swapchain) != VK_SUCCESS) {
        printf("Failed to create swapchain\n");
        return false;
    }
    printf("Swapchain created: %p\n", (void*)g_Swapchain);

    // Get swapchain images
    uint32_t imageCount;
    pfn_vkGetSwapchainImagesKHR(g_Device, g_Swapchain, &imageCount, nullptr);
    g_SwapchainImages.resize(imageCount);
    pfn_vkGetSwapchainImagesKHR(g_Device, g_Swapchain, &imageCount, g_SwapchainImages.data());

    // Create image views
    g_SwapchainImageViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = g_SwapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g_SwapchainFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        pfn_vkCreateImageView(g_Device, &viewInfo, nullptr, &g_SwapchainImageViews[i]);
    }

    // Create render pass
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = g_SwapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    pfn_vkCreateRenderPass(g_Device, &renderPassInfo, nullptr, &g_RenderPass);

    // Create framebuffers
    g_Framebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = g_RenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &g_SwapchainImageViews[i];
        framebufferInfo.width = g_SwapchainExtent.width;
        framebufferInfo.height = g_SwapchainExtent.height;
        framebufferInfo.layers = 1;
        pfn_vkCreateFramebuffer(g_Device, &framebufferInfo, nullptr, &g_Framebuffers[i]);
    }

    // Create command pool
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = g_GraphicsQueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pfn_vkCreateCommandPool(g_Device, &poolInfo, nullptr, &g_CommandPool);

    // Allocate command buffers
    g_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = g_CommandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    pfn_vkAllocateCommandBuffers(g_Device, &allocInfo, g_CommandBuffers.data());

    // Create sync objects
    g_ImageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_RenderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    g_InFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo = {};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        pfn_vkCreateSemaphore(g_Device, &semaphoreInfo, nullptr, &g_ImageAvailableSemaphores[i]);
        pfn_vkCreateSemaphore(g_Device, &semaphoreInfo, nullptr, &g_RenderFinishedSemaphores[i]);
        pfn_vkCreateFence(g_Device, &fenceInfo, nullptr, &g_InFlightFences[i]);
    }

    printf("Vulkan initialized successfully!\n");
    return true;
}

void Render() {
    pfn_vkWaitForFences(g_Device, 1, &g_InFlightFences[g_CurrentFrame], VK_TRUE, UINT64_MAX);
    pfn_vkResetFences(g_Device, 1, &g_InFlightFences[g_CurrentFrame]);

    uint32_t imageIndex;
    pfn_vkAcquireNextImageKHR(g_Device, g_Swapchain, UINT64_MAX, g_ImageAvailableSemaphores[g_CurrentFrame],
                              VK_NULL_HANDLE, &imageIndex);

    // Calculate animation
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    float barPosition = fmodf(elapsed * 0.5f, 1.0f);

    VkCommandBuffer cmd = g_CommandBuffers[g_CurrentFrame];
    pfn_vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    pfn_vkBeginCommandBuffer(cmd, &beginInfo);

    // Animate clear color for visible motion
    VkClearValue clearColor = {{{barPosition, 0.1f, 0.3f, 1.0f}}};

    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = g_RenderPass;
    renderPassInfo.framebuffer = g_Framebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = g_SwapchainExtent;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    pfn_vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    pfn_vkCmdEndRenderPass(cmd);

    // GPU Load: Multiple render pass iterations to simulate real game workload
    for (int pass = 0; pass < g_GpuLoadPasses; pass++) {
        // Vary color slightly so GPU can't optimize away
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        VkClearValue loadColor = {{{0.1f + (pass % 2) * 0.01f, 0.1f, 0.1f, 1.0f}}};
        renderPassInfo.pClearValues = &loadColor;
        pfn_vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        pfn_vkCmdEndRenderPass(cmd);
    }
    // Final pass with animated color
    renderPassInfo.pClearValues = &clearColor;
    pfn_vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    pfn_vkCmdEndRenderPass(cmd);

    pfn_vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {g_ImageAvailableSemaphores[g_CurrentFrame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    VkSemaphore signalSemaphores[] = {g_RenderFinishedSemaphores[g_CurrentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    pfn_vkQueueSubmit(g_GraphicsQueue, 1, &submitInfo, g_InFlightFences[g_CurrentFrame]);

    // Present
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &g_Swapchain;
    presentInfo.pImageIndices = &imageIndex;

    pfn_vkQueuePresentKHR(g_GraphicsQueue, &presentInfo);

    g_CurrentFrame = (g_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Cleanup() {
    if (g_Device) {
        pfn_vkDeviceWaitIdle(g_Device);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            pfn_vkDestroySemaphore(g_Device, g_ImageAvailableSemaphores[i], nullptr);
            pfn_vkDestroySemaphore(g_Device, g_RenderFinishedSemaphores[i], nullptr);
            pfn_vkDestroyFence(g_Device, g_InFlightFences[i], nullptr);
        }
        pfn_vkDestroyCommandPool(g_Device, g_CommandPool, nullptr);
        for (auto fb : g_Framebuffers)
            pfn_vkDestroyFramebuffer(g_Device, fb, nullptr);
        pfn_vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
        for (auto iv : g_SwapchainImageViews)
            pfn_vkDestroyImageView(g_Device, iv, nullptr);
        pfn_vkDestroySwapchainKHR(g_Device, g_Swapchain, nullptr);
        pfn_vkDestroyDevice(g_Device, nullptr);
    }
    if (g_Instance) {
        pfn_vkDestroySurfaceKHR(g_Instance, g_Surface, nullptr);
        pfn_vkDestroyInstance(g_Instance, nullptr);
    }
    if (g_VulkanLib)
        FreeLibrary(g_VulkanLib);
}

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
int main(int argc, char* argv[]) {
    // Load config from testappconfig.ini first
    LoadConfig();

    // Command line overrides config: [width] [height] [gpu_load_passes]
    if (argc >= 3) {
        g_WindowWidth = testapp::ParseIntOrZero(argv[1]);
        g_WindowHeight = testapp::ParseIntOrZero(argv[2]);
    }
    if (argc >= 4) {
        g_GpuLoadPasses = testapp::ParseIntOrZero(argv[3]);
    }

    // Set swapchain extent to configured size
    g_SwapchainExtent = {(uint32_t)g_WindowWidth, (uint32_t)g_WindowHeight};

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();

    printf("Vulkan Capture Test App\n");
    printf("=======================\n");
    printf("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    printf("GPU Load Passes: %d\n", g_GpuLoadPasses);
    printf("VSync: %s\n", g_VSync ? "ON (FIFO)" : "OFF (IMMEDIATE)");
    printf("Process ID: %lu\n", GetCurrentProcessId());
    printf("Press ESC to exit\n\n");

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    // Use borderless (popup) window for large resolutions to avoid decorations
    // that would cause the window to exceed screen bounds
    DWORD style = (g_WindowWidth >= 2560 || g_WindowHeight >= 1440) ? WS_POPUP : WS_OVERLAPPEDWINDOW;

    // Create window with exact pixel dimensions
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, g_WindowWidth, g_WindowHeight);

    wchar_t title[256];
    swprintf(title, 256, L"Vulkan Test - %dx%d - GPU Load: %d", g_WindowWidth, g_WindowHeight, g_GpuLoadPasses);

    HWND hwnd = CreateWindowW(WINDOW_CLASS, title, style, 0, 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, wc.hInstance, nullptr);

    // Load Vulkan dynamically - layer should already be loaded via VK_LAYER_PATH
    if (!LoadVulkanLibrary()) {
        printf("Failed to load Vulkan\n");
        return 1;
    }

    // Show window BEFORE Vulkan init to ensure correct surface size
    // Otherwise surface dimensions might change causing VK_ERROR_OUT_OF_DATE_KHR
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Verify client rect matches expected size
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    printf("Client rect: %ldx%ld\n", clientRect.right, clientRect.bottom);

    if (!InitVulkan(hwnd)) {
        printf("Failed to initialize Vulkan\n");
        return 1;
    }

    printf("Entering render loop...\n");

    // Main loop
    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!g_Running)
            break;
        if (g_Running) {
            Render();
        }
    }

    Cleanup();
    printf("Exiting\n");
    return 0;
}
