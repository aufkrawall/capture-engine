#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_hook.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "../common/config.h"
#include "hook_common.h"
#include "performance_metrics.h"
#include "../common/fg_detection.h"
#include "../common/system_metrics.h"
#include "../../common/frame_timing.h"
#include <MinHook.h>
#include <algorithm>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>
#include <vulkan/vulkan.h>
#include "lod_helper.h"
#include <imgui.h>
#include <mutex>
#include <unordered_map>
#include <vector>
// D3D11 includes for intermediate path (D3D11 textures with KMT handles)
#include <d3d11.h>
#include <d3d11_4.h> // For ID3D11Fence
#include <dxgi.h>
#include <dxgi1_4.h>
#include <psapi.h> // For GetModuleInformation
#pragma comment(lib, "psapi.lib")

static uint32_t MapVulkanFormatToDXGI(VkFormat format) {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return 28; // DXGI_FORMAT_R8G8B8A8_UNORM
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return 87; // DXGI_FORMAT_B8G8R8A8_UNORM (Actually B8G8R8A8 is 87)
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return 10; // DXGI_FORMAT_R16G16B16A16_FLOAT
  case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    return 24; // DXGI_FORMAT_R10G10B10A2_UNORM
  default:
    return 87; // Fallback to BGRA
  }
}

// Reverse mapping: DXGI format -> Vulkan format (for importing D3D11 textures)
static VkFormat MapDXGIToVulkanFormat(uint32_t dxgiFormat) {
  switch (dxgiFormat) {
  case 87: // DXGI_FORMAT_B8G8R8A8_UNORM
    return VK_FORMAT_B8G8R8A8_UNORM;
  case 28: // DXGI_FORMAT_R8G8B8A8_UNORM
    return VK_FORMAT_R8G8B8A8_UNORM;
  case 10: // DXGI_FORMAT_R16G16B16A16_FLOAT
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  case 24: // DXGI_FORMAT_R10G10B10A2_UNORM
    return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
  default:
    return VK_FORMAT_B8G8R8A8_UNORM;
  }
}

// Queue Tracking & Injection
static VkSampleCountFlagBits ParseVulkanMSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0) return VK_SAMPLE_COUNT_2_BIT;
    if (strcmp(msaa, "4x") == 0) return VK_SAMPLE_COUNT_4_BIT;
    if (strcmp(msaa, "8x") == 0) return VK_SAMPLE_COUNT_8_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

// Instance/Device Tracking
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static VkQueue g_Queue = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = 0;
static VkQueue g_AsyncQueue = VK_NULL_HANDLE;
static uint32_t g_AsyncQueueFamily = UINT32_MAX;
static uint32_t g_AsyncQueueIndex = UINT32_MAX;
 
// FG Tracking
static std::atomic<int> g_QueueSubmitsThisFrame{0};

// Thread-local recursion guard for Vulkan hooks
thread_local bool g_InVulkanHook = false;
struct VulkanHookGuard {
    VulkanHookGuard() { g_InVulkanHook = true; }
    ~VulkanHookGuard() { g_InVulkanHook = false; }
};

// Separate Capture Device Globals
static VkDevice g_CaptureDevice = VK_NULL_HANDLE;
static VkQueue g_CaptureQueue = VK_NULL_HANDLE;
static uint32_t g_CaptureQueueFamily = UINT32_MAX;
static VkCommandPool g_CaptureCommandPool = VK_NULL_HANDLE;
static VkCommandBuffer g_CaptureCommandBuffer = VK_NULL_HANDLE;

// Cross-Device Sync Semaphore (Exported from Capture, Imported to Game)
// Cross-Device Sync Semaphores (Ring Buffer for Frame Sync)
static VkSemaphore g_CrossDeviceSems_CaptureOwned[CAPTURE_TEXTURE_COUNT] = {VK_NULL_HANDLE}; // On Capture Device
static VkSemaphore g_CrossDeviceSems_GameImported[CAPTURE_TEXTURE_COUNT] = {VK_NULL_HANDLE}; // On Game Device
static HANDLE g_CrossDeviceSemHandles[CAPTURE_TEXTURE_COUNT] = {nullptr};

// Shared Images (Owned by Capture Device, Imported to Game Device)
struct CrossDeviceImage {
    VkImage mainImage = VK_NULL_HANDLE;   // On Game Device (Exported)
    VkDeviceMemory mainMemory = VK_NULL_HANDLE;
    VkImage importedImage = VK_NULL_HANDLE;   // On Capture Device (Imported)
    VkDeviceMemory importedMemory = VK_NULL_HANDLE;
    HANDLE ntHandle = nullptr;
};
// Stable Staging Resources (Internal, No Export)
static VkImage g_InternalStagingImages[CAPTURE_TEXTURE_COUNT] = {VK_NULL_HANDLE};
static VkDeviceMemory g_InternalStagingMemories[CAPTURE_TEXTURE_COUNT] = {VK_NULL_HANDLE};
static VkSemaphore g_GraphicsReadySems[CAPTURE_TEXTURE_COUNT] = {VK_NULL_HANDLE};

static CrossDeviceImage g_CrossDeviceImages[CAPTURE_TEXTURE_COUNT];

static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
static bool g_ImGuiInit = false;
static HWND g_hWnd = NULL;
static PerformanceMetrics g_PerfMetrics;
static std::recursive_mutex g_VulkanMutex;
static bool separateDeviceActive = false; // Global tracking for Separate Device Mode
static bool g_SampleRateShadingSupported = false; // Track if device supports/has enabled sample shading
static bool g_SamplerAnisotropySupported = false; // Track if device supports/has enabled anisotropy
static float g_MaxSamplerAnisotropy = 0.0f;
static std::vector<VkFence> g_PrerenderFences;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;
static VkDevice g_PrerenderDevice = VK_NULL_HANDLE; // Device required for fence creation

struct QueueFamilyInfo {
    uint32_t requestedCount; // Count requested by game in vkCreateDevice
    uint32_t retrievedCount; // Count actually retrieved via vkGetDeviceQueue
    uint32_t totalAvailable; // Total available in hardware
    VkQueueFlags flags;
};
static std::vector<QueueFamilyInfo> g_QueueFamilies;

// Queue Instance Tracking for Async Present Detection
struct QueueInstanceInfo {
    VkQueue queue;
    uint32_t familyIndex;
    uint32_t queueIndex;
    VkQueueFlags flags;
};
static std::vector<QueueInstanceInfo> g_QueueInstances;
static std::mutex g_QueueInstancesMutex;

 static constexpr int MAX_TRACKED_GRAPHICS_QUEUES = 8;
 static VkQueue g_TrackedGraphicsQueues[MAX_TRACKED_GRAPHICS_QUEUES] = {VK_NULL_HANDLE};
 static std::atomic<int> g_TrackedGraphicsQueueCount{0};

 static bool IsTrackedGraphicsQueue(VkQueue queue) {
     int count = g_TrackedGraphicsQueueCount.load(std::memory_order_acquire);
     for (int i = 0; i < count; i++) {
         if (g_TrackedGraphicsQueues[i] == queue) return true;
     }
     return false;
 }

 static void TrackGraphicsQueue(VkQueue queue) {
     if (queue == VK_NULL_HANDLE) return;
     int count = g_TrackedGraphicsQueueCount.load(std::memory_order_acquire);
     for (int i = 0; i < count; i++) {
         if (g_TrackedGraphicsQueues[i] == queue) return;
     }
     if (count < MAX_TRACKED_GRAPHICS_QUEUES) {
         g_TrackedGraphicsQueues[count] = queue;
         g_TrackedGraphicsQueueCount.store(count + 1, std::memory_order_release);
     }
 }

// Helper to check if a queue is compute-only (no graphics bit)
static bool IsComputeOnlyQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_QueueInstancesMutex);
    for (const auto& qi : g_QueueInstances) {
        if (qi.queue == queue) {
            return (qi.flags & VK_QUEUE_COMPUTE_BIT) && 
                   !(qi.flags & VK_QUEUE_GRAPHICS_BIT);
        }
    }
    return false;
}

// Find a graphics-capable queue for overlay rendering
static VkQueue FindGraphicsQueue() {
    std::lock_guard<std::mutex> lock(g_QueueInstancesMutex);
    for (const auto& qi : g_QueueInstances) {
        if (qi.flags & VK_QUEUE_GRAPHICS_BIT) {
            return qi.queue;
        }
    }
    return VK_NULL_HANDLE;
}

// Get queue family index for a given queue
static uint32_t GetQueueFamilyIndex(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_QueueInstancesMutex);
    for (const auto& qi : g_QueueInstances) {
        if (qi.queue == queue) {
            return qi.familyIndex;
        }
    }
    return UINT32_MAX;
}





// Helper for memory patching
// Helper to scan memory with mask
// Mask: 'x' = match, '?' = wild
static uint8_t* ScanPattern(uint8_t* start, size_t size, const char* pattern, const char* mask) {
    size_t patternLen = strlen(mask);
    uint8_t* end = start + size - patternLen;
    
    for (uint8_t* p = start; p < end; p++) {
        bool match = true;
        for (size_t i = 0; i < patternLen; i++) {
            if (mask[i] == 'x' && p[i] != (uint8_t)pattern[i]) {
                match = false;
                break;
            }
        }
        if (match) return p;
    }
    return nullptr;
}

// Helper: Apply a specific whitelisted pattern patch
static int ApplyWhitelistedPattern(uint8_t* start, size_t size, const uint8_t* pattern, size_t patternLen, const uint8_t* patch, size_t patchLen, const char* name) {
    int hits = 0;
    uint8_t* p = start;
    uint8_t* end = start + size - patternLen;
    
    while (p < end) {
        if (memcmp(p, pattern, patternLen) == 0) {
            DWORD old;
            if (VirtualProtect(p, patchLen, PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(p, patch, patchLen);
                VirtualProtect(p, patchLen, old, &old);
                FlushInstructionCache(GetCurrentProcess(), p, patchLen);
                hits++;
                EarlyLog("Vulkan: Shotgun Patch [%s] applied at %p", name, p);
            }
        }
        p++;
    }
    return hits;
}

static int ApplyShotgunWhitelists(uint8_t* start, size_t size) {
    int totalHits = 0;

    // --- Shotgun V3 Register-Agnostic Patterns ---
    // Targets: EAX(0), ECX(1), EDX(2), EBX(3), EBP(5), ESI(6), EDI(7)
    uint8_t targetRegs[] = { 0, 1, 2, 3, 5, 6, 7 };

    // A: Universal Triple/Quad Zero (04, 08, 0C, 10 or 08, 0C, 10)
    for (uint8_t reg : targetRegs) {
        uint8_t mod = 0x40 + reg;
        const char* regName = (reg == 6 ? "ESI" : (reg == 7 ? "EDI" : (reg == 0 ? "EAX" : "GPR")));

        // Quad Zero (04, 08, 0C, 10) - 28 bytes
        uint8_t pQ4[] = { 0xC7, mod, 0x04, 0x00, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x08, 0x00, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x0C, 0x00, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x10, 0x00, 0x00, 0x00, 0x00 };
        uint8_t hQ4[] = { 0xC7, mod, 0x04, 0x00, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x08, 0x10, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x0C, 0x10, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x10, 0x10, 0x00, 0x00, 0x00 };
        totalHits += ApplyWhitelistedPattern(start, size, pQ4, 28, hQ4, 28, regName);

        // Triple Zero (08, 0C, 10) - 21 bytes
        uint8_t pZ3[] = { 0xC7, mod, 0x08, 0x00, 0x00, 0x00, 0x00, 
                          0xC7, mod, 0x0C, 0x00, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x10, 0x00, 0x00, 0x00, 0x00 };
        uint8_t hZ3[] = { 0xC7, mod, 0x08, 0x10, 0x00, 0x00, 0x00, 
                          0xC7, mod, 0x0C, 0x10, 0x00, 0x00, 0x00,
                          0xC7, mod, 0x10, 0x10, 0x00, 0x00, 0x00 };
        totalHits += ApplyWhitelistedPattern(start, size, pZ3, 21, hZ3, 21, regName);

        // Handle + Double Zero (89 4X 08, C7 4X 0C, C7 4X 10) - 17 bytes
        uint8_t pHZ2[] = { 0x89, mod, 0x08, 
                           0xC7, mod, 0x0C, 0x00, 0x00, 0x00, 0x00,
                           0xC7, mod, 0x10, 0x00, 0x00, 0x00, 0x00 };
        uint8_t hHZ2[] = { 0x89, mod, 0x08, 
                           0xC7, mod, 0x0C, 0x10, 0x00, 0x00, 0x00,
                           0xC7, mod, 0x10, 0x10, 0x00, 0x00, 0x00 };
        totalHits += ApplyWhitelistedPattern(start, size, pHZ2, 17, hHZ2, 17, regName);
    }

    // B: SHRD Variant (Aged version)
    uint8_t pEDI_S1[] = { 0x0F, 0xAC, 0xC1, 0x02, 0x8B, 0x45, 0x08, 0xC7, 0x47, 0x08, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x47, 0x0C, 0x00, 0x00, 0x00, 0x00 };
    uint8_t hEDI_S1[] = { 0x0F, 0xAC, 0xC1, 0x02, 0x8B, 0x45, 0x08, 0xC7, 0x47, 0x08, 0x10, 0x00, 0x00, 0x00, 0xC7, 0x47, 0x0C, 0x10, 0x00, 0x00, 0x00 };
    totalHits += ApplyWhitelistedPattern(start, size, pEDI_S1, 21, hEDI_S1, 21, "SHRD");

    // C: REP MOVSD Context (F3 A5 C7 4x 0C...)
    for (uint8_t reg : targetRegs) {
        uint8_t mod = 0x40 + reg;
        uint8_t pREP[] = { 0xF3, 0xA5, 0xC7, mod, 0x0C, 0x00, 0x00, 0x00, 0x00, 0xC7, mod, 0x10, 0x00, 0x00, 0x00, 0x00 };
        uint8_t hREP[] = { 0xF3, 0xA5, 0xC7, mod, 0x0C, 0x10, 0x00, 0x00, 0x00, 0xC7, mod, 0x10, 0x10, 0x00, 0x00, 0x00 };
        totalHits += ApplyWhitelistedPattern(start, size, pREP, 16, hREP, 16, "REP_MOVSD");
    }

    return totalHits;
}

static int ApplyAllWhitelistedPatches(uint8_t* start, size_t size) {
    return ApplyShotgunWhitelists(start, size);
}

static void ScanForAllResets(uint8_t* start, size_t size) {
    const char* regNames[] = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };
    EarlyLog("Vulkan: Starting Broad Scan for 'MOV [Reg+0C], 0'...");
    for (int reg = 0; reg < 8; reg++) {
        if (reg == 4) continue;
        uint8_t modRM = 0x40 + reg;
        uint8_t pattern[] = { 0xC7, modRM, 0x0C, 0x00, 0x00, 0x00, 0x00 };
        uint8_t* p = start;
        uint8_t* end = start + size - 8;
        while (p < end) {
            if (memcmp(p, pattern, sizeof(pattern)) == 0) {
                EarlyLog("Vulkan: Broad Scan Found: MOV [%s+0C], 0 at %p", regNames[reg], p);
                uint8_t* dumpStart = (p > start + 16) ? p - 16 : start;
                uint8_t* dumpEnd = (p + 32 < start + size) ? p + 32 : start + size;
                char buf[128] = {0}; std::string dump;
                for (uint8_t* d = dumpStart; d < dumpEnd; d++) { snprintf(buf, sizeof(buf), "%02X ", *d); dump += buf; }
                EarlyLog("Vulkan: Context: %s", dump.c_str());
            }
            p++;
        }
    }
    EarlyLog("Vulkan: Broad Scan Complete.");
}

// Helper for memory patching
// Helper for memory patching
// (Removed NVIDIA LOD Bias Fix logic)


static void TryDiscoverAsyncQueue();




// --- Separate Capture Device Implementation ---

static uint32_t FindMemoryType(VkPhysicalDevice physDev, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDev, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

static bool CreateCaptureDevice() {
    if (g_CaptureDevice != VK_NULL_HANDLE) return true;
    if (g_PhysDevice == VK_NULL_HANDLE) return false;

    // 1. Find Transfer-only family (safest for dedicated copy)
    uint32_t qFamCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &qFamCount, nullptr);
    std::vector<VkQueueFamilyProperties> qProps(qFamCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &qFamCount, qProps.data());

    uint32_t transferFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qFamCount; i++) {
        bool hasTransfer = (qProps[i].queueFlags & VK_QUEUE_TRANSFER_BIT);
        bool hasGraphics = (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
        bool hasCompute = (qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
        // Prioritize PURE Transfer queues (no graphics/compute)
        if (hasTransfer && !hasGraphics && !hasCompute && qProps[i].queueCount > 0) {
            transferFamily = i;
            break;
        }
    }
    // Fallback to Compute if no pure Transfer
    if (transferFamily == UINT32_MAX) {
        for (uint32_t i = 0; i < qFamCount; i++) {
            bool hasCompute = (qProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
            bool hasGraphics = (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
            if (hasCompute && !hasGraphics && qProps[i].queueCount > 0) {
                transferFamily = i;
                break;
            }
        }
    }
    if (transferFamily == UINT32_MAX) {
        HookLog("Vulkan: CaptureDevice - No suitable Transfer/Compute family");
        return false;
    }

    // 2. Create Device
    float priority = 0.5f;
    VkDeviceQueueCreateInfo queueCI = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCI.queueFamilyIndex = transferFamily;
    queueCI.queueCount = 1;
    queueCI.pQueuePriorities = &priority;

    const char* extensions[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceCI = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCI.queueCreateInfoCount = 1;
    deviceCI.pQueueCreateInfos = &queueCI;
    deviceCI.enabledExtensionCount = 5;
    deviceCI.ppEnabledExtensionNames = extensions;

    // THIS is our own vkCreateDevice call - NOT modifying game's CI
    VkResult res;
    {
        VulkanHookGuard guard;
        res = vkCreateDevice(g_PhysDevice, &deviceCI, nullptr, &g_CaptureDevice);
    }
    if (res != VK_SUCCESS) {
        HookLog("Vulkan: CaptureDevice creation failed (%d)", res);
        return false;
    }

    // volkLoadDevice(g_CaptureDevice); // REMOVED to prevent global pointer hijacking in multi-device scenarios

    g_CaptureQueueFamily = transferFamily;
    {
        VulkanHookGuard guard;
        vkGetDeviceQueue(g_CaptureDevice, transferFamily, 0, &g_CaptureQueue);
    }
    HookLog("Vulkan: CaptureDevice created (Family %d, Queue %p)", transferFamily, g_CaptureQueue);

    // 3. Create Command Pool/Buffer
    VkCommandPoolCreateInfo poolCI = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolCI.queueFamilyIndex = transferFamily;
    poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(g_CaptureDevice, &poolCI, nullptr, &g_CaptureCommandPool);

    VkCommandBufferAllocateInfo allocCI = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocCI.commandPool = g_CaptureCommandPool;
    allocCI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_CaptureDevice, &allocCI, &g_CaptureCommandBuffer);

    return true;
}

static bool CreateCrossDeviceImages(uint32_t width, uint32_t height, VkFormat format) {
    if (g_Device == VK_NULL_HANDLE) return false;

    if (g_CrossDeviceImages[0].mainImage != VK_NULL_HANDLE) {
        return true; // Already created
    }
    // Clear previous images if any
    if (g_CrossDeviceImages[0].mainImage != VK_NULL_HANDLE) {
        HookLog("Vulkan: Re-creating Cross Device Images...");
    }

    // Get Function Pointers for Game Device
    auto getMemHandle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(g_Device, "vkGetMemoryWin32HandleKHR");
    if (!getMemHandle) {
        HookLog("Vulkan: Error - vkGetMemoryWin32HandleKHR missing on Game Device");
        return false;
    }

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        // 1. Create exportable image on GAME Device (Owner)
        VkExternalMemoryImageCreateInfo extMemInfo = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        extMemInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo imageCI = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageCI.pNext = &extMemInfo;
        imageCI.imageType = VK_IMAGE_TYPE_2D;
        imageCI.format = format;
        imageCI.extent = {width, height, 1};
        imageCI.mipLevels = 1;
        imageCI.arrayLayers = 1;
        imageCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Game writes TO this image. Capture reads FROM it.
        // Adding COLOR_ATTACHMENT_BIT as it's often required for D3D11 OpenSharedResource success
        imageCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        
        // Use EXCLUSIVE for shared export images to avoid complexity with D3D11 interop
        imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Store in .gameDeviceImage (renamed from .captureDeviceImage concept)
        // We will need to update the struct definition or just swap usage.
        // Let's try to update struct if possible, but I can't see the struct def here easily.
        // I'll stick to variable names matching the device they are on.
        // Re-using the struct members but assuming:
        // captureDeviceImage -> Image on Creating Device (Now Game)
        // gameDeviceImported -> Image on Importing Device (Now Capture)
        
        VkResult res = vkCreateImage(g_Device, &imageCI, nullptr, &g_CrossDeviceImages[i].mainImage);
        if (res != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkCreateImage[%d] failed with %d", i, res);
            return false;
        }

        // 2. Allocate exportable memory
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(g_Device, g_CrossDeviceImages[i].mainImage, &memReqs);

        VkExportMemoryWin32HandleInfoKHR win32ExportInfo = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        win32ExportInfo.dwAccess = GENERIC_ALL;

        VkExportMemoryAllocateInfo exportAllocInfo = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportAllocInfo.pNext = &win32ExportInfo;
        exportAllocInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkMemoryDedicatedAllocateInfo dedicatedAllocInfo = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedAllocInfo.image = g_CrossDeviceImages[i].mainImage;
        dedicatedAllocInfo.pNext = &exportAllocInfo;

        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.pNext = &dedicatedAllocInfo;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(g_PhysDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        res = vkAllocateMemory(g_Device, &allocInfo, nullptr, &g_CrossDeviceImages[i].mainMemory);
        if (res != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkAllocateMemory[%d] failed with %d", i, res);
            vkDestroyImage(g_Device, g_CrossDeviceImages[i].mainImage, nullptr);
            g_CrossDeviceImages[i].mainImage = VK_NULL_HANDLE;
            return false;
        }

        res = vkBindImageMemory(g_Device, g_CrossDeviceImages[i].mainImage, g_CrossDeviceImages[i].mainMemory, 0);
        if (res != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkBindImageMemory[%d] failed with %d", i, res);
            vkFreeMemory(g_Device, g_CrossDeviceImages[i].mainMemory, nullptr);
            vkDestroyImage(g_Device, g_CrossDeviceImages[i].mainImage, nullptr);
            g_CrossDeviceImages[i].mainImage = VK_NULL_HANDLE;
            g_CrossDeviceImages[i].mainMemory = VK_NULL_HANDLE;
            return false;
        }

        // 3. Export handle
        VkMemoryGetWin32HandleInfoKHR handleInfo = {VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        handleInfo.memory = g_CrossDeviceImages[i].mainMemory;
        handleInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        
        res = getMemHandle(g_Device, &handleInfo, &g_CrossDeviceImages[i].ntHandle);
        if (res != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkGetMemoryWin32HandleKHR[%d] failed with %d", i, res);
            vkFreeMemory(g_Device, g_CrossDeviceImages[i].mainMemory, nullptr);
            vkDestroyImage(g_Device, g_CrossDeviceImages[i].mainImage, nullptr);
            g_CrossDeviceImages[i].mainImage = VK_NULL_HANDLE;
            g_CrossDeviceImages[i].mainMemory = VK_NULL_HANDLE;
            return false;
        }

        if (g_CrossDeviceImages[i].ntHandle == INVALID_HANDLE_VALUE || g_CrossDeviceImages[i].ntHandle == nullptr) {
            HookLog("Vulkan: Error - CrossDeviceImage[%d] export returned invalid handle", i);
            vkFreeMemory(g_Device, g_CrossDeviceImages[i].mainMemory, nullptr);
            vkDestroyImage(g_Device, g_CrossDeviceImages[i].mainImage, nullptr);
            g_CrossDeviceImages[i].mainImage = VK_NULL_HANDLE;
            g_CrossDeviceImages[i].mainMemory = VK_NULL_HANDLE;
            return false;
        }

        HookLog("Vulkan: CrossDeviceImage[%d] exported successfully, handle=%p", i, g_CrossDeviceImages[i].ntHandle);
    }
    return true;
}

// Unused function removed
static bool ImportCrossDeviceImages(uint32_t width, uint32_t height, VkFormat format) {
    return true; // No-op
}

static bool CreateCrossDeviceSemaphore() {
    VkExportSemaphoreWin32HandleInfoKHR exportSemWin32Info = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    exportSemWin32Info.dwAccess = GENERIC_ALL;

    VkExportSemaphoreCreateInfo exportInfo = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.pNext = &exportSemWin32Info;
    exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo semCI = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semCI.pNext = &exportInfo;

    VkSemaphoreGetWin32HandleInfoKHR getHandleInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    getHandleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkSemaphoreCreateInfo importedSemCI = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    VkImportSemaphoreWin32HandleInfoKHR importInfo = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // Get Function Pointers
    auto getSemHandle = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(g_Device, "vkGetSemaphoreWin32HandleKHR");
    auto importSem = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(g_CaptureDevice, "vkImportSemaphoreWin32HandleKHR");

    if (!getSemHandle || !importSem) {
        HookLog("Vulkan: Error - Missing cross-device semaphore extensions (Get=%p, Import=%p)", getSemHandle, importSem);
        return false;
    }

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        // 1. Create exportable binary semaphore on GAME Device (Primary)
        // We reuse 'g_CrossDeviceSems_GameImported' to store the Game Device semaphore
        // (even though it's now exported, preventing variable rename churn).
        if (vkCreateSemaphore(g_Device, &semCI, nullptr, &g_CrossDeviceSems_GameImported[i]) != VK_SUCCESS) {
             HookLog("Vulkan: Error - Failed to create semaphore %d on Game Device", i);
             return false;
        }

        // 2. Export handle from Game Device
        getHandleInfo.semaphore = g_CrossDeviceSems_GameImported[i];
        HANDLE sharedHandle = NULL;
        if (getSemHandle(g_Device, &getHandleInfo, &sharedHandle) != VK_SUCCESS) {
            HookLog("Vulkan: Error - Failed to export semaphore %d", i);
            return false;
        }

        // 3. Import handle to Capture Device (Secondary)
        // We reuse 'g_CrossDeviceSems_CaptureOwned' to store the Capture Device semaphore
        // Previously created? No, we need to create it!
        // Wait, ImportSemaphoreWin32Handle imports into EXISTING semaphore or creates?
        // It imports payload into EXISTING semaphore.
        
        // So we must Create semaphore on Capture Device first.
        if (vkCreateSemaphore(g_CaptureDevice, &importedSemCI, nullptr, &g_CrossDeviceSems_CaptureOwned[i]) != VK_SUCCESS) {
             HookLog("Vulkan: Error - Failed to create semaphore %d on Capture Device", i);
             return false;
        }
        
        importInfo.semaphore = g_CrossDeviceSems_CaptureOwned[i];
        importInfo.handle = sharedHandle;
        
        if (importSem(g_CaptureDevice, &importInfo) != VK_SUCCESS) {
             HookLog("Vulkan: Error - Failed to import semaphore %d to Capture Device", i);
             return false;
        }
        
        HookLog("Vulkan: CrossDeviceSemaphore[%d] linked (Game->Capture)", i);
    }
    return true;
}

// Vulkan Capture Resources - extends HookCaptureBase for shared logic
class VulkanCapture : public HookCaptureBase {
public:
  VkImage sharedImages[CAPTURE_TEXTURE_COUNT]{};
  VkDeviceMemory sharedMem[CAPTURE_TEXTURE_COUNT]{};
  VkFormat vkFormat = VK_FORMAT_UNDEFINED;

  // Capture command resources
  VkCommandPool captureCommandPool = VK_NULL_HANDLE;
  VkCommandBuffer captureCommandBuffers[CAPTURE_TEXTURE_COUNT] = {};
  
  // Async Capture resources (if using separate queue)
  VkCommandPool asyncCommandPool = VK_NULL_HANDLE;
  VkCommandBuffer asyncCommandBuffers[CAPTURE_TEXTURE_COUNT] = {};
  uint32_t asyncQueueFamily = UINT32_MAX;

  // Timeline semaphore for GPU synchronization (Host -> IPC)
  VkSemaphore timelineSemaphore = VK_NULL_HANDLE;

  // Semaphores to signal Copy completion (Copy -> Present) -- OLD SYNC PATH
  VkSemaphore copyCompleteSemaphores[CAPTURE_TEXTURE_COUNT] = {};
  
  // Semaphores for Async Splitter (Game -> [Copy, Present])
  VkSemaphore presentTriggerSems[CAPTURE_TEXTURE_COUNT] = {};
  VkSemaphore copyTriggerSems[CAPTURE_TEXTURE_COUNT] = {};

  // D3D11 intermediate path: Create D3D11 textures that both Vulkan and
  // D3D11 can access. Vulkan copies to these, encoder opens via KMT handles.
  ID3D11Device *d3d11Device = nullptr;
  ID3D11DeviceContext *d3d11Context = nullptr;
  ID3D11Texture2D *d3d11Textures[CAPTURE_TEXTURE_COUNT] = {};
  HANDLE d3d11TextureHandles[CAPTURE_TEXTURE_COUNT] = {}; // KMT handles
  ID3D11Fence *d3d11Fence = nullptr;
  HANDLE d3d11FenceHandle = nullptr;
  bool usingD3D11Path = false;

  void CleanupD3D11() {
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      if (d3d11Textures[i]) {
        d3d11Textures[i]->Release();
        d3d11Textures[i] = nullptr;
      }
      d3d11TextureHandles[i] = nullptr;
    }
    if (d3d11Fence) {
      d3d11Fence->Release();
      d3d11Fence = nullptr;
    }
    d3d11FenceHandle = nullptr;
    if (d3d11Context) {
      d3d11Context->Release();
      d3d11Context = nullptr;
    }
    if (d3d11Device) {
      d3d11Device->Release();
      d3d11Device = nullptr;
    }
    usingD3D11Path = false;
  }

  void CleanupVulkan(VkDevice device) {
    StopCaptureThread();
    CleanupD3D11(); // Clean D3D11 resources first

    // Clean up Global Separate Capture Device Resources
    if (g_CaptureDevice != VK_NULL_HANDLE) {
        HookLog("Vulkan: Cleaning up Capture Device resources...");
    // Destroy Cross-Device Semaphores
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        if (g_CrossDeviceSems_GameImported[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(g_Device, g_CrossDeviceSems_GameImported[i], nullptr);
            g_CrossDeviceSems_GameImported[i] = VK_NULL_HANDLE;
        }
        if (g_CrossDeviceSems_CaptureOwned[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(g_CaptureDevice, g_CrossDeviceSems_CaptureOwned[i], nullptr);
            g_CrossDeviceSems_CaptureOwned[i] = VK_NULL_HANDLE;
        }
        // Handles are closed by Import (consumed), so no CloseHandle needed for imported ones.
        // If export failed or import not done, we might need to close, but we assume success if created.
        g_CrossDeviceSemHandles[i] = nullptr; 
    }
        
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (g_CrossDeviceImages[i].importedImage) {
                vkDestroyImage(g_CaptureDevice, g_CrossDeviceImages[i].importedImage, nullptr);
                g_CrossDeviceImages[i].importedImage = VK_NULL_HANDLE;
            }
            if (g_CrossDeviceImages[i].importedMemory) {
                vkFreeMemory(g_CaptureDevice, g_CrossDeviceImages[i].importedMemory, nullptr);
                g_CrossDeviceImages[i].importedMemory = VK_NULL_HANDLE;
            }
            if (g_CrossDeviceImages[i].ntHandle) {
                CloseHandle(g_CrossDeviceImages[i].ntHandle);
                g_CrossDeviceImages[i].ntHandle = nullptr;
            }
        }
        
        if (g_CaptureCommandPool) {
             vkDestroyCommandPool(g_CaptureDevice, g_CaptureCommandPool, nullptr);
             g_CaptureCommandPool = VK_NULL_HANDLE;
        }

        vkDestroyDevice(g_CaptureDevice, nullptr);
        g_CaptureDevice = VK_NULL_HANDLE;
        g_CaptureQueue = VK_NULL_HANDLE;
    }

    if (!device)
      return;
    
    // Standard cleanup loop handles sharedImages (which alias mainImage/mainMemory in separate device mode)
    // No extra cleanup needed for CrossDeviceImages on Game Device here.

    // Standard cleanup
    CleanupSharedHandles();

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      if (sharedImages[i])
        vkDestroyImage(device, sharedImages[i], nullptr);
      if (sharedMem[i])
        vkFreeMemory(device, sharedMem[i], nullptr);
      if (copyCompleteSemaphores[i])
        vkDestroySemaphore(device, copyCompleteSemaphores[i], nullptr);
      if (presentTriggerSems[i])
        vkDestroySemaphore(device, presentTriggerSems[i], nullptr);
      if (copyTriggerSems[i])
        vkDestroySemaphore(device, copyTriggerSems[i], nullptr);

      // CRITICAL: If these aliased g_CrossDeviceImages, NULL out the globals too!
      for (int j = 0; j < CAPTURE_TEXTURE_COUNT; j++) {
          if (sharedImages[i] == g_CrossDeviceImages[j].mainImage) g_CrossDeviceImages[j].mainImage = VK_NULL_HANDLE;
          if (sharedMem[i] == g_CrossDeviceImages[j].mainMemory) g_CrossDeviceImages[j].mainMemory = VK_NULL_HANDLE;
      }

      sharedImages[i] = VK_NULL_HANDLE;
      sharedMem[i] = VK_NULL_HANDLE;
      copyCompleteSemaphores[i] = VK_NULL_HANDLE;
      presentTriggerSems[i] = VK_NULL_HANDLE;
      copyTriggerSems[i] = VK_NULL_HANDLE;
    }

    // Destroy Internal Staging Resources
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        if (g_InternalStagingImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device, g_InternalStagingImages[i], nullptr);
            g_InternalStagingImages[i] = VK_NULL_HANDLE;
        }
        if (g_InternalStagingMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, g_InternalStagingMemories[i], nullptr);
            g_InternalStagingMemories[i] = VK_NULL_HANDLE;
        }
        if (g_GraphicsReadySems[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, g_GraphicsReadySems[i], nullptr);
            g_GraphicsReadySems[i] = VK_NULL_HANDLE;
        }
    }
    if (captureCommandPool) {
      if (captureCommandBuffers[0]) {
        vkFreeCommandBuffers(device, captureCommandPool, CAPTURE_TEXTURE_COUNT,
                             captureCommandBuffers);
        for (auto &cb : captureCommandBuffers)
          cb = VK_NULL_HANDLE;
      }
      vkDestroyCommandPool(device, captureCommandPool, nullptr);
      captureCommandPool = VK_NULL_HANDLE;
    }
    if (asyncCommandPool) {
      if (asyncCommandBuffers[0]) {
        vkFreeCommandBuffers(device, asyncCommandPool, CAPTURE_TEXTURE_COUNT, asyncCommandBuffers);
        for (auto &cb : asyncCommandBuffers) cb = VK_NULL_HANDLE;
      }
      vkDestroyCommandPool(device, asyncCommandPool, nullptr);
      asyncCommandPool = VK_NULL_HANDLE;
    }
    if (timelineSemaphore) {
      vkDestroySemaphore(device, timelineSemaphore, nullptr);
      timelineSemaphore = VK_NULL_HANDLE;
    }
    initialized = false;
  }

  void Cleanup() override { CleanupVulkan(g_Device); }

  // Create D3D11 textures and import them into Vulkan
  // This path works because D3D11 KMT handles CAN be opened cross-process
  bool CreateD3D11SharedResources(uint32_t w, uint32_t h, uint32_t dxgiFormat) {
    HookLog("Vulkan: CreateD3D11SharedResources %dx%d format=%d", w, h,
            dxgiFormat);

    width = w;
    height = h;
    format = dxgiFormat;
    vkFormat = MapDXGIToVulkanFormat(dxgiFormat);

    // Create D3D11 device on same adapter as Vulkan (matching LUID)
    HMODULE hDXGI = LoadLibraryA("dxgi.dll");
    if (!hDXGI) {
        HookLog("Vulkan: DXGI DLL not found");
        return false;
    }

    typedef HRESULT (WINAPI *PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
    PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 = (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateDXGIFactory1) {
         HookLog("Vulkan: CreateDXGIFactory1 not found");
         return false;
    }

    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      HookLog("Vulkan: Failed to create DXGI factory: HR=%x", hr);
      return false;
    }

    IDXGIAdapter *targetAdapter = nullptr;
    IDXGIAdapter *adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND;
         i++) {
      DXGI_ADAPTER_DESC desc;
      adapter->GetDesc(&desc);
      if (desc.AdapterLuid.LowPart == luidLow &&
          desc.AdapterLuid.HighPart == (LONG)luidHigh) {
        targetAdapter = adapter;
        HookLog("Vulkan: Found matching adapter: %S", desc.Description);
        break;
      }
      adapter->Release();
    }
    factory->Release();

    if (!targetAdapter) {
      HookLog("Vulkan: Could not find matching DXGI adapter for LUID %08x",
              luidLow);
      return false;
    }

    // Create D3D11 device on this adapter
    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
        HookLog("Vulkan: D3D11 DLL not found");
        targetAdapter->Release();
        return false;
    }

    typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice) {
         HookLog("Vulkan: D3D11CreateDevice not found");
         targetAdapter->Release();
         return false;
    }

    D3D_FEATURE_LEVEL featureLevel;
    hr = pD3D11CreateDevice(targetAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &d3d11Device, &featureLevel,
                           &d3d11Context);
    targetAdapter->Release();

    if (FAILED(hr)) {
      HookLog("Vulkan: Failed to create D3D11 device: HR=%x", hr);
      return false;
    }
    HookLog("Vulkan: Created D3D11 device (feature level %x)", featureLevel);

    // Create D3D11 textures with KMT sharing (the format that works
    // cross-process!)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)dxgiFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED; // KMT sharing - works cross-process!

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      hr = d3d11Device->CreateTexture2D(&texDesc, nullptr, &d3d11Textures[i]);
      if (FAILED(hr)) {
        HookLog("Vulkan: Failed to create D3D11 texture %d: HR=%x", i, hr);
        CleanupD3D11();
        return false;
      }

      // Get KMT shared handle
      IDXGIResource *dxgiRes = nullptr;
      hr = d3d11Textures[i]->QueryInterface(IID_PPV_ARGS(&dxgiRes));
      if (SUCCEEDED(hr)) {
        dxgiRes->GetSharedHandle(&d3d11TextureHandles[i]);
        dxgiRes->Release();
      }

      if (!d3d11TextureHandles[i]) {
        HookLog("Vulkan: Failed to get KMT handle for texture %d", i);
        CleanupD3D11();
        return false;
      }

      HookLog("Vulkan: Created D3D11 texture %d, KMT handle=%p", i,
              d3d11TextureHandles[i]);

      // Copy KMT handles to CaptureBase for publishing
      sharedTextureHandles[i] = d3d11TextureHandles[i];
    }

    // Import D3D11 textures into Vulkan using D3D11_TEXTURE_BIT
    // This allows Vulkan to write to the D3D11 textures directly
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      VkExternalMemoryImageCreateInfo extImageInfo = {
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      extImageInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

      VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imageInfo.pNext = &extImageInfo;
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.format = vkFormat;
      imageInfo.extent = {w, h, 1};
      imageInfo.mipLevels = 1;
      imageInfo.arrayLayers = 1;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      VkResult res =
          vkCreateImage(g_Device, &imageInfo, nullptr, &sharedImages[i]);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: Failed to create VkImage for D3D11 import %d: %d", i,
                res);
        CleanupD3D11();
        return false;
      }

      // Import D3D11 texture memory into Vulkan
      VkImportMemoryWin32HandleInfoKHR importInfo = {
          VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
      importInfo.handleType =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
      importInfo.handle = d3d11TextureHandles[i];

      VkMemoryDedicatedAllocateInfo dedicatedInfo = {
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicatedInfo.pNext = &importInfo;
      dedicatedInfo.image = sharedImages[i];

      // Get memory requirements
      VkMemoryRequirements memReq;
      vkGetImageMemoryRequirements(g_Device, sharedImages[i], &memReq);

      // Find compatible memory type
      VkPhysicalDeviceMemoryProperties memProps;
      vkGetPhysicalDeviceMemoryProperties(g_PhysDevice, &memProps);

      uint32_t memTypeIndex = 0;
      for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
        if ((memReq.memoryTypeBits & (1 << j)) &&
            (memProps.memoryTypes[j].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          memTypeIndex = j;
          break;
        }
      }

      VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocInfo.pNext = &dedicatedInfo;
      allocInfo.allocationSize = memReq.size;
      allocInfo.memoryTypeIndex = memTypeIndex;

      res = vkAllocateMemory(g_Device, &allocInfo, nullptr, &sharedMem[i]);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: Failed to import D3D11 memory %d: %d", i, res);
        CleanupD3D11();
        return false;
      }

      res = vkBindImageMemory(g_Device, sharedImages[i], this->sharedMem[i], 0);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: Failed to bind imported memory %d: %d", i, res);
        CleanupD3D11();
        return false;
      }

      HookLog("Vulkan: Imported D3D11 texture %d into Vulkan", i);
    }

    // Create command pool and buffers for capture
    VkCommandPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult res =
        vkCreateCommandPool(g_Device, &poolInfo, nullptr, &captureCommandPool);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to create command pool: %d", res);
      CleanupD3D11();
      return false;
    }

    VkCommandBufferAllocateInfo cbAllocInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbAllocInfo.commandPool = captureCommandPool;
    cbAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAllocInfo.commandBufferCount = CAPTURE_TEXTURE_COUNT;

    res =
        vkAllocateCommandBuffers(g_Device, &cbAllocInfo, captureCommandBuffers);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to allocate command buffers: %d", res);
      CleanupD3D11();
      return false;
    }

    // Create exportable timeline semaphore for GPU sync
    // Use D3D12_FENCE_BIT so D3D11 can import it directly
    VkExportSemaphoreWin32HandleInfoKHR win32ExportInfo = {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    win32ExportInfo.dwAccess = GENERIC_ALL;
    
    VkExportSemaphoreCreateInfo exportSemInfo = {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportSemInfo.pNext = &win32ExportInfo;
    exportSemInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    
    VkSemaphoreTypeCreateInfo semTypeInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semTypeInfo.pNext = &exportSemInfo;
    semTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semTypeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semInfo.pNext = &semTypeInfo;

    res = vkCreateSemaphore(g_Device, &semInfo, nullptr, &timelineSemaphore);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to create exportable timeline semaphore: %d", res);
      CleanupD3D11();
      return false;
    }
    
    // Export the timeline semaphore as D3D12 fence handle
    auto GetSemaphoreWin32Handle =
        (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
            g_Device, "vkGetSemaphoreWin32HandleKHR");
    if (GetSemaphoreWin32Handle) {
      VkSemaphoreGetWin32HandleInfoKHR getSemHandleInfo = {
          VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
      getSemHandleInfo.semaphore = timelineSemaphore;
      getSemHandleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      VkResult expRes = GetSemaphoreWin32Handle(g_Device, &getSemHandleInfo, &sharedFenceHandle);
      if (expRes == VK_SUCCESS) {
        HookLog("Vulkan: Exported timeline semaphore as D3D12 fence, handle=%p", sharedFenceHandle);
      } else {
        HookLog("Vulkan: Failed to export timeline semaphore: %d", expRes);
      }
    } else {
      HookLog("Vulkan: vkGetSemaphoreWin32HandleKHR not available");
    }

    // Create copy-complete semaphores for each texture
    VkSemaphoreCreateInfo binarySemInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      res = vkCreateSemaphore(g_Device, &binarySemInfo, nullptr,
                              &copyCompleteSemaphores[i]);
      vkCreateSemaphore(g_Device, &binarySemInfo, nullptr, &presentTriggerSems[i]);
      vkCreateSemaphore(g_Device, &binarySemInfo, nullptr, &copyTriggerSems[i]);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: Failed to create semaphores %d: %d", i, res);
      }
    }
    // Note: D3D11 fence creation removed - using Vulkan timeline semaphore
    // exported as D3D12_FENCE_BIT instead (set in sharedFenceHandle above)

    usingD3D11Path = true;
    initialized = true;
    HookLog("Vulkan: D3D11 intermediate path initialized successfully (%dx%d)",
            w, h);
    return true;
  }

  // Import textures created by VideoEncoder (for Vulkan games)
  // This uses VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT to import
  // D3D11 textures that the encoder created and exported
  bool ImportEncoderTextures(SharedMemoryLayout *ipcMem) {
    if (!ipcMem ||
        !ipcMem->encoderTextures.ready.load(std::memory_order_acquire)) {
      return false;
    }

    auto &enc = ipcMem->encoderTextures;
    HookLog("Vulkan: ImportEncoderTextures %dx%d format=%d", enc.width,
            enc.height, enc.format);

    // Get memory properties function
    auto vkGetMemoryWin32HandlePropertiesKHR =
        (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
            g_Device, "vkGetMemoryWin32HandlePropertiesKHR");
    if (!vkGetMemoryWin32HandlePropertiesKHR) {
      HookLog("Vulkan: ERROR - vkGetMemoryWin32HandlePropertiesKHR not found");
      return false;
    }

    width = enc.width;
    height = enc.height;
    format = enc.format;
    vkFormat = MapDXGIToVulkanFormat(enc.format);

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      HANDLE texHandle = (HANDLE)enc.textureHandles[i];
      if (!texHandle) {
        HookLog("Vulkan: ERROR - Encoder texture handle %d is null", i);
        return false;
      }

      // Get memory requirements from the handle
      VkMemoryWin32HandlePropertiesKHR handleProps = {
          VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
      // D3D11 textures with D3D11_RESOURCE_MISC_SHARED_NTHANDLE export as
      // opaque NT handles
      VkResult res = vkGetMemoryWin32HandlePropertiesKHR(
          g_Device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, texHandle,
          &handleProps);
      if (res != VK_SUCCESS) {
        HookLog(
            "Vulkan: ERROR - vkGetMemoryWin32HandlePropertiesKHR failed: %d",
            res);
        return false;
      }

      // Create VkImage with external memory flag
      VkExternalMemoryImageCreateInfo extImageInfo = {
          VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
      extImageInfo.handleTypes =
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

      VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      imageInfo.pNext = &extImageInfo;
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.format = vkFormat;
      imageInfo.extent = {enc.width, enc.height, 1};
      imageInfo.mipLevels = 1;
      imageInfo.arrayLayers = 1;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      res = vkCreateImage(g_Device, &imageInfo, nullptr, &sharedImages[i]);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: ERROR - vkCreateImage failed for import: %d", res);
        return false;
      }

      // Import the D3D11 memory
      VkImportMemoryWin32HandleInfoKHR importInfo = {
          VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
      importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
      importInfo.handle = texHandle;

      VkMemoryDedicatedAllocateInfo dedicatedInfo = {
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicatedInfo.pNext = &importInfo;
      dedicatedInfo.image = sharedImages[i];

      // Get memory requirements
      VkMemoryRequirements memReq;
      vkGetImageMemoryRequirements(g_Device, sharedImages[i], &memReq);

      // Find suitable memory type
      VkPhysicalDeviceMemoryProperties memProps;
      vkGetPhysicalDeviceMemoryProperties(g_PhysDevice, &memProps);

      uint32_t memTypeIndex = UINT32_MAX;
      for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
        if ((handleProps.memoryTypeBits & (1 << j)) &&
            (memReq.memoryTypeBits & (1 << j))) {
          memTypeIndex = j;
          break;
        }
      }

      if (memTypeIndex == UINT32_MAX) {
        HookLog("Vulkan: ERROR - No suitable memory type for import");
        return false;
      }

      VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocInfo.pNext = &dedicatedInfo;
      allocInfo.allocationSize = memReq.size;
      allocInfo.memoryTypeIndex = memTypeIndex;

      res = vkAllocateMemory(g_Device, &allocInfo, nullptr, &sharedMem[i]);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: ERROR - vkAllocateMemory failed for import: %d", res);
        return false;
      }

      res = vkBindImageMemory(g_Device, sharedImages[i], this->sharedMem[i], 0);
      if (res != VK_SUCCESS) {
        HookLog("Vulkan: ERROR - vkBindImageMemory failed: %d", res);
        return false;
      }

      HookLog("Vulkan: Imported encoder texture %d, handle=%p", i, texHandle);
    }

    // Create command pool and buffers for copy operations
    VkCommandPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult res =
        vkCreateCommandPool(g_Device, &poolInfo, nullptr, &captureCommandPool);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to create command pool: %d", res);
      return false;
    }

    VkCommandBufferAllocateInfo allocInfoCB = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfoCB.commandPool = captureCommandPool;
    allocInfoCB.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfoCB.commandBufferCount = CAPTURE_TEXTURE_COUNT;

    res =
        vkAllocateCommandBuffers(g_Device, &allocInfoCB, captureCommandBuffers);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to allocate command buffers: %d", res);
      return false;
    }

    // Create timeline semaphore for synchronization
    // (We use the encoder's fence for sync, so just create a placeholder)
    VkExportSemaphoreCreateInfo exportSemInfo = {
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportSemInfo.handleTypes =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

    VkSemaphoreTypeCreateInfo timelineTypeInfo = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineTypeInfo.pNext = &exportSemInfo;
    timelineTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineTypeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semInfo.pNext = &timelineTypeInfo;

    res = vkCreateSemaphore(g_Device, &semInfo, nullptr, &timelineSemaphore);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Failed to create exportable timeline semaphore: %d", res);
      return false;
    }
    
    // Use the encoder's fence handle for synchronization
    sharedFenceHandle = (HANDLE)enc.fenceHandle;

    initialized = true;
    HookLog("Vulkan: Successfully imported encoder textures (%dx%d)", width,
            height);
    return true;
  }

  void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
    if (initialized && width == w && height == h && format == fmt)
      return;
    CleanupVulkan(g_Device);

    width = w;
    height = h;
    vkFormat = (VkFormat)fmt;
    format = (uint32_t)MapVulkanFormatToDXGI(vkFormat);

    HookLog("Vulkan: CreateSharedResources %dx%d, vkFormat=%d -> dxgiFormat=%d",
            width, height, vkFormat, format);

    // Query LUID to ensure correct device selection in MediaEngine
    // This requires Vulkan 1.1 or VK_KHR_get_physical_device_properties2
    auto vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
            g_Instance, "vkGetPhysicalDeviceProperties2");
    if (!vkGetPhysicalDeviceProperties2)
      vkGetPhysicalDeviceProperties2 =
          (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
              g_Instance, "vkGetPhysicalDeviceProperties2KHR");

    if (vkGetPhysicalDeviceProperties2) {
      VkPhysicalDeviceIDProperties idProps = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 props2 = {
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &idProps;
      vkGetPhysicalDeviceProperties2(g_PhysDevice, &props2);

      if (idProps.deviceLUIDValid) {
        memcpy(&this->luidLow, idProps.deviceLUID, 4);
        memcpy(&this->luidHigh, idProps.deviceLUID + 4, 4);
        HookLog("Vulkan: LUID %08x %08x", this->luidLow, this->luidHigh);
      }
    }

    auto GetWin32Handle = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(
        g_Device, "vkGetMemoryWin32HandleKHR");
    if (!GetWin32Handle) {
      HookLog("Vulkan: Error - vkGetMemoryWin32HandleKHR not found");
      return;
    }

    // Use member variable, do not redeclare!
    separateDeviceActive = false;

    // --- Create Internal Staging Resources (100% Stable for Graphics Queue) ---
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = vkFormat;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Graphics queue copies TO this image. Async queue copies FROM it.
        // Adding COLOR_ATTACHMENT_BIT and STORAGE_BIT for consistency
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | 
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        // Internal synchronization between Graphics -> Transfer queues
        VkSemaphoreCreateInfo semCI = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(g_Device, &semCI, nullptr, &g_GraphicsReadySems[i]);
 
        // CRITICAL: We use CONCURRENT sharing mode for staging images
        // because they are accessed by both the Graphics family and Async family.
        uint32_t families[] = { g_QueueFamily, g_AsyncQueueFamily };
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = (g_AsyncQueueFamily != UINT32_MAX && g_AsyncQueueFamily != g_QueueFamily) ? 2 : 1;
        imageInfo.pQueueFamilyIndices = families;

        if (vkCreateImage(g_Device, &imageInfo, nullptr, &g_InternalStagingImages[i]) != VK_SUCCESS) {
            HookLog("Vulkan: Error - Failed to create internal staging image %d", i);
            continue;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(g_Device, g_InternalStagingImages[i], &memReqs);

        VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(g_PhysDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(g_Device, &allocInfo, nullptr, &g_InternalStagingMemories[i]) != VK_SUCCESS) {
            HookLog("Vulkan: Error - Failed to allocate internal staging memory %d", i);
            continue;
        }
        vkBindImageMemory(g_Device, g_InternalStagingImages[i], g_InternalStagingMemories[i], 0);
    }

    // --- OPTIMIZED PATH: Single-Copy Architecture ---
    // Use D3D11 shared textures directly (imported into Vulkan).
    // The async queue on the GAME device copies swapchain → D3D11 texture in ONE operation.
    // This matches DX12 efficiency (1 copy vs previous 2 copies).
    // No separate capture device needed - eliminates complexity and overhead.
    
    if (g_AsyncQueue != VK_NULL_HANDLE && g_AsyncQueueFamily != UINT32_MAX) {
        // Create D3D11 textures and import them into Vulkan for direct copy
        if (CreateD3D11SharedResources(width, height, format)) {
            separateDeviceActive = false;  // We're NOT using separate device anymore
            usingD3D11Path = true;
            HookLog("Vulkan: Using Optimized Single-Copy Path (Async Queue on Game Device)");
        } else {
            HookLog("Vulkan: D3D11 shared resources failed, falling back to inline path");
        }
    }

    // --- Path B: Inline Path (Fallback) ---
    // Only use this if D3D11 path didn't succeed
    if (!separateDeviceActive && !usingD3D11Path) {
        HookLog("Vulkan: Using Inline Path (Fallback)");
        
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
          VkExternalMemoryImageCreateInfo extImageInfo = {
              VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
          // Use D3D11_TEXTURE_BIT for Vulkan→D3D11 export - creates handles
          // that D3D11 can open with OpenSharedResource
          extImageInfo.handleTypes =
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

          VkImageCreateInfo imageInfo = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
          imageInfo.pNext = &extImageInfo;
          imageInfo.imageType = VK_IMAGE_TYPE_2D;
          imageInfo.format = vkFormat;
          imageInfo.extent = {width, height, 1};
          imageInfo.mipLevels = 1;
          imageInfo.arrayLayers = 1;
          imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
          imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
          imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
          imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
          imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

          VkResult imageRes =
              vkCreateImage(g_Device, &imageInfo, nullptr, &sharedImages[i]);
          if (imageRes != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkCreateImage %d failed (%d)", i, imageRes);
            continue;
          }

          VkMemoryRequirements memReqs;
          vkGetImageMemoryRequirements(g_Device, sharedImages[i], &memReqs);

          VkExportMemoryWin32HandleInfoKHR winExportInfo = {
              VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
          winExportInfo.dwAccess = GENERIC_ALL;

          VkExportMemoryAllocateInfo exportInfo = {
              VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
          exportInfo.pNext = &winExportInfo;
          exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

          VkMemoryDedicatedAllocateInfo dedicatedInfo = {
              VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
          dedicatedInfo.image = sharedImages[i];
          dedicatedInfo.pNext = &exportInfo;

          VkMemoryAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
          allocInfo.pNext = &dedicatedInfo;
          allocInfo.allocationSize = memReqs.size;

          // Find memory type
          VkPhysicalDeviceMemoryProperties memProps;
          vkGetPhysicalDeviceMemoryProperties(g_PhysDevice, &memProps);
          allocInfo.memoryTypeIndex = 0;
          for (uint32_t j = 0; j < memProps.memoryTypeCount; j++) {
            if ((memReqs.memoryTypeBits & (1 << j)) &&
                (memProps.memoryTypes[j].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
              allocInfo.memoryTypeIndex = j;
              break;
            }
          }

          VkResult allocRes =
              vkAllocateMemory(g_Device, &allocInfo, nullptr, &sharedMem[i]);
          if (allocRes != VK_SUCCESS) {
            HookLog("Vulkan: Error - vkAllocateMemory %d failed (%d)", i, allocRes);
            continue;
          }
          HookLog("Vulkan: Allocated memory %d", i);
          vkBindImageMemory(g_Device, sharedImages[i], sharedMem[i], 0);

          VkMemoryGetWin32HandleInfoKHR getHandleInfo = {
              VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
          getHandleInfo.memory = sharedMem[i];
          getHandleInfo.handleType =
              VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

          GetWin32Handle(g_Device, &getHandleInfo, &sharedTextureHandles[i]);
          HookLog("Vulkan: Created shared image %d, handle: %p", i,
                  sharedTextureHandles[i]);
        }
    }

    // Create command pool and buffers for capture
    VkCommandPoolCreateInfo poolInfo = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = g_QueueFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    HookLog("Vulkan: Creating command pool...");
    VkResult poolRes =
        vkCreateCommandPool(g_Device, &poolInfo, nullptr, &captureCommandPool);
    if (poolRes != VK_SUCCESS) {
      HookLog("Vulkan: Error - vkCreateCommandPool failed (%d)", poolRes);
      return;
    }

    VkCommandBufferAllocateInfo allocCmdInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocCmdInfo.commandPool = captureCommandPool;
    allocCmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCmdInfo.commandBufferCount = CAPTURE_TEXTURE_COUNT;

    HookLog("Vulkan: Allocating command buffers...");
    VkResult cmdRes = vkAllocateCommandBuffers(g_Device, &allocCmdInfo,
                                               captureCommandBuffers);
    if (cmdRes != VK_SUCCESS) {
      HookLog("Vulkan: Error - vkAllocateCommandBuffers failed (%d)", cmdRes);
      return;
    }
    // Removed pre-recording loop to rely on dynamic recording in CopyFrame
    // This prevents baking in stale SwapchainImage handles.
    
    TryDiscoverAsyncQueue();

    // [New] Create Async Command Pool & Buffers for the Three-Stage Bridge
    // This is required in both Separate Device and Injected Queue modes.
    if (g_AsyncQueue != VK_NULL_HANDLE && g_AsyncQueueFamily != UINT32_MAX) {
      asyncQueueFamily = g_AsyncQueueFamily;
      VkCommandPoolCreateInfo asyncPoolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      asyncPoolInfo.queueFamilyIndex = asyncQueueFamily;
      asyncPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

      HookLog("Vulkan: Creating Async Command Pool (Family %d)...", asyncQueueFamily);
      if (vkCreateCommandPool(g_Device, &asyncPoolInfo, nullptr, &asyncCommandPool) == VK_SUCCESS) {
          VkCommandBufferAllocateInfo asyncAlloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
          asyncAlloc.commandPool = asyncCommandPool;
          asyncAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
          asyncAlloc.commandBufferCount = CAPTURE_TEXTURE_COUNT;
          if (vkAllocateCommandBuffers(g_Device, &asyncAlloc, asyncCommandBuffers) == VK_SUCCESS) {
            HookLog("Vulkan: Async command buffers allocated.");
          } else {
            HookLog("Vulkan: Error - Failed to allocate async command buffers");
          }
      } else {
        HookLog("Vulkan: Error - Failed to create async command pool");
      }
    }


    // Create binary semaphores for Copy -> Present sync
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      vkCreateSemaphore(g_Device, &semInfo, nullptr,
                        &copyCompleteSemaphores[i]);
      vkCreateSemaphore(g_Device, &semInfo, nullptr, &presentTriggerSems[i]);
      vkCreateSemaphore(g_Device, &semInfo, nullptr, &copyTriggerSems[i]);
    }

    // --- Timeline Semaphore (Encoder Sync) ---
    {
        VkExportSemaphoreWin32HandleInfoKHR win32ExportSemInfo = {
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        win32ExportSemInfo.dwAccess = GENERIC_ALL;

        VkExportSemaphoreCreateInfo exportSemInfo = {
            VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportSemInfo.pNext = &win32ExportSemInfo; 
        exportSemInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

        VkSemaphoreTypeCreateInfo timelineInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timelineInfo.pNext = &exportSemInfo;
        timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineInfo.initialValue = 0;

        VkSemaphoreCreateInfo timelineSemInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        timelineSemInfo.pNext = &timelineInfo;

        VkDevice targetDevice = separateDeviceActive ? g_CaptureDevice : g_Device;
        const char* deviceName = separateDeviceActive ? "CAPTURE" : "GAME";

        HookLog("Vulkan: Creating timeline semaphore on %s device...", deviceName);
        VkResult semRes = vkCreateSemaphore(targetDevice, &timelineSemInfo, nullptr, &timelineSemaphore);
        if (semRes != VK_SUCCESS || timelineSemaphore == VK_NULL_HANDLE) {
            HookLog("Vulkan: ERROR - vkCreateSemaphore (timeline) failed on %s device (%d)", deviceName, semRes);
            if (separateDeviceActive) {
                separateDeviceActive = false; // Fallback? Might be too late if other resources are already cross-device
            }
            return;
        }

        // Export the semaphore for the Encoder
        VkSemaphoreGetWin32HandleInfoKHR getSemHandleInfo = {
            VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        getSemHandleInfo.semaphore = timelineSemaphore;
        getSemHandleInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;

        auto getSemHandle = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(targetDevice, "vkGetSemaphoreWin32HandleKHR");
        if (getSemHandle) {
             if (getSemHandle(targetDevice, &getSemHandleInfo, &sharedFenceHandle) == VK_SUCCESS) {
                 HookLog("Vulkan: Exported shared fence handle from %s device: %p", deviceName, sharedFenceHandle);
             } else {
                 HookLog("Vulkan: ERROR - Failed to export timeline semaphore from %s device", deviceName);
             }
        } else {
             HookLog("Vulkan: ERROR - vkGetSemaphoreWin32HandleKHR not found on %s device", deviceName);
        }
    }

    initialized = true;
    HookLog("Vulkan: Capture resources initialized (%ux%u)", width, height);
  }

  // Copy swapchain image to shared texture, returns the semaphore that will
  // signal completion
  VkSemaphore CopyFrame(VkQueue queue, VkImage srcImage, uint32_t srcWidth,
                        uint32_t srcHeight,
                        const std::vector<VkSemaphore> &waitSemaphores) {
    if (!initialized || !captureCommandBuffers[0])
      return VK_NULL_HANDLE;

    // Use current writeIndex to pick resources
    int idx = writeIndex;
    VkImage dstImage = separateDeviceActive ? g_InternalStagingImages[idx] : sharedImages[idx];
    VkCommandBuffer cmd = captureCommandBuffers[idx];
    VkSemaphore signalSem = copyCompleteSemaphores[idx];

    if (!dstImage || !cmd || !signalSem)
      return VK_NULL_HANDLE;

    // Reset command buffer for new frame
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition source to TRANSFER_SRC
    VkImageMemoryBarrier srcBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.srcAccessMask = 0;
    srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.image = srcImage;
    srcBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Transition dest to TRANSFER_DST
    VkImageMemoryBarrier dstBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcAccessMask = 0;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.image = dstImage;
    dstBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier barriers[] = {srcBarrier, dstBarrier};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, barriers);

    // Copy
    VkImageCopy region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {srcWidth, srcHeight, 1};
    vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition source back to PRESENT_SRC
    srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    srcBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    srcBarrier.dstAccessMask = 0;

    // Transition dest to TRANSFER_SRC for the Async Queue (Next Step)
    dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    // IF separateDeviceActive, we are using EXCLUSIVE sharing for sharedImages,
    // so we need a Release barrier here on the GAME queue.
    if (separateDeviceActive) {
        dstBarrier.srcQueueFamilyIndex = g_QueueFamily;
        dstBarrier.dstQueueFamilyIndex = g_AsyncQueueFamily;
    }

    barriers[0] = srcBarrier;
    barriers[1] = dstBarrier;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, barriers);

    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    // Wait for App's semaphores
    std::vector<VkPipelineStageFlags> waitStages(
        waitSemaphores.size(), VK_PIPELINE_STAGE_TRANSFER_BIT);
    submitInfo.waitSemaphoreCount = (uint32_t)waitSemaphores.size();
    submitInfo.pWaitSemaphores = waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.data();

    if (separateDeviceActive) {
        // --- STABLE THREE-STAGE SUBMISSION ---
        // 1. Submit Local Copy to Staging on Game Queue
        // Signal signalSem (Present trigger) AND g_GraphicsReadySems[idx] (Transfer Bridge trigger)
        VkSemaphore signalSemaphores[] = {signalSem, g_GraphicsReadySems[idx]};
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = signalSemaphores;

        {
             std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
             HookLog("Vulkan: Submitting Stage 1 (Index %d)...", idx);
             VkResult gameSubmitRes = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
             if (gameSubmitRes != VK_SUCCESS) {
                 HookLog("Vulkan: ERROR - Game queue submit failed: %d (Queue=%p, Families=[%d,%d])", 
                         gameSubmitRes, queue, g_QueueFamily, g_AsyncQueueFamily);
             }
        }
    } else {
        // --- INLINE SUBMISSION (Legacy) ---
        // Signal timeline semaphore (for IPC) AND copyCompleteSemaphore (for Present) directly
        fenceValue++;
        uint64_t signalValue = fenceValue;
        VkTimelineSemaphoreSubmitInfo timelineSubmitInfo = {
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timelineSubmitInfo.signalSemaphoreValueCount = 1;
        timelineSubmitInfo.pSignalSemaphoreValues = &signalValue;

        VkSemaphore signalSemaphores[] = {timelineSemaphore, signalSem};
        uint64_t signalValues[] = {signalValue, 0}; // 0 for binary semaphore ignored

        submitInfo.pNext = &timelineSubmitInfo;
        submitInfo.signalSemaphoreCount = 2;
        submitInfo.pSignalSemaphores = signalSemaphores;
        timelineSubmitInfo.signalSemaphoreValueCount = 2;
        timelineSubmitInfo.pSignalSemaphoreValues = signalValues;

        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    }

    // Update write index
    writeIndex = (writeIndex + 1) % CAPTURE_TEXTURE_COUNT;

    return signalSem;
  }
};
static VulkanCapture g_VulkanCapture;

struct VulkanSwapchain {
  VkSwapchainKHR swapchain;
  VkDevice device;
  std::vector<VkImage> images;
  std::vector<VkImageView> imageViews; // Track for cleanup
  std::vector<VkFramebuffer> framebuffers;
  uint32_t width, height;
  VkFormat format;
  VkSharingMode sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  void Cleanup() {
    for (auto fb : framebuffers) {
      vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers.clear();
    for (auto iv : imageViews) {
      vkDestroyImageView(device, iv, nullptr);
    }
    imageViews.clear();
    images.clear();
  }
};
static std::vector<VulkanSwapchain> g_Swapchains;

// Note: Vulkan async capture code enabled
static void AsyncCaptureThreadProc() {
  HookLog("Vulkan: AsyncCaptureThread Started (Stable Three-Stage Mode)");
  g_VulkanCapture.captureThreadRunning = true;
  
  // VkQueue activeQueue = g_AsyncQueue; // Removed unused
  if (g_AsyncQueue == VK_NULL_HANDLE && g_CaptureQueue == VK_NULL_HANDLE) {
     HookLog("Vulkan: ERROR - AsyncCaptureThread started without any valid queue. Exiting thread.");
     g_VulkanCapture.captureThreadRunning = false;
     return;
  }
  
  static VkFence cpuWaitFence = VK_NULL_HANDLE;
  if (cpuWaitFence == VK_NULL_HANDLE && g_CaptureDevice != VK_NULL_HANDLE) {
      VkFenceCreateInfo fenceCI = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vkCreateFence(g_CaptureDevice, &fenceCI, nullptr, &cpuWaitFence);
  }

  while (!g_VulkanCapture.captureThreadShutdown) {
    DWORD waitResult = WaitForSingleObject(g_VulkanCapture.captureEvent, 16);
    if (waitResult != WAIT_OBJECT_0) continue;

    while (true) {
      uint32_t wIdxGlobal = g_VulkanCapture.pendingWriteIdx.load(std::memory_order_acquire);
      uint32_t rIdx = g_VulkanCapture.pendingReadIdx.load(std::memory_order_acquire);
      if (rIdx >= wIdxGlobal) break;

      PendingCaptureFrame &frame = g_VulkanCapture.pendingRing[rIdx % CAPTURE_RING_SIZE];
      int writeIdx = frame.backBufferIndex; // USE THE INDEX PASSED FROM THE GAME THREAD
      bool separateDeviceActive = (g_CaptureDevice != VK_NULL_HANDLE);
      
      HookLog("Vulkan: Async - Processing Frame (RIdx=%d, WriteIdx=%d, Separate=%d)", rIdx, writeIdx, separateDeviceActive);

      if (separateDeviceActive) {
          // --- STABLE THREE-STAGE PIPE ---
          VkCommandBuffer transCmd = g_VulkanCapture.asyncCommandBuffers[writeIdx];
          VkSemaphore semGraphicsReady = g_GraphicsReadySems[writeIdx];
          VkSemaphore semGameImported = g_CrossDeviceSems_GameImported[writeIdx];
          VkImage srcStage = g_InternalStagingImages[writeIdx];
          VkImage dstExport = g_VulkanCapture.sharedImages[writeIdx];

          if (transCmd && semGraphicsReady && semGameImported && srcStage && dstExport && g_AsyncQueue) {
              HookLog("Vulkan: Async - Stage 2: Bridge Copy (Index %d)...", writeIdx);
              vkResetCommandBuffer(transCmd, 0);
              VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
              begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
              vkBeginCommandBuffer(transCmd, &begin);

              // 1. Transition Dst (Exportable) Image to TRANSFER_DST
              // Since we are in EXCLUSIVE mode, we need an ACQUIRE barrier here on the ASYNC queue.
              VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
              barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              barrier.image = dstExport;
              barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // We don't care about previous layout if we just acquired it
              barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
              barrier.srcAccessMask = 0;
              barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
              barrier.srcQueueFamilyIndex = g_QueueFamily;
              barrier.dstQueueFamilyIndex = g_AsyncQueueFamily;
              barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
              vkCmdPipelineBarrier(transCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

              // 2. Bridge Copy: Staging -> Exportable
              VkImageCopy region = {};
              region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
              region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
              region.extent = {g_VulkanCapture.width, g_VulkanCapture.height, 1};
              vkCmdCopyImage(transCmd, srcStage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstExport, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

              // 3. Transition Dst back to GENERAL for Encoder access
              barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
              barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
              barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
              barrier.dstAccessMask = 0;
              vkCmdPipelineBarrier(transCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

              vkEndCommandBuffer(transCmd);

              VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
              VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
              submit.waitSemaphoreCount = 1;
              submit.pWaitSemaphores = &semGraphicsReady;
              submit.pWaitDstStageMask = &waitStage;
              submit.commandBufferCount = 1;
              submit.pCommandBuffers = &transCmd;
              submit.signalSemaphoreCount = 1;
              submit.pSignalSemaphores = &semGameImported;

              {
                  std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
                  VkResult res = vkQueueSubmit(g_AsyncQueue, 1, &submit, VK_NULL_HANDLE);
                  if (res != VK_SUCCESS) {
                      HookLog("Vulkan: Async - ERROR: Bridge Submit failed: %d", res);
                  } else {
                      HookLog("Vulkan: Async - Stage 2 Submit Complete");
                  }
              }
          } else {
              HookLog("Vulkan: Async - ERROR: Missing resources for Stage 2 (Cmd=%p, SemG=%p, SemI=%p, Src=%p, Dst=%p, Q=%p)", 
                      transCmd, semGraphicsReady, semGameImported, srcStage, dstExport, g_AsyncQueue);
          }

          VkSemaphore semCaptureOwned = g_CrossDeviceSems_CaptureOwned[writeIdx];
          if (g_CaptureQueue && semCaptureOwned && cpuWaitFence) {
              HookLog("Vulkan: Async - Stage 3: Separate Sync (Index %d)...", writeIdx);
              vkResetFences(g_CaptureDevice, 1, &cpuWaitFence);
              VkPipelineStageFlags syncWaitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
              VkSubmitInfo syncSubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
              syncSubmit.waitSemaphoreCount = 1;
              syncSubmit.pWaitSemaphores = &semCaptureOwned;
              syncSubmit.pWaitDstStageMask = &syncWaitStage;

              {
                  std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
                  VkResult res = vkQueueSubmit(g_CaptureQueue, 1, &syncSubmit, cpuWaitFence);
                  if (res != VK_SUCCESS) {
                      HookLog("Vulkan: Async - ERROR: Sync Submit failed: %d", res);
                  }
              }
              vkWaitForFences(g_CaptureDevice, 1, &cpuWaitFence, VK_TRUE, 1000000000); 
              HookLog("Vulkan: Async - Stage 3 Sync Complete");
          } else {
              HookLog("Vulkan: Async - ERROR: Missing resources for Stage 3 (Q=%p, SemC=%p, Fence=%p)", 
                      g_CaptureQueue, semCaptureOwned, cpuWaitFence);
          }

          g_VulkanCapture.fenceValue++;
          uint64_t signalValue = g_VulkanCapture.fenceValue;
          VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
          uint64_t signalValues[] = { signalValue }; 
          timelineInfo.signalSemaphoreValueCount = 1;
          timelineInfo.pSignalSemaphoreValues = signalValues;

          VkSemaphore signalSems[] = { g_VulkanCapture.timelineSemaphore };
          VkSubmitInfo finalSubmit = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; 
          finalSubmit.pNext = &timelineInfo;
          finalSubmit.signalSemaphoreCount = 1;
          finalSubmit.pSignalSemaphores = signalSems;

          {
              std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
              VkResult res = vkQueueSubmit(g_CaptureQueue, 1, &finalSubmit, VK_NULL_HANDLE);
              if (res != VK_SUCCESS) {
                  HookLog("Vulkan: Async - ERROR: Timeline Submit failed: %d", res);
              } else {
                  HookLog("Vulkan: Async - Frame %d Ready (Fence=%llu)", writeIdx, signalValue);
              }
          }
          // PASS RAW QPC: MediaEngine converts to MS using trusted frequency
          g_VulkanCapture.SignalFrameReady(g_IPC, writeIdx, frame.timestampQPC, signalValue);

      } else {
          // --- LEGACY ASYNC PATH (Standard Copy) ---
          VkCommandBuffer cmd = g_VulkanCapture.asyncCommandBuffers[writeIdx];
          VkImage srcImage = (VkImage)frame.apiData;
          VkImage dstImage = g_VulkanCapture.sharedImages[writeIdx];
          VkSemaphore queueWaitSem = (VkSemaphore)frame.syncObject;

          if (cmd && srcImage && dstImage && g_AsyncQueue) {
              HookLog("Vulkan: Async - Processing Standard Frame...");
              vkResetCommandBuffer(cmd, 0);
              VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
              begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
              vkBeginCommandBuffer(cmd, &begin);

              VkImageMemoryBarrier barriers[2] = {};
              barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              barriers[0].image = srcImage;
              barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
              barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
              barriers[0].srcAccessMask = 0;
              barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
              barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

              barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              barriers[1].image = dstImage;
              barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
              barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
              barriers[1].srcAccessMask = 0;
              barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
              barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

              vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
              
              VkImageCopy region = {};
              region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
              region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
              region.extent = {g_VulkanCapture.width, g_VulkanCapture.height, 1};
              vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

              barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
              barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
              barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
              barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
              vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
              vkEndCommandBuffer(cmd);

              VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
              VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
              if (queueWaitSem) {
                  submit.waitSemaphoreCount = 1;
                  submit.pWaitSemaphores = &queueWaitSem;
                  submit.pWaitDstStageMask = &waitStage;
              }
              submit.commandBufferCount = 1;
              submit.pCommandBuffers = &cmd;

              g_VulkanCapture.fenceValue++;
              uint64_t signalValue = g_VulkanCapture.fenceValue;
              VkTimelineSemaphoreSubmitInfo timelineInfo = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
              uint64_t signalVals[] = { signalValue, 0 };
              timelineInfo.signalSemaphoreValueCount = 2;
              timelineInfo.pSignalSemaphoreValues = signalVals;
              
              VkSemaphore signalSems[] = { g_VulkanCapture.timelineSemaphore, g_VulkanCapture.presentTriggerSems[writeIdx] };
              submit.pNext = &timelineInfo;
              submit.signalSemaphoreCount = 2;
              submit.pSignalSemaphores = signalSems;

              {
                  std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
                  vkQueueSubmit(g_AsyncQueue, 1, &submit, VK_NULL_HANDLE);
              }
              // PASS RAW QPC: MediaEngine converts to MS using trusted frequency
              g_VulkanCapture.SignalFrameReady(g_IPC, writeIdx, frame.timestampQPC, signalValue);
          }
      }
      
      g_VulkanCapture.pendingReadIdx.store(rIdx + 1, std::memory_order_release);
    }
  }

  if (cpuWaitFence) vkDestroyFence(g_CaptureDevice, cpuWaitFence, nullptr);
  g_VulkanCapture.captureThreadRunning = false;
  HookLog("Vulkan: AsyncCaptureThread Exiting");
}

static bool TryRecoverPhysicalDevice() {
  if (g_PhysDevice != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) 
    return true;  // Already have everything
  
  HookLog("Vulkan: Attempting late injection recovery...");
  
  // Create a temporary instance to enumerate physical devices
  VkInstance tempInstance = VK_NULL_HANDLE;
  
  VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  appInfo.pApplicationName = "CaptureEngine Recovery";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "CaptureEngine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_1;
  
  const char* instExtensions[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME
  };
  
  VkInstanceCreateInfo createInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  createInfo.pApplicationInfo = &appInfo;
  createInfo.enabledExtensionCount = 3;
  createInfo.ppEnabledExtensionNames = instExtensions;
  
  // Use the original create function if available, otherwise load directly
  auto pfnCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(nullptr, "vkCreateInstance");
  if (!pfnCreateInstance) {
    HookLog("Vulkan: Recovery failed - can't get vkCreateInstance");
    return false;
  }
  
  VkResult res = pfnCreateInstance(&createInfo, nullptr, &tempInstance);
  if (res != VK_SUCCESS) {
    HookLog("Vulkan: Recovery failed - vkCreateInstance returned %d", res);
    return false;
  }
  
  // Store as our instance (we don't have the game's anyway)
  if (g_Instance == VK_NULL_HANDLE) {
    g_Instance = tempInstance;
  }
  
  // Enumerate physical devices
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(tempInstance, &deviceCount, nullptr);
  if (deviceCount == 0) {
    HookLog("Vulkan: Recovery failed - no physical devices found");
    vkDestroyInstance(tempInstance, nullptr);
    if (g_Instance == tempInstance) g_Instance = VK_NULL_HANDLE;
    return false;
  }
  
  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(tempInstance, &deviceCount, devices.data());
  
  // Find the first discrete GPU (prefer NVIDIA/AMD)
  VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;
  for (auto& device : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      selectedDevice = device;
      HookLog("Vulkan: Recovery found discrete GPU: %s", props.deviceName);
      break;
    }
  }
  
  if (selectedDevice == VK_NULL_HANDLE) {
    selectedDevice = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(selectedDevice, &props);
    HookLog("Vulkan: Recovery using first GPU: %s", props.deviceName);
  }
  
  g_PhysDevice = selectedDevice;
  
  // If we still need a device, create one
  if (g_Device == VK_NULL_HANDLE) {
    // Find a graphics queue family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_PhysDevice, &queueFamilyCount, queueFamilies.data());
    
    uint32_t graphicsFamily = UINT32_MAX;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
      if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        graphicsFamily = i;
        break;
      }
    }
    
    if (graphicsFamily == UINT32_MAX) {
      HookLog("Vulkan: Recovery failed - no graphics queue family");
      return false;
    }
    
    g_QueueFamily = graphicsFamily;
    
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueCreateInfo.queueFamilyIndex = graphicsFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;
    
    const char* deviceExtensions[] = {
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME
    };
    
    VkDeviceCreateInfo deviceCreateInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = 6;
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;
    
    res = vkCreateDevice(g_PhysDevice, &deviceCreateInfo, nullptr, &g_Device);
    if (res != VK_SUCCESS) {
      HookLog("Vulkan: Recovery failed - vkCreateDevice returned %d", res);
      return false;
    }
    
    // Get the queue
    vkGetDeviceQueue(g_Device, graphicsFamily, 0, &g_Queue);
    HookLog("Vulkan: Recovery created device and queue successfully");
  }
  
  return true;
}


// Function pointers
typedef VkResult(VKAPI_PTR *PFN_vkCreateInstance)(const VkInstanceCreateInfo *,
                                                  const VkAllocationCallbacks *,
                                                  VkInstance *);
typedef VkResult(VKAPI_PTR *PFN_vkCreateDevice)(VkPhysicalDevice,
                                                const VkDeviceCreateInfo *,
                                                const VkAllocationCallbacks *,
                                                VkDevice *);
typedef void(VKAPI_PTR *PFN_vkGetDeviceQueue)(VkDevice, uint32_t, uint32_t,
                                              VkQueue *);
typedef VkResult(VKAPI_PTR *PFN_vkCreateSwapchainKHR)(
    VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
    VkSwapchainKHR *);
typedef VkResult(VKAPI_PTR *PFN_vkGetSwapchainImagesKHR)(VkDevice,
                                                         VkSwapchainKHR,
                                                         uint32_t *, VkImage *);
typedef VkResult(VKAPI_PTR *PFN_vkAcquireNextImageKHR)(VkDevice, VkSwapchainKHR,
                                                       uint64_t, VkSemaphore,
                                                       VkFence, uint32_t *);
typedef VkResult(VKAPI_PTR *PFN_vkQueuePresentKHR)(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
typedef VkResult(VKAPI_PTR *PFN_vkQueueSubmit)(VkQueue queue, uint32_t submitCount,
                                               const VkSubmitInfo *pSubmits,
                                               VkFence fence);
typedef VkResult(VKAPI_PTR *PFN_vkQueueSubmit2)(VkQueue queue, uint32_t submitCount,
                                                const VkSubmitInfo2 *pSubmits,
                                                VkFence fence);
typedef void(VKAPI_PTR *PFN_vkUpdateDescriptorSets)(VkDevice device,
                                                    uint32_t descriptorWriteCount,
                                                    const VkWriteDescriptorSet *pDescriptorWrites,
                                                    uint32_t descriptorCopyCount,
                                                    const VkCopyDescriptorSet *pDescriptorCopies);
typedef void(VKAPI_PTR *PFN_vkDestroySwapchainKHR)(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *pAllocator);
typedef void(VKAPI_PTR *PFN_vkDestroySampler)(VkDevice device, VkSampler sampler,
                                              const VkAllocationCallbacks *pAllocator);
typedef VkResult(VKAPI_PTR *PFN_vkCreateDescriptorUpdateTemplate)(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate);
typedef void(VKAPI_PTR *PFN_vkDestroyDescriptorUpdateTemplate)(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks *pAllocator);
typedef void(VKAPI_PTR *PFN_vkUpdateDescriptorSetWithTemplate)(
    VkDevice device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void *pData);
static PFN_vkCreateInstance o_vkCreateInstance = nullptr;
static PFN_vkCreateDevice o_vkCreateDevice = nullptr;
static PFN_vkGetDeviceQueue o_vkGetDeviceQueue = nullptr;
static PFN_vkCreateSwapchainKHR o_vkCreateSwapchainKHR = nullptr;
static PFN_vkGetSwapchainImagesKHR o_vkGetSwapchainImagesKHR = nullptr;
static PFN_vkAcquireNextImageKHR o_vkAcquireNextImageKHR = nullptr;
static PFN_vkQueuePresentKHR o_vkQueuePresentKHR = nullptr;
static PFN_vkQueueSubmit o_vkQueueSubmit = nullptr;
static PFN_vkQueueSubmit2 o_vkQueueSubmit2KHR = nullptr;
static PFN_vkQueueSubmit2 o_vkQueueSubmit2 = nullptr;
static PFN_vkDestroySwapchainKHR o_vkDestroySwapchainKHR = nullptr;
static PFN_vkCreateWin32SurfaceKHR o_vkCreateWin32SurfaceKHR = nullptr;
static PFN_vkGetDeviceQueue2 o_vkGetDeviceQueue2 = nullptr;
static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;

static PFN_vkCreateImage o_vkCreateImage = nullptr;
static PFN_vkCreateRenderPass o_vkCreateRenderPass = nullptr;
static PFN_vkCreateRenderPass2 o_vkCreateRenderPass2 = nullptr;
static PFN_vkCreateGraphicsPipelines o_vkCreateGraphicsPipelines = nullptr;
static PFN_vkCreateSampler o_vkCreateSampler = nullptr;
static PFN_vkUpdateDescriptorSets o_vkUpdateDescriptorSets = nullptr;
static PFN_vkDestroySampler o_vkDestroySampler = nullptr;
static PFN_vkCreateDescriptorUpdateTemplate o_vkCreateDescriptorUpdateTemplate = nullptr;
static PFN_vkCreateDescriptorUpdateTemplate o_vkCreateDescriptorUpdateTemplateKHR = nullptr;
static PFN_vkDestroyDescriptorUpdateTemplate o_vkDestroyDescriptorUpdateTemplate = nullptr;
static PFN_vkDestroyDescriptorUpdateTemplate o_vkDestroyDescriptorUpdateTemplateKHR = nullptr;
static PFN_vkUpdateDescriptorSetWithTemplate o_vkUpdateDescriptorSetWithTemplate = nullptr;
static PFN_vkUpdateDescriptorSetWithTemplate o_vkUpdateDescriptorSetWithTemplateKHR = nullptr;

static VkCommandPool g_CommandPool = VK_NULL_HANDLE;
static std::vector<VkCommandBuffer> g_CommandBuffers;

struct SamplerRecord {
  VkDevice device = VK_NULL_HANDLE;
  VkSamplerCreateInfo baseCI = {};
  VkSampler forcedSampler = VK_NULL_HANDLE;
};

static std::mutex g_SamplerMutex;
static std::unordered_map<VkSampler, SamplerRecord> g_SamplerRecords;

struct DescriptorTemplateEntry {
  VkDescriptorType descriptorType;
  uint32_t descriptorCount;
  size_t offset;
  size_t stride;
};

struct DescriptorTemplateRecord {
  VkDevice device = VK_NULL_HANDLE;
  std::vector<DescriptorTemplateEntry> entries;
};

static std::mutex g_TemplateMutex;
static std::unordered_map<VkDescriptorUpdateTemplate, DescriptorTemplateRecord> g_TemplateRecords;

static VkSampler GetOrCreateForcedSampler(VkSampler originalSampler, const GraphicsConfig &gfx) {
  std::lock_guard<std::mutex> lock(g_SamplerMutex);
  auto it = g_SamplerRecords.find(originalSampler);
  if (it == g_SamplerRecords.end()) return VK_NULL_HANDLE;

  SamplerRecord &rec = it->second;
  if (rec.forcedSampler != VK_NULL_HANDLE) return rec.forcedSampler;
  if (rec.device == VK_NULL_HANDLE) return VK_NULL_HANDLE;

  VkSamplerCreateInfo ci = rec.baseCI;

  int anisotropyLevel = 1;
  const char *afStr = gfx.anisotropicFiltering.c_str();
  if (afStr && afStr[0] != '\0' && strncmp(afStr, "default", 7) != 0) {
    if (strncmp(afStr, "off", 3) == 0) {
      anisotropyLevel = 1;
    } else {
      char *endPtr = nullptr;
      long parsed = strtol(afStr, &endPtr, 10);
      if (endPtr != afStr && parsed > 0) {
        anisotropyLevel = (int)parsed;
      } else if (sscanf(afStr, "%dx", &anisotropyLevel) != 1) {
        anisotropyLevel = 1;
      }
    }
  }

  if (anisotropyLevel > 1 && g_SamplerAnisotropySupported && !ci.unnormalizedCoordinates) {
    ci.anisotropyEnable = VK_TRUE;
    float target = (float)anisotropyLevel;
    if (g_MaxSamplerAnisotropy > 0.0f && target > g_MaxSamplerAnisotropy) {
      target = g_MaxSamplerAnisotropy;
    }
    ci.maxAnisotropy = target;

    ci.minFilter = VK_FILTER_LINEAR;
    ci.magFilter = VK_FILTER_LINEAR;
  }

  const bool allowMipOverrides = (ci.maxLod > 0.25f);

  const char *biasStr = gfx.mipBias.c_str();
  if (allowMipOverrides && biasStr && biasStr[0] != '\0' && strncmp(biasStr, "default", 7) != 0) {
    float userBias = 0.0f;
    if (sscanf(biasStr, "%f", &userBias) == 1) {
      const std::string &mode = gfx.mipBiasMode;
      if (mode == "offset") {
        ci.mipLodBias = rec.baseCI.mipLodBias + userBias;
      } else if (mode == "base") {
        if (userBias < 0.0f) {
          ci.mipLodBias = rec.baseCI.mipLodBias + userBias;
        }
      } else {
        ci.mipLodBias = userBias;
      }
    }
  }

  const char *mmStr = gfx.mipMapping.c_str();
  if (allowMipOverrides && mmStr && mmStr[0] != '\0' && strncmp(mmStr, "default", 7) != 0) {
    if (strncmp(mmStr, "bilinear", 8) == 0) {
      ci.minFilter = VK_FILTER_LINEAR;
      ci.magFilter = VK_FILTER_LINEAR;
      ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    } else if (strncmp(mmStr, "trilinear", 9) == 0) {
      ci.minFilter = VK_FILTER_LINEAR;
      ci.magFilter = VK_FILTER_LINEAR;
      ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    } else if (strncmp(mmStr, "nearest", 7) == 0) {
      ci.minFilter = VK_FILTER_NEAREST;
      ci.magFilter = VK_FILTER_NEAREST;
      ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
  }

  VkSampler forced = VK_NULL_HANDLE;
  VkResult r = o_vkCreateSampler(rec.device, &ci, nullptr, &forced);
  if (r != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }

  rec.forcedSampler = forced;
  return forced;
}

// -- Detours --

VkResult VKAPI_CALL Detour_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
  if (g_InVulkanHook) return o_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
  VulkanHookGuard guard;

  // Try to apply NVIDIA LOD fix BEFORE instance creation
  // This catches any samplers/instances created during driver initialization triggered by o_vkCreateInstance.
  // ApplyNvidiaLodBiasFix removed

  VkResult res = o_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
  if (res == VK_SUCCESS) {
    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    g_Instance = *pInstance;
    volkLoadInstance(g_Instance);
    
    // Re-verify after load just in case driver was deferred-loaded
    // ApplyNvidiaLodBiasFix removed
    
    EarlyLog("Vulkan: Instance created (handle=%p)", *pInstance);
  }
  return res;
}

VkResult VKAPI_CALL Detour_vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    if (pCreateInfo && g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            VkImageCreateInfo modifiedCI = *pCreateInfo;
            if (strcmp(msaa, "off") == 0) {
                modifiedCI.samples = VK_SAMPLE_COUNT_1_BIT;
            } else {
                VkSampleCountFlagBits samples = ParseVulkanMSAA(msaa);
                if (samples > VK_SAMPLE_COUNT_1_BIT) {
                    // Only upgrade if it's an attachment candidate (color/depth)
                    if (modifiedCI.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
                        modifiedCI.samples = samples;
                        // HookLog("Vulkan: Forcing MSAA %dx for Image", (int)samples);
                    }
                }
            }
            return o_vkCreateImage(device, &modifiedCI, pAllocator, pImage);
        }
    }
    return o_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
}

VkResult VKAPI_CALL Detour_vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (pCreateInfo && g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            VkSampleCountFlagBits samples = ParseVulkanMSAA(msaa);
            if (samples > VK_SAMPLE_COUNT_1_BIT || strcmp(msaa, "off") == 0) {
                if (strcmp(msaa, "off") == 0) samples = VK_SAMPLE_COUNT_1_BIT;
                
                std::vector<VkAttachmentDescription> attachments(pCreateInfo->attachmentCount);
                for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                    attachments[i] = pCreateInfo->pAttachments[i];
                    // Upgrade attachment samples to match forced MSAA
                    // Note: Depth/Stencil should also be upgraded to match color
                    attachments[i].samples = samples;
                }
                VkRenderPassCreateInfo modifiedCI = *pCreateInfo;
                modifiedCI.pAttachments = attachments.data();
                return o_vkCreateRenderPass(device, &modifiedCI, pAllocator, pRenderPass);
            }
        }
    }
    return o_vkCreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);
}

VkResult VKAPI_CALL Detour_vkCreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    if (pCreateInfo && g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            VkSampleCountFlagBits samples = ParseVulkanMSAA(msaa);
            if (samples > VK_SAMPLE_COUNT_1_BIT || strcmp(msaa, "off") == 0) {
                 if (strcmp(msaa, "off") == 0) samples = VK_SAMPLE_COUNT_1_BIT;
                 
                 std::vector<VkAttachmentDescription2> attachments(pCreateInfo->attachmentCount);
                 for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
                     attachments[i] = pCreateInfo->pAttachments[i];
                     attachments[i].samples = samples;
                 }
                 VkRenderPassCreateInfo2 modifiedCI = *pCreateInfo;
                 modifiedCI.pAttachments = attachments.data();
                 return o_vkCreateRenderPass2(device, &modifiedCI, pAllocator, pRenderPass);
            }
        }
    }
    return o_vkCreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);
}

static VkResult VKAPI_PTR Detour_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) 
{
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->graphicsConfig.sgssaa) {
        std::vector<VkGraphicsPipelineCreateInfo> modifiedInfos(createInfoCount);
        std::vector<VkPipelineMultisampleStateCreateInfo> modifiedMsStates(createInfoCount);
        
        bool modified = false;
        for (uint32_t i = 0; i < createInfoCount; i++) {
            modifiedInfos[i] = pCreateInfos[i];
            
            if (pCreateInfos[i].pMultisampleState) {
                // Determine effective sample count (either native or our forced MSAA)
                // Since forced MSAA happens at Image/RenderPass creation, the pipeline state MUST match.
                // However, the game might still be asking for 1x here if it doesn't know about our override.
                // But blindly changing this might break things if the render pass is incompatible.
                // For SGSSAA, we primarily want to enable Sample Shading IF MSAA is active.
                
                VkSampleCountFlagBits samples = pCreateInfos[i].pMultisampleState->rasterizationSamples;
                
                // Check if we are proactively forcing MSAA globally? 
                // Currently ParseVulkanMSAA returns non-1 only if forced.
                VkSampleCountFlagBits forcedSamples = ParseVulkanMSAA(g_IPC->GetSharedMem()->graphicsConfig.msaaSamples);
                if (forcedSamples != VK_SAMPLE_COUNT_1_BIT) {
                    samples = forcedSamples;
                }
                
                if (samples > VK_SAMPLE_COUNT_1_BIT) {
                    // Copy MS state to modify
                    modifiedMsStates[i] = *pCreateInfos[i].pMultisampleState;
                    
                    if (g_SampleRateShadingSupported) {
                        modifiedMsStates[i].sampleShadingEnable = VK_TRUE;
                        modifiedMsStates[i].minSampleShading = 1.0f; // Force full per-sample shading
                    }
                    
                    // If we are forcing MSAA, ensure rasterizationSamples matches
                    if (forcedSamples != VK_SAMPLE_COUNT_1_BIT) {
                        modifiedMsStates[i].rasterizationSamples = forcedSamples;
                    }

                    modifiedInfos[i].pMultisampleState = &modifiedMsStates[i];
                    modified = true;
                    // HookLog("Vulkan: SGSSAA enabled for pipeline %d (samples=%d, shading=1.0)", i, samples);
                }
            }
        }
        
        if (modified) {
            return o_vkCreateGraphicsPipelines(device, pipelineCache, createInfoCount, modifiedInfos.data(), pAllocator, pPipelines);
        }
    }
    
    return o_vkCreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

static VkResult VKAPI_PTR Detour_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler)
{
    static int s_createSamplerCallCount = 0;
    s_createSamplerCallCount++;
    if (s_createSamplerCallCount <= 5) {
        HookLog("Vulkan: Detour_vkCreateSampler called (#%d) o_vkCreateSampler=%p IPC=%p SharedMem=%p",
                s_createSamplerCallCount, (void*)o_vkCreateSampler, 
                (void*)g_IPC, g_IPC ? (void*)g_IPC->GetSharedMem() : nullptr);
    }

    if (!o_vkCreateSampler) {
        HookLog("Vulkan: ERROR - o_vkCreateSampler is NULL!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSamplerCreateInfo modifiedInfo = *pCreateInfo;
    
    if (g_IPC && g_IPC->GetSharedMem()) {
         const auto& gfx = GetActiveGraphicsConfig();

         const bool allowMipOverrides = (pCreateInfo->maxLod > 0.25f);

         const bool anyOverrides =
             (gfx.anisotropicFiltering != "default" && !gfx.anisotropicFiltering.empty()) ||
             (gfx.mipMapping != "default" && !gfx.mipMapping.empty()) ||
             (gfx.mipBias != "default" && !gfx.mipBias.empty()) ||
             (gfx.sgssaa);

         if (!anyOverrides) {
             return o_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
         }
         
         // Anisotropy override
         int anisotropyLevel = 1;
         const char* afStr = gfx.anisotropicFiltering.c_str();
         if (afStr && afStr[0] != '\0' && strncmp(afStr, "default", 7) != 0) {
             if (strncmp(afStr, "off", 3) == 0) {
                 anisotropyLevel = 1;
             } else {
                 char* endPtr = nullptr;
                 long parsed = strtol(afStr, &endPtr, 10);
                 if (endPtr != afStr && parsed > 0) {
                    anisotropyLevel = (int)parsed;
                 } else if (sscanf(afStr, "%dx", &anisotropyLevel) != 1) {
                    anisotropyLevel = 1;
                 }
             }
         }

         if (anisotropyLevel > 1) {
             if (g_SamplerAnisotropySupported && !pCreateInfo->unnormalizedCoordinates) {
                 modifiedInfo.anisotropyEnable = VK_TRUE;
                 float target = (float)anisotropyLevel;
                 if (g_MaxSamplerAnisotropy > 0.0f && target > g_MaxSamplerAnisotropy) {
                    target = g_MaxSamplerAnisotropy;
                 }
                 modifiedInfo.maxAnisotropy = target;

                 // Vulkan spec requirement: when anisotropyEnable is VK_TRUE,
                 // minFilter and magFilter must be VK_FILTER_LINEAR.
                 modifiedInfo.minFilter = VK_FILTER_LINEAR;
                 modifiedInfo.magFilter = VK_FILTER_LINEAR;
             }
         }
         
         // Mip Bias Override
         const char* biasStr = gfx.mipBias.c_str();
         if (allowMipOverrides && biasStr && biasStr[0] != '\0' && strncmp(biasStr, "default", 7) != 0) {
             float userBias = 0.0f;
             if (sscanf(biasStr, "%f", &userBias) == 1) {
                 const std::string& mode = gfx.mipBiasMode;
                 if (mode == "offset") {
                    modifiedInfo.mipLodBias = pCreateInfo->mipLodBias + userBias;
                 } else if (mode == "base") {
                    if (userBias < 0.0f) {
                      modifiedInfo.mipLodBias = pCreateInfo->mipLodBias + userBias;
                    }
                 } else {
                    modifiedInfo.mipLodBias = userBias;
                 }
             }
         }

         // Mip Mapping Override (Texture Filtering)
         const char* mmStr = gfx.mipMapping.c_str();
         if (allowMipOverrides && mmStr && mmStr[0] != '\0' && strncmp(mmStr, "default", 7) != 0) {
             if (strncmp(mmStr, "bilinear", 8) == 0) {
                 modifiedInfo.minFilter = VK_FILTER_LINEAR;
                 modifiedInfo.magFilter = VK_FILTER_LINEAR;
                 modifiedInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
             } else if (strncmp(mmStr, "trilinear", 9) == 0) {
                 modifiedInfo.minFilter = VK_FILTER_LINEAR;
                 modifiedInfo.magFilter = VK_FILTER_LINEAR;
                 modifiedInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
             } else if (strncmp(mmStr, "nearest", 7) == 0) {
                 modifiedInfo.minFilter = VK_FILTER_NEAREST;
                 modifiedInfo.magFilter = VK_FILTER_NEAREST;
                 modifiedInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
             }
         }
         
         // SGSSAA Auto-Bias (Adds to existing bias)
         float sgssaaBias = 0.0f;
         if (allowMipOverrides && GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
             modifiedInfo.mipLodBias += sgssaaBias;
         }

         static int s_samplerOverrideLogCount = 0;
         if (s_samplerOverrideLogCount < 10) {
             s_samplerOverrideLogCount++;
             HookLog(
                 "Vulkan: CreateSampler override (af=%s maxLod=%.2f allowMip=%d unnorm=%d) "
                 "Aniso: %d/%.1f -> %d/%.1f  Filter: %d/%d/%d  Bias: %.3f -> %.3f",
                 gfx.anisotropicFiltering.c_str(), pCreateInfo->maxLod,
                 allowMipOverrides ? 1 : 0, (int)pCreateInfo->unnormalizedCoordinates,
                 (int)pCreateInfo->anisotropyEnable, (double)pCreateInfo->maxAnisotropy,
                 (int)modifiedInfo.anisotropyEnable, (double)modifiedInfo.maxAnisotropy,
                 (int)modifiedInfo.minFilter, (int)modifiedInfo.magFilter, (int)modifiedInfo.mipmapMode,
                 (double)pCreateInfo->mipLodBias, (double)modifiedInfo.mipLodBias);
         }
    }
    
    VkResult res = o_vkCreateSampler(device, &modifiedInfo, pAllocator, pSampler);
    if (res == VK_SUCCESS && pSampler && *pSampler != VK_NULL_HANDLE && pCreateInfo) {
      SamplerRecord rec;
      rec.device = device;
      rec.baseCI = *pCreateInfo;
      {
        std::lock_guard<std::mutex> lock(g_SamplerMutex);
        g_SamplerRecords[*pSampler] = rec;
      }
    }
    return res;
}

static void VKAPI_PTR Detour_vkDestroySampler(VkDevice device, VkSampler sampler,
                                             const VkAllocationCallbacks *pAllocator) {
  if (!o_vkDestroySampler) return;

  VkSampler forced = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lock(g_SamplerMutex);
    auto it = g_SamplerRecords.find(sampler);
    if (it != g_SamplerRecords.end()) {
      forced = it->second.forcedSampler;
      g_SamplerRecords.erase(it);
    }
  }

  if (forced != VK_NULL_HANDLE && forced != sampler) {
    o_vkDestroySampler(device, forced, nullptr);
  }
  o_vkDestroySampler(device, sampler, pAllocator);
}

static void VKAPI_PTR Detour_vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet *pDescriptorWrites, uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet *pDescriptorCopies) {
  if (!o_vkUpdateDescriptorSets) return;

  if (!g_IPC || !g_IPC->GetSharedMem() || !pDescriptorWrites || descriptorWriteCount == 0) {
    o_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
    return;
  }

  const auto &gfx = GetActiveGraphicsConfig();
  const bool afActive = (gfx.anisotropicFiltering != "default" && !gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "off");
  const bool anyMip = (gfx.mipMapping != "default" && !gfx.mipMapping.empty()) ||
                      (gfx.mipBias != "default" && !gfx.mipBias.empty()) ||
                      (gfx.sgssaa);

  if (!afActive && !anyMip) {
    o_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
    return;
  }

  bool anyChanged = false;
  std::vector<VkWriteDescriptorSet> writes(descriptorWriteCount);
  std::vector<VkDescriptorImageInfo> imageInfos;
  imageInfos.reserve(64);

  for (uint32_t i = 0; i < descriptorWriteCount; i++) {
    writes[i] = pDescriptorWrites[i];
    VkWriteDescriptorSet &w = writes[i];

    const bool isSamplerWrite = (w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
                                (w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    if (!isSamplerWrite || w.descriptorCount == 0 || !w.pImageInfo) {
      continue;
    }

    const size_t base = imageInfos.size();
    imageInfos.insert(imageInfos.end(), w.pImageInfo, w.pImageInfo + w.descriptorCount);
    w.pImageInfo = imageInfos.data() + base;

    for (uint32_t j = 0; j < w.descriptorCount; j++) {
      VkDescriptorImageInfo &di = (VkDescriptorImageInfo &)w.pImageInfo[j];
      if (di.sampler == VK_NULL_HANDLE) continue;

      VkSampler forced = GetOrCreateForcedSampler(di.sampler, gfx);
      if (forced != VK_NULL_HANDLE) {
        di.sampler = forced;
        anyChanged = true;
      }
    }
  }

  if (!anyChanged) {
    o_vkUpdateDescriptorSets(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
    return;
  }

  o_vkUpdateDescriptorSets(device, descriptorWriteCount, writes.data(), descriptorCopyCount, pDescriptorCopies);
}

static VkResult VKAPI_PTR Detour_vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate) {
  
  static int s_templateCreateCount = 0;
  s_templateCreateCount++;
  if (s_templateCreateCount <= 5) {
    HookLog("Vulkan: Detour_vkCreateDescriptorUpdateTemplate called (#%d) o_core=%p o_khr=%p entries=%u",
            s_templateCreateCount, (void*)o_vkCreateDescriptorUpdateTemplate, 
            (void*)o_vkCreateDescriptorUpdateTemplateKHR,
            pCreateInfo ? pCreateInfo->descriptorUpdateEntryCount : 0);
  }

  PFN_vkCreateDescriptorUpdateTemplate origFn = o_vkCreateDescriptorUpdateTemplate;
  if (!origFn) origFn = o_vkCreateDescriptorUpdateTemplateKHR;
  if (!origFn) {
    HookLog("Vulkan: ERROR - No original vkCreateDescriptorUpdateTemplate!");
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }

  VkResult res = origFn(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
  if (res != VK_SUCCESS || !pDescriptorUpdateTemplate || !pCreateInfo) return res;

  DescriptorTemplateRecord rec;
  rec.device = device;
  rec.entries.reserve(pCreateInfo->descriptorUpdateEntryCount);

  for (uint32_t i = 0; i < pCreateInfo->descriptorUpdateEntryCount; i++) {
    const VkDescriptorUpdateTemplateEntry &e = pCreateInfo->pDescriptorUpdateEntries[i];
    DescriptorTemplateEntry entry;
    entry.descriptorType = e.descriptorType;
    entry.descriptorCount = e.descriptorCount;
    entry.offset = e.offset;
    entry.stride = e.stride;
    rec.entries.push_back(entry);
  }

  {
    std::lock_guard<std::mutex> lock(g_TemplateMutex);
    g_TemplateRecords[*pDescriptorUpdateTemplate] = std::move(rec);
  }

  return res;
}

static void VKAPI_PTR Detour_vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks *pAllocator) {
  
  PFN_vkDestroyDescriptorUpdateTemplate origFn = o_vkDestroyDescriptorUpdateTemplate;
  if (!origFn) origFn = o_vkDestroyDescriptorUpdateTemplateKHR;
  if (!origFn) return;

  {
    std::lock_guard<std::mutex> lock(g_TemplateMutex);
    g_TemplateRecords.erase(descriptorUpdateTemplate);
  }

  origFn(device, descriptorUpdateTemplate, pAllocator);
}

static void VKAPI_PTR Detour_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorSet descriptorSet,
    VkDescriptorUpdateTemplate descriptorUpdateTemplate, const void *pData) {
  
  static int s_templateUpdateCount = 0;
  s_templateUpdateCount++;
  if (s_templateUpdateCount <= 5) {
    HookLog("Vulkan: Detour_vkUpdateDescriptorSetWithTemplate called (#%d) o_core=%p o_khr=%p",
            s_templateUpdateCount, (void*)o_vkUpdateDescriptorSetWithTemplate, 
            (void*)o_vkUpdateDescriptorSetWithTemplateKHR);
  }

  PFN_vkUpdateDescriptorSetWithTemplate origFn = o_vkUpdateDescriptorSetWithTemplate;
  if (!origFn) origFn = o_vkUpdateDescriptorSetWithTemplateKHR;
  if (!origFn) {
    HookLog("Vulkan: ERROR - No original vkUpdateDescriptorSetWithTemplate!");
    return;
  }

  if (!g_IPC || !g_IPC->GetSharedMem() || !pData) {
    origFn(device, descriptorSet, descriptorUpdateTemplate, pData);
    return;
  }

  const auto &gfx = GetActiveGraphicsConfig();
  const bool afActive = (gfx.anisotropicFiltering != "default" && 
                         !gfx.anisotropicFiltering.empty() && 
                         gfx.anisotropicFiltering != "off");
  const bool anyMip = (gfx.mipMapping != "default" && !gfx.mipMapping.empty()) ||
                      (gfx.mipBias != "default" && !gfx.mipBias.empty()) ||
                      (gfx.sgssaa);

  if (!afActive && !anyMip) {
    origFn(device, descriptorSet, descriptorUpdateTemplate, pData);
    return;
  }

  DescriptorTemplateRecord rec;
  {
    std::lock_guard<std::mutex> lock(g_TemplateMutex);
    auto it = g_TemplateRecords.find(descriptorUpdateTemplate);
    if (it == g_TemplateRecords.end()) {
      origFn(device, descriptorSet, descriptorUpdateTemplate, pData);
      return;
    }
    rec = it->second;
  }

  std::vector<uint8_t> modifiedData;
  bool anyChanged = false;

  for (const auto &entry : rec.entries) {
    const bool isSamplerType = (entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) ||
                               (entry.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    if (!isSamplerType) continue;

    for (uint32_t j = 0; j < entry.descriptorCount; j++) {
      size_t itemOffset = entry.offset + j * entry.stride;
      const VkDescriptorImageInfo *imgInfo = 
          reinterpret_cast<const VkDescriptorImageInfo *>(static_cast<const uint8_t *>(pData) + itemOffset);

      if (imgInfo->sampler == VK_NULL_HANDLE) continue;

      VkSampler forced = GetOrCreateForcedSampler(imgInfo->sampler, gfx);
      if (forced != VK_NULL_HANDLE && forced != imgInfo->sampler) {
        if (modifiedData.empty()) {
          size_t totalSize = 0;
          for (const auto &e : rec.entries) {
            size_t end = e.offset + e.descriptorCount * e.stride;
            if (end > totalSize) totalSize = end;
          }
          totalSize += sizeof(VkDescriptorImageInfo);
          modifiedData.resize(totalSize);
          memcpy(modifiedData.data(), pData, totalSize);
        }

        VkDescriptorImageInfo *modImgInfo = 
            reinterpret_cast<VkDescriptorImageInfo *>(modifiedData.data() + itemOffset);
        modImgInfo->sampler = forced;
        anyChanged = true;
      }
    }
  }

  if (anyChanged && !modifiedData.empty()) {
    origFn(device, descriptorSet, descriptorUpdateTemplate, modifiedData.data());
  } else {
    origFn(device, descriptorSet, descriptorUpdateTemplate, pData);
  }
}

VkResult VKAPI_CALL Detour_vkCreateWin32SurfaceKHR(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
  if (pCreateInfo && pCreateInfo->hwnd) {
    g_hWnd = pCreateInfo->hwnd;
    HookLog("Vulkan: Captured HWND from Surface Creation: %p", g_hWnd);
  }
  return o_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
}

VkResult VKAPI_CALL Detour_vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
  if (g_InVulkanHook) return o_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
  VulkanHookGuard guard;

  std::vector<const char *> extensions;
  for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
    extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
  }

  // --- Queue Tracking & Injection ---
  uint32_t qFamCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qFamCount, nullptr);
  std::vector<VkQueueFamilyProperties> qProps(qFamCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qFamCount, qProps.data());

  std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
  g_QueueFamilies.resize(qFamCount);
  for (uint32_t i = 0; i < qFamCount; i++) {
      g_QueueFamilies[i].totalAvailable = qProps[i].queueCount;
      g_QueueFamilies[i].flags = qProps[i].queueFlags;
      g_QueueFamilies[i].requestedCount = 0; 
      g_QueueFamilies[i].retrievedCount = 0;
  }

  // Track original requests
  for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      uint32_t famIdx = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
      if (famIdx < qFamCount) {
          g_QueueFamilies[famIdx].requestedCount += pCreateInfo->pQueueCreateInfos[i].queueCount;
      }
  }

  // Find family for Injection (Priority: Transfer > Compute > Graphics)
  int bestInjectionFam = -1;
  int bestInjectionScore = -1;
  for (uint32_t i = 0; i < qFamCount; i++) {
      if (g_QueueFamilies[i].requestedCount < g_QueueFamilies[i].totalAvailable) {
          int score = 0;
          if ((g_QueueFamilies[i].flags & VK_QUEUE_TRANSFER_BIT) && 
              !(g_QueueFamilies[i].flags & VK_QUEUE_GRAPHICS_BIT) && 
              !(g_QueueFamilies[i].flags & VK_QUEUE_COMPUTE_BIT)) score = 100;
          else if ((g_QueueFamilies[i].flags & VK_QUEUE_COMPUTE_BIT) && 
                   !(g_QueueFamilies[i].flags & VK_QUEUE_GRAPHICS_BIT)) score = 50;
          else if (g_QueueFamilies[i].flags & VK_QUEUE_TRANSFER_BIT) score = 30;
          else score = 10;

          if (score > bestInjectionScore) {
              bestInjectionScore = score;
              bestInjectionFam = i;
          }
      }
  }

  std::vector<VkDeviceQueueCreateInfo> injectedQueueCIs;
  std::vector<std::vector<float>> prioritiesStorage; 
  
  bool familyInjected = false;
  if (bestInjectionFam != -1) {
      g_AsyncQueueFamily = bestInjectionFam;
      g_AsyncQueueIndex = g_QueueFamilies[bestInjectionFam].requestedCount; // Next available index
      familyInjected = true;
      HookLog("Vulkan: Injecting extra queue for capture in Family %d (Index %d, TypeScore %d)", 
              bestInjectionFam, g_AsyncQueueIndex, bestInjectionScore);
  }

  for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      VkDeviceQueueCreateInfo ci = pCreateInfo->pQueueCreateInfos[i];
      if (familyInjected && ci.queueFamilyIndex == (uint32_t)bestInjectionFam) {
          // Increment count and provide new priority array
          uint32_t newCount = ci.queueCount + 1;
          std::vector<float> priorities(newCount);
          for (uint32_t j = 0; j < ci.queueCount; j++) priorities[j] = ci.pQueuePriorities[j];
          priorities[newCount-1] = 0.5f; // Our priority
          
          prioritiesStorage.push_back(priorities);
          ci.queueCount = newCount;
          ci.pQueuePriorities = prioritiesStorage.back().data();
          familyInjected = false; // Done
      }
      injectedQueueCIs.push_back(ci);
  }

  // If family wasn't in original list, add it
  if (familyInjected) {
      VkDeviceQueueCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      ci.queueFamilyIndex = bestInjectionFam;
      ci.queueCount = 1;
      std::vector<float> priorities = {0.5f};
      prioritiesStorage.push_back(priorities);
      ci.pQueuePriorities = prioritiesStorage.back().data();
      injectedQueueCIs.push_back(ci);
      g_AsyncQueueIndex = 0; 
  }

  // Inject Extensions
  bool hasExternalMem = false;
  bool hasExternalMemWin32 = false;
  for (auto ext : extensions) {
    if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0) hasExternalMem = true;
    if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0) hasExternalMemWin32 = true;
  }

  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> availableExts(extCount);
  vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, availableExts.data());

  auto IsSupported = [&](const char *name) {
    for (const auto &e : availableExts) if (strcmp(e.extensionName, name) == 0) return true;
    return false;
  };

  if (!hasExternalMem && IsSupported(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))
    extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
  if (!hasExternalMemWin32 && IsSupported(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME))
    extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);

  bool hasExternalSem = false;
  bool hasExternalSemWin32 = false;
  for (auto ext : extensions) {
    if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) == 0) hasExternalSem = true;
    if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0) hasExternalSemWin32 = true;
  }
  if (!hasExternalSem && IsSupported(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME))
    extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
  if (!hasExternalSemWin32 && IsSupported(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME))
    extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);

  VkDeviceCreateInfo newCI = *pCreateInfo;
  newCI.enabledExtensionCount = (uint32_t)extensions.size();
  newCI.ppEnabledExtensionNames = extensions.data();
  newCI.queueCreateInfoCount = (uint32_t)injectedQueueCIs.size();
  newCI.pQueueCreateInfos = injectedQueueCIs.data();

  // --- SGSSAA Feature Injection ---
  // We MUST enable sampleRateShading to use it in pipelines, otherwise it's a spec violation.
  VkPhysicalDeviceFeatures supportedFeatures = {};
  vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);

  VkPhysicalDeviceFeatures features = {};
  bool appUsesFeatures2 = false;
  VkPhysicalDeviceFeatures2* pFeatures2InNext = nullptr;

  // 1. Check pNext for VkPhysicalDeviceFeatures2
  struct VulkanHeader {
      VkStructureType sType;
      void* pNext;
  };
  VulkanHeader* pNext = (VulkanHeader*)pCreateInfo->pNext;
  while (pNext) {
      if (pNext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
          pFeatures2InNext = (VkPhysicalDeviceFeatures2*)pNext;
          appUsesFeatures2 = true;
          features = pFeatures2InNext->features;
          break;
      }
      pNext = (VulkanHeader*)pNext->pNext;
  }

  // 2. If not in pNext, check pEnabledFeatures
  if (!appUsesFeatures2 && pCreateInfo->pEnabledFeatures) {
      features = *pCreateInfo->pEnabledFeatures;
  }
  
  // Enable sampleRateShading if supported and we want to support SGSSAA
  if (supportedFeatures.sampleRateShading) {
    features.sampleRateShading = VK_TRUE;
    g_SampleRateShadingSupported = true;
    HookLog("Vulkan: sampleRateShading feature enabled for %s", appUsesFeatures2 ? "pNext (Features2)" : "pEnabledFeatures");
  } else {
    g_SampleRateShadingSupported = false;
    HookLog("Vulkan: WARNING - sampleRateShading NOT supported by hardware. SGSSAA will be disabled.");
  }
  
  // Enable samplerAnisotropy to support AF override
  if (supportedFeatures.samplerAnisotropy) {
    features.samplerAnisotropy = VK_TRUE;
    g_SamplerAnisotropySupported = true;
    HookLog("Vulkan: samplerAnisotropy feature enabled");
  } else {
    g_SamplerAnisotropySupported = false;
    HookLog("Vulkan: WARNING - samplerAnisotropy NOT supported by hardware. AF override will fail.");
  }

  // 3. Apply changes back to newCI
  newCI.enabledExtensionCount = (uint32_t)extensions.size();
  newCI.ppEnabledExtensionNames = extensions.data();
  newCI.queueCreateInfoCount = (uint32_t)injectedQueueCIs.size();
  newCI.pQueueCreateInfos = injectedQueueCIs.data();

  if (appUsesFeatures2) {
      // If the app used pNext, we MUST modify the struct in the pNext chain
      // and ensure pEnabledFeatures is NULL to stay spec compliant.
      pFeatures2InNext->features = features;
      newCI.pEnabledFeatures = nullptr;
  } else {
      // Otherwise, we can safely use the classic pEnabledFeatures
      newCI.pEnabledFeatures = &features;
  }

  VkResult res = o_vkCreateDevice(physicalDevice, &newCI, pAllocator, pDevice);
  if (res == VK_SUCCESS) {
    g_PhysDevice = physicalDevice;
    g_Device = *pDevice;
    volkLoadDevice(g_Device);
    HookLog("Vulkan: Device Created (with injected queue & interop extensions)");

    {
      VkPhysicalDeviceProperties props = {};
      vkGetPhysicalDeviceProperties(g_PhysDevice, &props);
      g_MaxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
    }
    
    // Initialize SystemMetricsCollector with adapter LUID for GPU stats
    // Get LUID from VkPhysicalDeviceIDProperties
    auto vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(g_Instance, "vkGetPhysicalDeviceProperties2");
    if (!vkGetPhysicalDeviceProperties2)
      vkGetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(g_Instance, "vkGetPhysicalDeviceProperties2KHR");
    
    if (vkGetPhysicalDeviceProperties2) {
      VkPhysicalDeviceIDProperties idProps = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
      VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      props2.pNext = &idProps;
      vkGetPhysicalDeviceProperties2(g_PhysDevice, &props2);
      
      if (idProps.deviceLUIDValid) {
        // LUID is 8 bytes: first 4 bytes = LowPart, next 4 bytes = HighPart
        uint32_t luidLow = *(uint32_t*)&idProps.deviceLUID[0];
        uint32_t luidHigh = *(uint32_t*)&idProps.deviceLUID[4];
        SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
        HookLog("Vulkan: SystemMetricsCollector initialized with LUID: %08X%08X", luidHigh, luidLow);
        ReportLUID(luidLow, luidHigh);

        // Set VRAM Total explicitly to prevent background thread crash
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) {
            IDXGIAdapter* adapter = nullptr;
            LUID targetLuid;
            targetLuid.LowPart = luidLow;
            targetLuid.HighPart = luidHigh;
            
            for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                if (desc.AdapterLuid.LowPart == targetLuid.LowPart && desc.AdapterLuid.HighPart == targetLuid.HighPart) {
                    SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
                    adapter->Release();
                    break;
                }
                adapter->Release();
            }
            factory->Release();
        }
      }
    }
    
    // Retrieve our private queue immediately
    if (bestInjectionFam != -1) {
        // CRITICAL FIX: o_vkGetDeviceQueue might be NULL if the app hasn't called vkGetDeviceProcAddr yet.
        // We use the original o_vkGetDeviceProcAddr to retrieve it safely from the newly created device.
        PFN_vkGetDeviceQueue pfnGetQueue = (PFN_vkGetDeviceQueue)o_vkGetDeviceProcAddr(g_Device, "vkGetDeviceQueue");
        if (pfnGetQueue) {
            pfnGetQueue(g_Device, g_AsyncQueueFamily, g_AsyncQueueIndex, &g_AsyncQueue);
            HookLog("Vulkan: Injected Async Queue retrieved: %p", g_AsyncQueue);
        } else {
            HookLog("Vulkan: ERROR - Failed to retrieve vkGetDeviceQueue from device! Async capture will fail.");
        }
    }
  }
  return res;
}

void VKAPI_CALL Detour_vkGetDeviceQueue(VkDevice device,
                                        uint32_t queueFamilyIndex,
                                        uint32_t queueIndex, VkQueue *pQueue) {
  if (g_InVulkanHook) {
    if (o_vkGetDeviceQueue) o_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    return;
  }
  if (!o_vkGetDeviceQueue && o_vkGetDeviceProcAddr) {
     o_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)o_vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
  }
  if (o_vkGetDeviceQueue) o_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
  if (pQueue && *pQueue) {
    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    
    // Usage Tracking
    if (g_Device == device && queueFamilyIndex < g_QueueFamilies.size()) {
       if (queueIndex + 1 > g_QueueFamilies[queueFamilyIndex].retrievedCount) {
          g_QueueFamilies[queueFamilyIndex].retrievedCount = queueIndex + 1;
       }
    }

    VkQueueFlags flagsForQueue = 0;
    if (g_Device == device && queueFamilyIndex < g_QueueFamilies.size()) {
        flagsForQueue = g_QueueFamilies[queueFamilyIndex].flags;
    }

    if (flagsForQueue & VK_QUEUE_GRAPHICS_BIT) {
        TrackGraphicsQueue(*pQueue);
    }

    {
        std::lock_guard<std::mutex> qlock(g_QueueInstancesMutex);
        VkQueueFlags flags = flagsForQueue;
        
        // Check if already registered
        bool alreadyRegistered = false;
        for (const auto& qi : g_QueueInstances) {
            if (qi.queue == *pQueue) {
                alreadyRegistered = true;
                break;
            }
        }
        
        if (!alreadyRegistered) {
            g_QueueInstances.push_back({*pQueue, queueFamilyIndex, queueIndex, flags});
            
            const char* typeStr = "Unknown";
            if (flags & VK_QUEUE_GRAPHICS_BIT) typeStr = "Graphics";
            else if (flags & VK_QUEUE_COMPUTE_BIT) typeStr = "Compute";
            else if (flags & VK_QUEUE_TRANSFER_BIT) typeStr = "Transfer";
            
            HookLog("Vulkan: Queue %p registered (Family %d, Index %d, Type: %s, Flags: 0x%X)", 
                    *pQueue, queueFamilyIndex, queueIndex, typeStr, flags);
        }
    }

    // Only capture the first queue (typically graphics+present capable)
    if (g_Queue == VK_NULL_HANDLE) {
      g_Queue = *pQueue;
      g_QueueFamily = queueFamilyIndex;
      HookLog("Vulkan: Using Queue: %p (Family: %d) for overlay", g_Queue,
              g_QueueFamily);
    }
  }
}

void VKAPI_CALL Detour_vkGetDeviceQueue2(VkDevice device,
                                         const VkDeviceQueueInfo2 *pQueueInfo,
                                         VkQueue *pQueue) {
  if (g_InVulkanHook) {
    if (o_vkGetDeviceQueue2) o_vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    return;
  }
  if (!o_vkGetDeviceQueue2 && o_vkGetDeviceProcAddr) {
     o_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)o_vkGetDeviceProcAddr(device, "vkGetDeviceQueue2");
  }
  if (o_vkGetDeviceQueue2) o_vkGetDeviceQueue2(device, pQueueInfo, pQueue);
  if (pQueue && *pQueue) {
    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    
    // Usage Tracking
    if (g_Device == device && pQueueInfo->queueFamilyIndex < g_QueueFamilies.size()) {
       if (pQueueInfo->queueIndex + 1 > g_QueueFamilies[pQueueInfo->queueFamilyIndex].retrievedCount) {
          g_QueueFamilies[pQueueInfo->queueFamilyIndex].retrievedCount = pQueueInfo->queueIndex + 1;
       }
    }

    VkQueueFlags flagsForQueue = 0;
    if (g_Device == device && pQueueInfo->queueFamilyIndex < g_QueueFamilies.size()) {
        flagsForQueue = g_QueueFamilies[pQueueInfo->queueFamilyIndex].flags;
    }

    if (flagsForQueue & VK_QUEUE_GRAPHICS_BIT) {
        TrackGraphicsQueue(*pQueue);
    }

    {
        std::lock_guard<std::mutex> qlock(g_QueueInstancesMutex);
        VkQueueFlags flags = flagsForQueue;
        bool alreadyRegistered = false;
        for (const auto& qi : g_QueueInstances) {
            if (qi.queue == *pQueue) {
                alreadyRegistered = true;
                break;
            }
        }
        if (!alreadyRegistered) {
            g_QueueInstances.push_back({*pQueue, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, flags});
            const char* typeStr = "Unknown";
            if (flags & VK_QUEUE_GRAPHICS_BIT) typeStr = "Graphics";
            else if (flags & VK_QUEUE_COMPUTE_BIT) typeStr = "Compute";
            else if (flags & VK_QUEUE_TRANSFER_BIT) typeStr = "Transfer";
            HookLog("Vulkan: Queue %p registered (Queue2) (Family %d, Index %d, Type: %s, Flags: 0x%X)",
                    *pQueue, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, typeStr, flags);
        }
    }

    if (g_Queue == VK_NULL_HANDLE) {
      g_Queue = *pQueue;
      g_QueueFamily = pQueueInfo->queueFamilyIndex;
      HookLog("Vulkan: Using Queue (Queue2): %p (Family: %d) for overlay",
              g_Queue, g_QueueFamily);
    }
  }
}

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam) {
  DWORD processId;
  GetWindowThreadProcessId(hwnd, &processId);
  if (processId == GetCurrentProcessId()) {
    if (IsWindowVisible(hwnd)) {
      char title[256];
      GetWindowTextA(hwnd, title, sizeof(title));
      if (strlen(title) > 0) {
        *(HWND *)lParam = hwnd;
        return FALSE;
      }
    }
  }
  return TRUE;
}

VkResult VKAPI_CALL Detour_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount,
    VkImage *pSwapchainImages) {
  if (!o_vkGetSwapchainImagesKHR && o_vkGetDeviceProcAddr) {
      o_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)o_vkGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR");
  }
  if (!o_vkGetSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
  return o_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount,
                                   pSwapchainImages);
}

VkResult VKAPI_CALL Detour_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex) {
  if (!o_vkAcquireNextImageKHR && o_vkGetDeviceProcAddr) {
      o_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)o_vkGetDeviceProcAddr(device, "vkAcquireNextImageKHR");
  }
  if (!o_vkAcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;
  return o_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence,
                                 pImageIndex);
}

PFN_vkVoidFunction VKAPI_CALL Detour_vkGetDeviceProcAddr(VkDevice device,
                                                        const char *pName);

void VKAPI_CALL
Detour_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                             const VkAllocationCallbacks *pAllocator) {
  {
    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    auto it = std::remove_if(g_Swapchains.begin(), g_Swapchains.end(),
                             [&](const VulkanSwapchain &sc) {
                               if (sc.swapchain == swapchain) {
                                 const_cast<VulkanSwapchain &>(sc).Cleanup();
                                 return true;
                               }
                               return false;
                             });
    g_Swapchains.erase(it, g_Swapchains.end());
  }
  if (!o_vkDestroySwapchainKHR && o_vkGetDeviceProcAddr) {
      o_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)o_vkGetDeviceProcAddr(device, "vkDestroySwapchainKHR");
  }
  if (o_vkDestroySwapchainKHR) o_vkDestroySwapchainKHR(device, swapchain, pAllocator);
}

static void CreateFramebuffers(VulkanSwapchain &sc) {
  sc.framebuffers.resize(sc.images.size());
  sc.imageViews.resize(sc.images.size());
  for (size_t i = 0; i < sc.images.size(); i++) {
    // We need an ImageView for each image
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = sc.images[i];
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = sc.format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    vkCreateImageView(sc.device, &view_info, nullptr, &sc.imageViews[i]);

    VkFramebufferCreateInfo fb_info = {};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = g_RenderPass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &sc.imageViews[i];
    fb_info.width = sc.width;
    fb_info.height = sc.height;
    fb_info.layers = 1;
    vkCreateFramebuffer(sc.device, &fb_info, nullptr, &sc.framebuffers[i]);
  }
}


static void TryDiscoverAsyncQueue() {
    // Legacy discovery (unused if Separate Device works)
    if (g_AsyncQueue != VK_NULL_HANDLE || g_QueueFamilies.empty()) 
        return;

    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    if (g_AsyncQueue != VK_NULL_HANDLE) return;
    // ... (rest of logic unchanged for fallback) ...

    uint32_t bestFam = UINT32_MAX;
    int bestScore = -1;

    HookLog("Vulkan: Queue Discovery Analysis (%zu families):", g_QueueFamilies.size());
    for (uint32_t i = 0; i < (uint32_t)g_QueueFamilies.size(); i++) {
        auto& info = g_QueueFamilies[i];
        
        const char* typeStr = "Unknown";
        bool hasGraphics = (info.flags & VK_QUEUE_GRAPHICS_BIT);
        bool hasCompute = (info.flags & VK_QUEUE_COMPUTE_BIT);
        bool hasTransfer = (info.flags & VK_QUEUE_TRANSFER_BIT);
        
        if (hasGraphics) typeStr = "Graphics";
        else if (hasCompute) typeStr = "Compute";
        else if (hasTransfer) typeStr = "Transfer";
        
        HookLog("  Family %d: %s | Requested: %d, Retrieved: %d, HW Total: %d",
                i, typeStr, info.requestedCount, info.retrievedCount, info.totalAvailable);
        
        // BUG FIX: Check against REQUESTED count, not hardware total.
        // The driver only creates the queues the game requested at vkCreateDevice.
        // If game requested 1 queue from a family with 2 available, we can use index 1
        // ONLY if retrievedCount < requestedCount (meaning game requested but didn't fetch yet)
        // OR if requestedCount < totalAvailable AND we can actually get the extra queue.
        //
        // Key insight: vkGetDeviceQueue for an index >= requestedCount will fail silently 
        // (return VK_NULL_HANDLE). So we can only use queues the game actually requested.
        //
        // NEW STRATEGY: Check if game requested queues but didn't retrieve all of them.
        if (info.requestedCount > 0 && info.retrievedCount < info.requestedCount) {
            int score = -1;
            
            if (hasTransfer && !hasGraphics && !hasCompute) score = 100;
            else if (hasCompute && !hasGraphics) score = 50;
            else if (hasGraphics) score = 10;
            
            if (score > bestScore) {
                bestScore = score;
                bestFam = i;
            }
            HookLog("    -> Spare queue available! (Score: %d)", score);
        }
    }
    
    if (bestFam != UINT32_MAX) {
        uint32_t queueIndex = g_QueueFamilies[bestFam].retrievedCount;
        VkQueue spareQueue = VK_NULL_HANDLE;
        
        PFN_vkGetDeviceQueue pGetDeviceQueue = o_vkGetDeviceQueue;
        if (!pGetDeviceQueue && g_Device != VK_NULL_HANDLE) {
             if (o_vkGetDeviceProcAddr)
                 pGetDeviceQueue = (PFN_vkGetDeviceQueue)o_vkGetDeviceProcAddr(g_Device, "vkGetDeviceQueue");
        }
        
        if (pGetDeviceQueue && g_Device != VK_NULL_HANDLE) {
            pGetDeviceQueue(g_Device, bestFam, queueIndex, &spareQueue);
        }
        
        if (spareQueue != VK_NULL_HANDLE) {
            g_AsyncQueue = spareQueue;
            g_AsyncQueueFamily = bestFam;
            g_AsyncQueueIndex = queueIndex;
            g_QueueFamilies[bestFam].retrievedCount++;
            
            HookLog("Vulkan: Lazy Discovery found spare queue: Family %d, Index %d (Handle %p)", 
                    bestFam, queueIndex, spareQueue);
        } else {
            HookLog("Vulkan: vkGetDeviceQueue returned NULL for Family %d Index %d", bestFam, queueIndex);
        }
    } else {
        HookLog("Vulkan: No spare queues found - all requested queues are in use by game.");
    }
}


VkResult VKAPI_CALL Detour_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {

  // Try to find a spare async queue if we haven't already
  TryDiscoverAsyncQueue();

  VkSwapchainCreateInfoKHR modifiedCI = *pCreateInfo;

  if (pCreateInfo && pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
    g_FGCompat.OnSwapchainRecreation();
  }

    // VSync Override
    {
        VkSwapchainCreateInfoKHR* pFinalParams = &modifiedCI;
        VkPresentModeKHR presentMode = pFinalParams->presentMode; // Start with original
        
        std::string mode = GetActiveGraphicsConfig().vsyncMode;
        if (mode != "default") {
            if (mode == "off") presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (mode == "fifo") presentMode = VK_PRESENT_MODE_FIFO_KHR;
            else if (mode == "adaptive") presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR; // Or MAILBOX_KHR if strict adaptive is not avail
            else if (mode == "mailbox") presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        
        if (presentMode != pFinalParams->presentMode) {
            pFinalParams->presentMode = presentMode;
            HookLog("Vulkan: CreateSwapchainKHR: Overriding VSync to %s (mode %d)", mode.c_str(), presentMode);
        }

        // Backbuffer Count
        int count = GetActiveGraphicsConfig().backbufferCount;
        if (count >= 2 && count != pFinalParams->minImageCount) {
             pFinalParams->minImageCount = (uint32_t)count;
             HookLog("Vulkan: CreateSwapchainKHR: Overriding minImageCount to %d", count);
        }
    }

  if (!o_vkCreateSwapchainKHR && o_vkGetDeviceProcAddr) {
    o_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)o_vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR");
  }
  if (!o_vkCreateSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult res =
      o_vkCreateSwapchainKHR(device, &modifiedCI, pAllocator, pSwapchain);
  if (res == VK_SUCCESS) {
    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
    VulkanSwapchain sc;
    sc.swapchain = *pSwapchain;
    sc.device = device;
    sc.width = pCreateInfo->imageExtent.width;
    sc.height = pCreateInfo->imageExtent.height;
    sc.format = pCreateInfo->imageFormat;
    sc.sharingMode = modifiedCI.imageSharingMode;
    EarlyLog("Vulkan: Swapchain created (%ux%u, format=%d)", sc.width, sc.height, sc.format);

    uint32_t count;
    if (!o_vkGetSwapchainImagesKHR && o_vkGetDeviceProcAddr) {
        o_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)o_vkGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR");
    }
    if (o_vkGetSwapchainImagesKHR) {
        o_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        sc.images.resize(count);
        o_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, sc.images.data());
    } else {
        HookLog("Vulkan: ERROR - vkGetSwapchainImagesKHR is missing!");
    }

    if (!g_RenderPass) {
      // Redefine render pass with correct format
      VkAttachmentDescription attachment = {};
      attachment.format = sc.format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      // Image arrives in COLOR_ATTACHMENT_OPTIMAL (from our pre-render barrier)
      // and leaves in COLOR_ATTACHMENT_OPTIMAL (our post-render barrier handles -> PRESENT_SRC)
      attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

      VkAttachmentReference color_attachment = {};
      color_attachment.attachment = 0;
      color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

      VkSubpassDescription subpass = {};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color_attachment;

      // Add subpass dependencies for proper synchronization
      VkSubpassDependency dependency = {};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.dstSubpass = 0;
      dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.srcAccessMask = 0;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

      VkRenderPassCreateInfo rp_info = {};
      rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      rp_info.attachmentCount = 1;
      rp_info.pAttachments = &attachment;
      rp_info.subpassCount = 1;
      rp_info.pSubpasses = &subpass;
      rp_info.dependencyCount = 1;
      rp_info.pDependencies = &dependency;

      // Use local device parameter, not global g_Device
      VkResult rpRes =
          vkCreateRenderPass(device, &rp_info, nullptr, &g_RenderPass);
      HookLog("Vulkan: RenderPass creation result: %d (device=%p, g_Device=%p)",
              rpRes, device, g_Device);

      // Store device if not already stored
      if (g_Device == VK_NULL_HANDLE) {
        g_Device = device;
      }
    }

    CreateFramebuffers(sc);
    g_Swapchains.push_back(sc);
    HookLog("Vulkan: Swapchain Created (%dx%d), framebuffers=%zu", sc.width,
            sc.height, sc.framebuffers.size());
  }
  return res;
}

static void InitImGuiVulkan(VkQueue queue) {
  if (g_ImGuiInit || g_Device == VK_NULL_HANDLE ||
      g_RenderPass == VK_NULL_HANDLE)
    return;

  if (!g_hWnd) {
    EnumWindows(FindGameWindowProc, (LPARAM)&g_hWnd);
    if (g_hWnd) {
      HookLog("Vulkan: Fallback Found HWND: %p", g_hWnd);
    } else {
      return; // Still no window
    }
  }

  HookLog("Vulkan: Initializing ImGui...");

  // Ensure volk has device-level function pointers.
  // With VK_NO_PROTOTYPES builds, ImGui Vulkan backend relies on vk* symbols
  // being populated by volk.
  volkLoadDevice(g_Device);

  // Descriptor Pool
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};

  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
  pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
  pool_info.pPoolSizes = pool_sizes;
  vkCreateDescriptorPool(g_Device, &pool_info, nullptr, &g_DescriptorPool);

  g_SharedOverlay.InitImGui(g_hWnd);

  ImGui_ImplVulkan_InitInfo init_info = {};
  init_info.Instance = g_Instance;
  init_info.PhysicalDevice = g_PhysDevice;
  init_info.Device = g_Device;
  init_info.QueueFamily = g_QueueFamily;
  init_info.Queue = queue;
  init_info.PipelineCache = VK_NULL_HANDLE;
  init_info.DescriptorPool = g_DescriptorPool;
  init_info.Subpass = 0;
  init_info.MinImageCount = 2;
  init_info.ImageCount = 3;
  init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  init_info.Allocator = nullptr;
  init_info.CheckVkResultFn = nullptr;
  init_info.RenderPass = g_RenderPass;

  // Load functions for VK_NO_PROTOTYPES builds to avoid assertion/crash.
  // imgui_impl_vulkan.cpp expects g_FunctionsLoaded=true.
  ImGui_ImplVulkan_LoadFunctions(
      [](const char* name, void*) -> PFN_vkVoidFunction {
        if (g_Device == VK_NULL_HANDLE) return nullptr;
        if (o_vkGetDeviceProcAddr) return o_vkGetDeviceProcAddr(g_Device, name);
        return vkGetDeviceProcAddr(g_Device, name);
      },
      nullptr);

  ImGui_ImplVulkan_Init(&init_info);
  ImGui_ImplVulkan_CreateFontsTexture();

  g_ImGuiInit = true;
  HookLog("Vulkan: ImGui Initialized Successfully");
}

VkResult VKAPI_CALL
Detour_vkQueueSubmit(VkQueue queue, uint32_t submitCount,
                     const VkSubmitInfo *pSubmits, VkFence fence) {
  static int submitTotal = 0;
  if (++submitTotal % 100 == 0) {
      HookLog("Vulkan: Detour_vkQueueSubmit (Total %d, queue=%p)", submitTotal, queue);
  }

  if (g_InVulkanHook) {
      if (o_vkQueueSubmit) return o_vkQueueSubmit(queue, submitCount, pSubmits, fence);
      return VK_SUCCESS;
  }
  VulkanHookGuard guard;

  if (!o_vkQueueSubmit && o_vkGetDeviceProcAddr) {
      // We don't have a device handle here easily, but we can try to use a global one if we have it
      if (g_Device != VK_NULL_HANDLE) {
          o_vkQueueSubmit = (PFN_vkQueueSubmit)o_vkGetDeviceProcAddr(g_Device, "vkQueueSubmit");
      }
  }

  bool countThisSubmit = IsTrackedGraphicsQueue(queue);
  if (!countThisSubmit) {
      int knownGfxQueues = g_TrackedGraphicsQueueCount.load(std::memory_order_relaxed);
      if (knownGfxQueues == 0 && (g_Queue == VK_NULL_HANDLE || queue == g_Queue)) {
          countThisSubmit = true;
      }
  }
  if (countThisSubmit) {
      g_QueueSubmitsThisFrame.fetch_add((int)submitCount, std::memory_order_relaxed);
  }
  
  if (!o_vkQueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
  return o_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

VkResult VKAPI_CALL
Detour_vkQueueSubmit2(VkQueue queue, uint32_t submitCount,
                      const VkSubmitInfo2 *pSubmits, VkFence fence) {
  static int submit2Total = 0;
  if (++submit2Total % 100 == 0) {
      HookLog("Vulkan: Detour_vkQueueSubmit2 (Total %d, queue=%p)", submit2Total, queue);
  }

  if (g_InVulkanHook) {
      if (o_vkQueueSubmit2) return o_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
      return VK_SUCCESS;
  }
  VulkanHookGuard guard;

  if (!o_vkQueueSubmit2 && o_vkGetDeviceProcAddr && g_Device != VK_NULL_HANDLE) {
      o_vkQueueSubmit2 = (PFN_vkQueueSubmit2)o_vkGetDeviceProcAddr(g_Device, "vkQueueSubmit2");
  }

  bool countThisSubmit = IsTrackedGraphicsQueue(queue);
  if (!countThisSubmit) {
      int knownGfxQueues = g_TrackedGraphicsQueueCount.load(std::memory_order_relaxed);
      if (knownGfxQueues == 0 && (g_Queue == VK_NULL_HANDLE || queue == g_Queue)) {
          countThisSubmit = true;
      }
  }
  if (countThisSubmit) {
      g_QueueSubmitsThisFrame.fetch_add((int)submitCount, std::memory_order_relaxed);
  }

  if (!o_vkQueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
  return o_vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}

VkResult VKAPI_CALL
Detour_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount,
                         const VkSubmitInfo2 *pSubmits, VkFence fence) {
  static int submit2KHRTotal = 0;
  if (++submit2KHRTotal % 100 == 0) {
      HookLog("Vulkan: Detour_vkQueueSubmit2KHR (Total %d, queue=%p)", submit2KHRTotal, queue);
  }

  if (g_InVulkanHook) {
      if (o_vkQueueSubmit2KHR) return o_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
      return VK_SUCCESS;
  }
  VulkanHookGuard guard;

  if (!o_vkQueueSubmit2KHR && o_vkGetDeviceProcAddr && g_Device != VK_NULL_HANDLE) {
      o_vkQueueSubmit2KHR = (PFN_vkQueueSubmit2)o_vkGetDeviceProcAddr(g_Device, "vkQueueSubmit2KHR");
  }

  bool countThisSubmit = IsTrackedGraphicsQueue(queue);
  if (!countThisSubmit) {
      int knownGfxQueues = g_TrackedGraphicsQueueCount.load(std::memory_order_relaxed);
      if (knownGfxQueues == 0 && (g_Queue == VK_NULL_HANDLE || queue == g_Queue)) {
          countThisSubmit = true;
      }
  }
  if (countThisSubmit) {
      g_QueueSubmitsThisFrame.fetch_add((int)submitCount, std::memory_order_relaxed);
  }

  if (!o_vkQueueSubmit2KHR) return VK_ERROR_INITIALIZATION_FAILED;
  return o_vkQueueSubmit2KHR(queue, submitCount, pSubmits, fence);
}

VkResult VKAPI_CALL
Detour_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
  static int presentCount = 0;
  if (++presentCount % 60 == 0) { // Log every 60 frames to avoid spamming
      HookLog("Vulkan: Detour_vkQueuePresentKHR (Frame %d, queue=%p)", presentCount, queue);
  }

  if (g_InVulkanHook) {
      if (o_vkQueuePresentKHR) return o_vkQueuePresentKHR(queue, pPresentInfo);
      return VK_SUCCESS;
  }
  VulkanHookGuard guard;

  if (!o_vkQueuePresentKHR && o_vkGetDeviceProcAddr && g_Device != VK_NULL_HANDLE) {
      o_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)o_vkGetDeviceProcAddr(g_Device, "vkQueuePresentKHR");
  }

  // FG: Record frame for behavioral detection
  int submitCount = g_QueueSubmitsThisFrame.exchange(0);
  bool isRealFrame = (submitCount > 0);
  g_FGCompat.RecordFrame(submitCount);

  bool fgRuntimeDetected = (g_FGCompat.DetectLoadedFGRuntime() != FGCompatibility::FGType::None);
  bool fgSuspended = fgRuntimeDetected && g_FGCompat.IsSuspended();
  bool allowOverlayFrame = (!fgRuntimeDetected || isRealFrame) && !fgSuspended;

  // Async Present Detection - Check if present queue is compute-only
  static bool g_AsyncPresentDetected = false;
  static VkQueue g_GraphicsQueueForOverlay = VK_NULL_HANDLE;
  static uint32_t g_PresentQueueFamily = UINT32_MAX;
  static uint32_t g_OverlayQueueFamily = UINT32_MAX;
  
  if (!g_AsyncPresentDetected) {
    if (IsComputeOnlyQueue(queue)) {
      g_AsyncPresentDetected = true;
      g_PresentQueueFamily = GetQueueFamilyIndex(queue);
      g_GraphicsQueueForOverlay = FindGraphicsQueue();
      
      if (g_GraphicsQueueForOverlay != VK_NULL_HANDLE) {
        g_OverlayQueueFamily = GetQueueFamilyIndex(g_GraphicsQueueForOverlay);
      }
      
      HookLog("Vulkan: *** ASYNC PRESENT DETECTED ***");
      HookLog("Vulkan: Present queue %p is COMPUTE-ONLY (Family %d)", queue, g_PresentQueueFamily);
      
      if (g_GraphicsQueueForOverlay) {
        HookLog("Vulkan: Found graphics queue %p (Family %d) for overlay rendering", 
                g_GraphicsQueueForOverlay, g_OverlayQueueFamily);

        if (g_GraphicsQueueForOverlay != VK_NULL_HANDLE && g_OverlayQueueFamily != UINT32_MAX) {
          g_Queue = g_GraphicsQueueForOverlay;
          g_QueueFamily = g_OverlayQueueFamily;
          TrackGraphicsQueue(g_GraphicsQueueForOverlay);
        }
        
        if (g_PresentQueueFamily != g_OverlayQueueFamily) {
          HookLog("Vulkan: WARNING - Present and overlay queues are in different families");
          HookLog("Vulkan: Queue family ownership transfers may be needed for correctness");
        }
      } else {
        HookLog("Vulkan: WARNING - No graphics queue found! Overlay will be disabled.");
      }
    }
  }

  // Late injection recovery: capture queue if we don't have it
  if (g_Queue == VK_NULL_HANDLE && queue != VK_NULL_HANDLE) {
    g_Queue = queue;
    HookLog("Vulkan: Late injection - recovered queue from Present");
  }
  
  // Late injection: register swapchain if not seen before
  static bool lateInjectionSwapchainWarned = false;
  if (pPresentInfo && pPresentInfo->swapchainCount > 0) {
    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    bool found = false;
    for (auto& sc : g_Swapchains) {
      if (sc.swapchain == swapchain) {
        found = true;
        break;
      }
    }
    if (!found && !lateInjectionSwapchainWarned) {
      lateInjectionSwapchainWarned = true;
      HookLog("Vulkan: Late injection detected - swapchain %p not registered", swapchain);
      HookLog("Vulkan: Overlay unavailable (game's swapchain created before injection)");
      HookLog("Vulkan: Resize or restart game to enable overlay, OR use DX12 mode if available");
      // Note: We cannot render to a swapchain created by a different Vulkan device.
      // Our recovered device is separate from the game's device, so we can't access
      // the game's swapchain images or create compatible render passes.
    }
  }
  
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

  // Enable CSV logging for frame times (one-time init, only if debug logging enabled)
  static bool csvLoggingInitialized = false;
  SharedMemoryLayout* csvShm = (g_IPC) ? g_IPC->GetSharedMem() : nullptr;
  TryEnableFrameTimeCSVLogging(csvShm, (const void*)&Detour_vkQueuePresentKHR, g_PerfMetrics, "Vulkan", csvLoggingInitialized);

  // Track recording state for performance metrics
  static bool lastRecordingState = false;
  bool isRecording = g_IPC && g_IPC->IsRecording();
  if (isRecording != lastRecordingState) {
    g_PerfMetrics.SetRecording(isRecording);
    lastRecordingState = isRecording;
  }

  g_PerfMetrics.Update(us);

  // --- Timing Diagnostics (log slow operations) ---
  static int diagLogCount = 0;
  static int64_t diagLastUs = 0;

  auto diagTime = [&]() -> int64_t {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return (now.QuadPart * 1000000) / qpcFreq;
  };
  int64_t diagT0 = diagTime();
  int64_t diagT1 = diagT0; // Will be set after prerender limit

  // --- Prerender Limit Simulation ---
  if (g_Device != VK_NULL_HANDLE) {
      float limit = GetActivePrerenderLimit();

      if (limit >= 0.0f) {
          static float lastLimit = -2.0f;
          if (fabs(lastLimit - limit) > 0.001f) {
              if (limit < 1.0f) {
                 if (limit == 0.0f) HookLog("Vulkan: Prerender Limit active: 0.0 (Strict Serial)");
                 else HookLog("Vulkan: Prerender Limit active: %.2f (Hybrid Serial, Timeout Scaled)", limit);
              } else {
                 HookLog("Vulkan: Prerender Limit active: %.2f frames (Buffered)", limit);
              }
              lastLimit = limit;
          }

          if (g_PrerenderDevice != g_Device) {
              for (auto f : g_PrerenderFences) vkDestroyFence(g_PrerenderDevice, f, nullptr);
              g_PrerenderFences.clear();
              g_PrerenderDevice = g_Device;
          }

          // Case 0: Strict Serial (Submit & Wait immediately)
          // Guarantees 0 frames in flight, but may cause utilization drops.
          if (limit == 0.0f) {
              if (g_PrerenderFences.empty()) {
                  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                  VkFence f;
                  vkCreateFence(g_Device, &fci, nullptr, &f);
                  g_PrerenderFences.push_back(f);
              }
              
              VkFence fence = g_PrerenderFences[0];
              vkResetFences(g_Device, 1, &fence);
              vkQueueSubmit(queue, 0, nullptr, fence);
              vkWaitForFences(g_Device, 1, &fence, VK_TRUE, 2000000000);
          }
          // Case > 0: Buffered (Ring Buffer) + Optional Pacing
          else {
              // Ensure Ring Buffer supports at least "Limit 1" (Lookback 2) for any non-zero limit.
              // This guarantees queueing (Queue Size 1), allowing GPU overlap.
              // limit=0.5 -> effective 1 -> Lookback 2.
              // limit=1 -> effective 1 -> Lookback 2.
              // limit=2 -> effective 2 -> Lookback 3.
              // Lookback = effective + 1.
              
              bool isFractional = (limit > 0.01f && limit < 1.0f);
              int effectiveLimit = isFractional ? 1 : (int)limit;
              size_t lookback = effectiveLimit + 1; // Wait for Frame (i - lookback)
              
              size_t needed = lookback + 1; // Ring buffer size
              
              if (g_PrerenderFences.size() != needed) {
                   for (auto f : g_PrerenderFences) vkDestroyFence(g_Device, f, nullptr);
                   g_PrerenderFences.clear();
                   
                   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                   fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; 
                   for (size_t i = 0; i < needed; i++) {
                       VkFence f;
                       vkCreateFence(g_Device, &fci, nullptr, &f);
                       g_PrerenderFences.push_back(f);
                   }
                   g_PrerenderFrameIndex = 0;
              }
              
              size_t size = g_PrerenderFences.size();
              
              // Frame Pacing Wait (Lookback)
              // For fractional limits, lookback is 2 (Buffered 1).
              VkFence oldFence = g_PrerenderFences[(g_PrerenderFrameIndex + size - lookback) % size];
              vkWaitForFences(g_Device, 1, &oldFence, VK_TRUE, 2000000000); 
              vkResetFences(g_Device, 1, &oldFence); 
              
              VkFence currentFence = g_PrerenderFences[g_PrerenderFrameIndex % size];
              vkResetFences(g_Device, 1, &currentFence);
              vkQueueSubmit(queue, 0, nullptr, currentFence);
              
              // Strict Serial + Fixed Idle Gap for fractional limits
              if (isFractional) {
                  // effectiveLimit already set to 0 for Strict Serial above
                  
                  // After the wait completes, calculate and apply a fixed idle gap
                  float fps = g_PerfMetrics.GetCurrentFPS();
                  double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;
                  
                  // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
                  int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
                  if (idleGapUs > 0) {
                      if (idleGapUs > 10000) idleGapUs = 10000; // Cap at 10ms
                      PrecisionSleep(idleGapUs);
                  }
              }
              
              g_PrerenderFrameIndex++;
          }
      }
  }

  // Apply FPS limiter AFTER prerender limit to avoid compounding waits
  // (prerender wait + limiter wait would double the frame time)
  g_SharedFpsLimiter.SetIPCClient(g_IPC);
  g_SharedFpsLimiter.Apply();
  diagT1 = diagTime();

  // Late injection recovery: try to recover device/queue if missing
  static bool recoveryAttempted = false;
  static bool logDeviceOnce = false;
  if (!logDeviceOnce) {
    HookLog("Vulkan: Device state - g_Device=%p, g_Queue=%p, g_PhysDevice=%p, recoveryAttempted=%d",
            g_Device, g_Queue, g_PhysDevice, recoveryAttempted);
    logDeviceOnce = true;
  }
  
  if (!recoveryAttempted && (g_Device == VK_NULL_HANDLE || g_PhysDevice == VK_NULL_HANDLE)) {
    recoveryAttempted = true;
    HookLog("Vulkan: Triggering late injection recovery (device=%p, phys=%p)", 
            g_Device, g_PhysDevice);
    TryRecoverPhysicalDevice();
    HookLog("Vulkan: After recovery - g_Device=%p, g_Queue=%p, g_PhysDevice=%p",
            g_Device, g_Queue, g_PhysDevice);
  }

  if (g_Device != VK_NULL_HANDLE && !g_ImGuiInit && g_Queue != VK_NULL_HANDLE) {
    InitImGuiVulkan(g_Queue);
  }

  // Vulkan: Always draw overlay normally (capture_include_overlay=false not supported due to perf impact)
  if (g_ImGuiInit && g_Device != VK_NULL_HANDLE) {
    bool showOverlay = true;
    bool showFPS = true;
    if (g_IPC && g_IPC->GetSharedMem()) {
      showOverlay = g_IPC->GetSharedMem()->overlayConfig.showOverlay;
      showFPS = g_IPC->GetSharedMem()->overlayConfig.showFPS;
    }

    static bool loggedOnce = false;
    if (!loggedOnce) {
      HookLog("Vulkan: Overlay check - showOverlay=%d, showFPS=%d, "
              "g_Swapchains.size=%zu",
              showOverlay, showFPS, g_Swapchains.size());
      loggedOnce = true;
    }

    // DEBUG: Track overlay render decisions
    static int dbgPresentNum = 0;
    static int dbgOverlayDrawn = 0;
    static int dbgOverlaySkipped = 0;
    static int dbgLastLogFrame = 0;
    dbgPresentNum++;
    
    if (showOverlay) {
      // Find swapchain
      bool foundSwapchain = false;
      for (auto &sc : g_Swapchains) {
        if (sc.swapchain == pPresentInfo->pSwapchains[0]) {
          foundSwapchain = true;
          uint32_t idx = pPresentInfo->pImageIndices[0];

          if (idx >= sc.framebuffers.size()) {
            HookLog(
                "Vulkan: ERROR - framebuffer index %u out of range (size %zu)",
                idx, sc.framebuffers.size());
            break;
          }
          if (!sc.framebuffers[idx]) {
            HookLog("Vulkan: ERROR - framebuffer[%u] is NULL", idx);
            dbgOverlaySkipped++;
            break;
          }
          
          // FG: Only render overlay on REAL frames
          // Interpolated frames often crash if we touch them or submit commands to wrong queue state
          if (!allowOverlayFrame) {
              dbgOverlaySkipped++;
              if (dbgPresentNum - dbgLastLogFrame > 60) {
                HookLog("Vulkan: DEBUG - Overlay SKIPPED (FG/notReal) present#%d isReal=%d fgSuspended=%d",
                        dbgPresentNum, isRealFrame, fgSuspended);
                dbgLastLogFrame = dbgPresentNum;
              }
              break;
          }
          
              ImGui_ImplVulkan_NewFrame();
              g_SharedOverlay.BeginFrame();
    
              // Use shared overlay
              g_SharedOverlay.SetMetrics(&g_PerfMetrics);
              g_SharedOverlay.SetIPCClient(g_IPC);
              g_SharedOverlay.SetDroppedFrames(g_VulkanCapture.droppedFrames.load(std::memory_order_relaxed));
              g_SharedOverlay.SetGraphicsAPI("Vulkan");
              // Detect HDR
              bool isHDR = (sc.format == VK_FORMAT_R16G16B16A16_SFLOAT || 
                           sc.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
              g_SharedOverlay.SetHDR(isHDR);

              g_SharedOverlay.RenderUI();
    
              g_SharedOverlay.EndFrame();
    
              // Draw into Command Buffer using in-flight frame ring buffer
              // to avoid blocking the game thread
              static constexpr int MAX_FRAMES_IN_FLIGHT = 5; // Use 5 frames for smoother overlay at high GPU load
              static VkFence inFlightFences[MAX_FRAMES_IN_FLIGHT] = {};
              static VkCommandBuffer inFlightCBs[MAX_FRAMES_IN_FLIGHT] = {};
              static VkSemaphore inFlightSemaphores[MAX_FRAMES_IN_FLIGHT] = {};
              static int currentFrame = 0;
              static bool fencesCreated = false;
              static uint32_t commandPoolQueueFamily = UINT32_MAX;
              struct RetiredOverlayPool {
                VkCommandPool pool;
                VkFence fences[MAX_FRAMES_IN_FLIGHT];
                VkSemaphore semaphores[MAX_FRAMES_IN_FLIGHT];
                bool valid;
              };
              static RetiredOverlayPool retiredPools[4] = {};

              for (int r = 0; r < 4; r++) {
                if (!retiredPools[r].valid) continue;
                bool allDone = true;
                for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                  VkFence rf = retiredPools[r].fences[i];
                  if (!rf) continue;
                  VkResult st = vkWaitForFences(g_Device, 1, &rf, VK_TRUE, 0);
                  if (st == VK_TIMEOUT) {
                    allDone = false;
                    break;
                  }
                }
                if (allDone) {
                  for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                    if (retiredPools[r].fences[i]) {
                      vkDestroyFence(g_Device, retiredPools[r].fences[i], nullptr);
                      retiredPools[r].fences[i] = VK_NULL_HANDLE;
                    }
                    if (retiredPools[r].semaphores[i]) {
                      vkDestroySemaphore(g_Device, retiredPools[r].semaphores[i], nullptr);
                      retiredPools[r].semaphores[i] = VK_NULL_HANDLE;
                    }
                  }
                  if (retiredPools[r].pool) {
                    vkDestroyCommandPool(g_Device, retiredPools[r].pool, nullptr);
                    retiredPools[r].pool = VK_NULL_HANDLE;
                  }
                  retiredPools[r].valid = false;
                }
              }

              if (commandPoolQueueFamily != UINT32_MAX && commandPoolQueueFamily != g_QueueFamily) {
                if (g_CommandPool != VK_NULL_HANDLE) {
                  int freeSlot = -1;
                  for (int r = 0; r < 4; r++) {
                    if (!retiredPools[r].valid) {
                      freeSlot = r;
                      break;
                    }
                  }
                  if (freeSlot < 0) {
                    break;
                  }

                  retiredPools[freeSlot].pool = g_CommandPool;
                  for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                    retiredPools[freeSlot].fences[i] = inFlightFences[i];
                    retiredPools[freeSlot].semaphores[i] = inFlightSemaphores[i];
                    inFlightFences[i] = VK_NULL_HANDLE;
                    inFlightSemaphores[i] = VK_NULL_HANDLE;
                    inFlightCBs[i] = VK_NULL_HANDLE;
                  }
                  retiredPools[freeSlot].valid = true;
                }

                g_CommandPool = VK_NULL_HANDLE;
                fencesCreated = false;
                currentFrame = 0;
                commandPoolQueueFamily = UINT32_MAX;
              }

              if (!g_CommandPool) {
                VkCommandPoolCreateInfo pool_info = {
                    VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
                pool_info.queueFamilyIndex = g_QueueFamily;
                pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                vkCreateCommandPool(g_Device, &pool_info, nullptr, &g_CommandPool);
                if (g_CommandPool != VK_NULL_HANDLE) {
                  commandPoolQueueFamily = g_QueueFamily;
                }
              }

              // Create fences and command buffers once
              if (!fencesCreated) {
                VkCommandBufferAllocateInfo alloc_info = {
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                alloc_info.commandPool = g_CommandPool;
                alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
                vkAllocateCommandBuffers(g_Device, &alloc_info, inFlightCBs);
    
                VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // Start signaled
                for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                  vkCreateFence(g_Device, &fenceInfo, nullptr, &inFlightFences[i]);
                }
                VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                  vkCreateSemaphore(g_Device, &semInfo, nullptr, &inFlightSemaphores[i]);
                }
                fencesCreated = true;
              }
    
              // Wait for this frame's previous submission to complete
              // Use short timeout (2ms) to prevent overlay flashing at high GPU load
              // while still not significantly impacting frame pacing
              VkFence fence = VK_NULL_HANDLE;
              VkCommandBuffer cb = VK_NULL_HANDLE;

              bool foundFreeSlot = false;
              for (int attempt = 0; attempt < MAX_FRAMES_IN_FLIGHT; attempt++) {
                fence = inFlightFences[currentFrame];
                cb = inFlightCBs[currentFrame];
                VkResult fenceResult =
                    vkWaitForFences(g_Device, 1, &fence, VK_TRUE, 0); // Non-blocking for zero latency
                if (fenceResult != VK_TIMEOUT) {
                  foundFreeSlot = true;
                  break;
                }
                currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
              }
              if (!foundFreeSlot) {
                // Previous overlay work not done - skip this frame
                dbgOverlaySkipped++;
                if (dbgPresentNum - dbgLastLogFrame > 60) {
                  HookLog("Vulkan: DEBUG - Overlay SKIPPED (no free fence slot) frame=%d drawn=%d skipped=%d", 
                          dbgPresentNum, dbgOverlayDrawn, dbgOverlaySkipped);
                  dbgLastLogFrame = dbgPresentNum;
                }
                currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
                break;
              }
              vkResetFences(g_Device, 1, &fence);
              vkResetCommandBuffer(cb, 0);
    
              VkCommandBufferBeginInfo begin_info = {
                  VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
              begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
              vkBeginCommandBuffer(cb, &begin_info);
    
              VkRenderPassBeginInfo rp_begin = {};
              rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
              rp_begin.renderPass = g_RenderPass;
              rp_begin.framebuffer = sc.framebuffers[idx];
              rp_begin.renderArea.offset = {0, 0};
              rp_begin.renderArea.extent = {sc.width, sc.height};

              // CRITICAL: Wait for ALL prior rendering to complete before drawing overlay.
              // The game may not provide wait semaphores (waitSems=0), so we must ensure
              // synchronization via pipeline barriers with proper stage/access masks.
              VkImageMemoryBarrier toColor = {};
              toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              // srcAccessMask: Wait for prior color writes to be visible
              toColor.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
              toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
              toColor.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
              toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
              toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              toColor.image = sc.images[idx];
              toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
              toColor.subresourceRange.baseMipLevel = 0;
              toColor.subresourceRange.levelCount = 1;
              toColor.subresourceRange.baseArrayLayer = 0;
              toColor.subresourceRange.layerCount = 1;

              // srcStageMask: Wait for ALL prior commands (game's rendering) to complete
              // This is essential when the game provides no wait semaphores
              vkCmdPipelineBarrier(cb,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   0,
                                   0, nullptr,
                                   0, nullptr,
                                   1, &toColor);
              vkCmdBeginRenderPass(cb, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
    
              // Set viewport and scissor for the overlay rendering
              VkViewport viewport = {};
              viewport.x = 0.0f;
              viewport.y = 0.0f;
              viewport.width = (float)sc.width;
              viewport.height = (float)sc.height;
              viewport.minDepth = 0.0f;
              viewport.maxDepth = 1.0f;
              vkCmdSetViewport(cb, 0, 1, &viewport);
    
              VkRect2D scissor = {};
              scissor.offset = {0, 0};
              scissor.extent = {sc.width, sc.height};
              vkCmdSetScissor(cb, 0, 1, &scissor);
    
              ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    
              vkCmdEndRenderPass(cb);

              VkImageMemoryBarrier toPresent = {};
              toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
              toPresent.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
              toPresent.dstAccessMask = 0;
              toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
              toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
              toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              toPresent.image = sc.images[idx];
              toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
              toPresent.subresourceRange.baseMipLevel = 0;
              toPresent.subresourceRange.levelCount = 1;
              toPresent.subresourceRange.baseArrayLayer = 0;
              toPresent.subresourceRange.layerCount = 1;

              vkCmdPipelineBarrier(cb,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                   0,
                                   0, nullptr,
                                   0, nullptr,
                                   1, &toPresent);
              vkEndCommandBuffer(cb);
    
              VkSemaphore overlayDoneSem = inFlightSemaphores[currentFrame];

              VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
              submit.commandBufferCount = 1;
              submit.pCommandBuffers = &cb;
              submit.signalSemaphoreCount = 1;
              submit.pSignalSemaphores = &overlayDoneSem;

              // Wait on the app's present wait semaphores (GPU-side dependency only).
              // This prevents racing the game's rendering and reduces overlay flicker.
              std::vector<VkPipelineStageFlags> overlayWaitStages;
              if (pPresentInfo && pPresentInfo->waitSemaphoreCount > 0 && pPresentInfo->pWaitSemaphores) {
                overlayWaitStages.resize(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                submit.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                submit.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                submit.pWaitDstStageMask = overlayWaitStages.data();
              }
    
              // Choose queue for overlay submission
              VkQueue overlayQueue = g_Queue;
              
              // If async present is detected, use graphics queue for overlay
              if (g_AsyncPresentDetected && g_GraphicsQueueForOverlay != VK_NULL_HANDLE) {
                overlayQueue = g_GraphicsQueueForOverlay;
              }
              
              // Skip overlay if async present but no graphics queue available
              if (g_AsyncPresentDetected && g_GraphicsQueueForOverlay == VK_NULL_HANDLE) {
                // Silently skip - already logged warning during detection
                break;
              }
    
              dbgOverlayDrawn++;
              
              // DEBUG: Log every 120 frames to track overlay health
              static int frameCount = 0;
              if (frameCount < 3 || (dbgPresentNum - dbgLastLogFrame > 120)) {
                HookLog("Vulkan: Submitting overlay (present#%d, drawn=%d, skipped=%d, imgIdx=%u, sameQ=%d, waitSems=%u)",
                        dbgPresentNum, dbgOverlayDrawn, dbgOverlaySkipped, idx,
                        (overlayQueue == queue) ? 1 : 0,
                        pPresentInfo->waitSemaphoreCount);
                dbgLastLogFrame = dbgPresentNum;
              }
              frameCount++;
    
              // Submit without blocking - fence will be waited on next time this
              // slot is used. This ensures no performance impact or input lag.
              vkQueueSubmit(overlayQueue, 1, &submit, fence);
    
              // Advance to next frame slot
              currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

              // Synchronization strategy depends on whether overlay and present use the same queue
              // CRITICAL: We already consumed the app's wait semaphores in our overlay submit above.
              // Binary semaphores can only be waited on ONCE per signal.
              // We MUST NOT pass the original pPresentInfo because it still references those semaphores!
              VkPresentInfoKHR modifiedPresent = *pPresentInfo;
              
              if (overlayQueue == queue) {
                // SAME QUEUE: Vulkan guarantees submission order execution.
                // Overlay submit completes before present executes (queue ordering).
                // Set waitSemaphoreCount=0 since we already consumed them.
                modifiedPresent.waitSemaphoreCount = 0;
                modifiedPresent.pWaitSemaphores = nullptr;
                return o_vkQueuePresentKHR(queue, &modifiedPresent);
              } else {
                // DIFFERENT QUEUES (async present): Need our semaphore to synchronize
                modifiedPresent.waitSemaphoreCount = 1;
                modifiedPresent.pWaitSemaphores = &overlayDoneSem;
                return o_vkQueuePresentKHR(queue, &modifiedPresent);
              }
          break;
        }
      }
      if (!foundSwapchain) {
        dbgOverlaySkipped++;
        static bool loggedOnce = false;
        if (!loggedOnce) {
          HookLog("Vulkan: WARNING - No matching swapchain found in present "
                  "(swapchains: %zu)",
                  g_Swapchains.size());
          loggedOnce = true;
        }
      }
    }
  }
  
  int64_t diagT2 = diagTime();
  
  // Log slow frames (>2ms overhead from our code)
  // Exclude limiter time, as that's intentional waiting
  int64_t totalOverheadUs = diagT2 - diagT0;
  int64_t limiterTimeUs = diagT1 - diagT0;
  int64_t internalOverheadUs = totalOverheadUs - limiterTimeUs;
  
  if (internalOverheadUs > 2000 && diagLogCount < 50) {
      EarlyLog("Vulkan SLOW: Total=%lldus, Limiter=%lldus, Overlay=%lldus",
               totalOverheadUs, limiterTimeUs, diagT2 - diagT1);
      diagLogCount++;
  }

  // Video capture BEFORE Present - must copy while we still own the swapchain image
  // Only capture REAL frames
  if (g_IPC && g_IPC->IsRecording() && pPresentInfo->swapchainCount > 0 && isRealFrame) {
    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    uint32_t imageIndex = pPresentInfo->pImageIndices[0];

    for (auto &sc : g_Swapchains) {
      if (sc.swapchain == swapchain && imageIndex < sc.images.size()) {
        if (!g_VulkanCapture.initialized) {
          // Late injection recovery: if we don't have device/physdevice, try to recover
          if (g_Device == VK_NULL_HANDLE && sc.device != VK_NULL_HANDLE) {
            g_Device = sc.device;
            HookLog("Vulkan: Late injection - recovered device from swapchain");
          }
          if (g_PhysDevice == VK_NULL_HANDLE) {
            if (!TryRecoverPhysicalDevice()) {
              HookLog("Vulkan: Late injection recovery failed, capture disabled");
              break;
            }
          }
          
          // uint32_t dxgiFormat = MapVulkanFormatToDXGI((VkFormat)sc.format);
          
          VkPhysicalDeviceIDProperties idProps = {
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
          VkPhysicalDeviceProperties2 props2 = {
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
          props2.pNext = &idProps;
          vkGetPhysicalDeviceProperties2(g_PhysDevice, &props2);
          memcpy(&g_VulkanCapture.luidLow, idProps.deviceLUID, 4);
          memcpy(&g_VulkanCapture.luidHigh, idProps.deviceLUID + 4, 4);

          // Initialize capture resources (uses optimized D3D11 path with async queue)
          g_VulkanCapture.CreateSharedResources(sc.width, sc.height, (uint32_t)sc.format);
          
          if (g_VulkanCapture.initialized) {
              if (g_VulkanCapture.usingD3D11Path) {
                  HookLog("Vulkan: D3D11 path initialized successfully");
              } else {
                  HookLog("Vulkan: Vulkan path initialized successfully");
              }
          } else {
              HookLog("Vulkan: ERROR - Capture initialization failed!");
          }
          g_VulkanCapture.PublishToSharedMemory(g_IPC);
        }

        // --- Async Capture Path ---
        if (g_AsyncQueue != VK_NULL_HANDLE && g_VulkanCapture.initialized) {
            if (!g_VulkanCapture.captureThreadRunning) {
                g_VulkanCapture.StartCaptureThread(AsyncCaptureThreadProc);
            }
            
            // Capture the write index before advancing (if needed)
            int textureIdx = g_VulkanCapture.writeIndex;
            
            // OPTIMIZATION: For D3D11 path, skip game queue copy - let async thread do single-copy
            // This matches DX12 efficiency: swapchain → D3D11 texture in ONE operation on async queue
            if (g_VulkanCapture.usingD3D11Path && !separateDeviceActive) {
                // Create a semaphore to signal when swapchain is ready for async copy
                VkSemaphore asyncReadySem = g_VulkanCapture.copyCompleteSemaphores[textureIdx];
                
                // Submit a minimal command to transition swapchain and signal the semaphore
                // The async thread will wait on this before doing the actual copy
                VkCommandBuffer cmd = g_VulkanCapture.captureCommandBuffers[textureIdx];
                vkResetCommandBuffer(cmd, 0);
                VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &beginInfo);
                // No actual work - just a synchronization point
                vkEndCommandBuffer(cmd);
                
                VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &cmd;
                
                // Wait for app's rendering to complete
                std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_TRANSFER_BIT);
                submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages.data();
                
                // Signal: async thread can proceed AND present can proceed
                VkSemaphore signalSems[] = {asyncReadySem, g_VulkanCapture.presentTriggerSems[textureIdx]};
                submitInfo.signalSemaphoreCount = 2;
                submitInfo.pSignalSemaphores = signalSems;
                
                {
                    std::lock_guard<std::recursive_mutex> lock(g_VulkanMutex);
                    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
                }
                
                // Enqueue for async thread (will do the actual copy)
                g_VulkanCapture.EnqueueFrame(us, 0, textureIdx, 
                                            (void*)sc.images[imageIndex], 
                                            (uint64_t)asyncReadySem);
                g_VulkanCapture.writeIndex = (textureIdx + 1) % CAPTURE_TEXTURE_COUNT;
                
                // Present waits on presentTriggerSem (signals after game queue submit)
                VkPresentInfoKHR newPresent = *pPresentInfo;
                newPresent.waitSemaphoreCount = 1;
                newPresent.pWaitSemaphores = &g_VulkanCapture.presentTriggerSems[textureIdx];
                
                return o_vkQueuePresentKHR(queue, &newPresent);
            }
            
            // --- LEGACY PATH: Copy on game queue (for separateDeviceActive or fallback) ---
            std::vector<VkSemaphore> waitSemaphores;
            for(uint32_t i=0; i<pPresentInfo->waitSemaphoreCount; i++) {
                waitSemaphores.push_back(pPresentInfo->pWaitSemaphores[i]);
            }
            
            VkSemaphore completionSem = g_VulkanCapture.CopyFrame(
                queue, sc.images[imageIndex], sc.width, sc.height, waitSemaphores);

            if (completionSem != VK_NULL_HANDLE) {
                g_VulkanCapture.EnqueueFrame(us, 0, textureIdx, 
                                            (void*)sc.images[imageIndex], 
                                            (uint64_t)completionSem);
                
                // Present waits on completionSem
                VkPresentInfoKHR newPresent = *pPresentInfo;
                newPresent.waitSemaphoreCount = 1;
                newPresent.pWaitSemaphores = &completionSem;
                
                return o_vkQueuePresentKHR(queue, &newPresent);
            }
        }
        
        // --- Fallback Synchronous Path ---
        // Synchronous capture BEFORE present - we still own the image
        // NOTE: This path runs on the game's present queue which may cause stuttering.
        // Async capture (queue injection) is disabled due to stability issues.
        static bool syncPathLogged = false;
        if (!syncPathLogged) {
          HookLog("Vulkan: Using synchronous capture path (may cause stutter in some games)");
          syncPathLogged = true;
        }
        VkImage srcImage = sc.images[imageIndex];
        std::vector<VkSemaphore> waitSemaphores;
        // Collect wait semaphores from PresentInfo
        for(uint32_t i=0; i<pPresentInfo->waitSemaphoreCount; i++) {
            waitSemaphores.push_back(pPresentInfo->pWaitSemaphores[i]);
        }
        
        VkSemaphore completionSem = g_VulkanCapture.CopyFrame(
            queue, srcImage, sc.width, sc.height, waitSemaphores);

        if (completionSem != VK_NULL_HANDLE) {
          int usedTextureIdx = (g_VulkanCapture.writeIndex + CAPTURE_TEXTURE_COUNT - 1) %
                  CAPTURE_TEXTURE_COUNT;
          g_VulkanCapture.SignalFrameReady(
              g_IPC, usedTextureIdx,
              us / 1000, g_VulkanCapture.fenceValue);
          
          // CRITICAL FIX: Make Present wait on Capture completion
          // This serializes Render -> Capture -> Present
          VkPresentInfoKHR newPresent = *pPresentInfo;
          newPresent.waitSemaphoreCount = 1;
          newPresent.pWaitSemaphores = &completionSem; // Wait for copy
          // Note: pPresentInfo->pWaitSemaphores were already waited by CopyFrame
          
          return o_vkQueuePresentKHR(queue, &newPresent);
        }
        break;
      }
    }
  }

  // Call Present after capture is done
  VkResult presentResult = o_vkQueuePresentKHR(queue, pPresentInfo);

  return presentResult;
}

PFN_vkVoidFunction VKAPI_CALL Detour_vkGetInstanceProcAddr(VkInstance instance,
                                                           const char *pName) {
  HookLog("Vulkan: vkGetInstanceProcAddr(%p, %s)", instance, pName ? pName : "NULL");
  PFN_vkVoidFunction res = o_vkGetInstanceProcAddr(instance, pName);
  if (!res || !pName)
    return res;

  if (instance != VK_NULL_HANDLE && g_Instance == VK_NULL_HANDLE) {
      g_Instance = instance;
      HookLog("Vulkan: Recovered instance handle from vkGetInstanceProcAddr");
      volkLoadInstance(g_Instance);
  }

  if (strcmp(pName, "vkCreateInstance") == 0)
    return (PFN_vkVoidFunction)Detour_vkCreateInstance;
  if (strcmp(pName, "vkCreateDevice") == 0)
    return (PFN_vkVoidFunction)Detour_vkCreateDevice;
  if (strcmp(pName, "vkCreateSampler") == 0) {
    o_vkCreateSampler = (PFN_vkCreateSampler)res;
    return (PFN_vkVoidFunction)Detour_vkCreateSampler;
  }
  if (strcmp(pName, "vkUpdateDescriptorSets") == 0) {
    o_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSets;
  }
  if (strcmp(pName, "vkDestroySampler") == 0) {
    o_vkDestroySampler = (PFN_vkDestroySampler)res;
    return (PFN_vkVoidFunction)Detour_vkDestroySampler;
  }
  if (strcmp(pName, "vkCreateDescriptorUpdateTemplate") == 0) {
    o_vkCreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkCreateDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkCreateDescriptorUpdateTemplateKHR") == 0) {
    o_vkCreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkCreateDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkDestroyDescriptorUpdateTemplate") == 0) {
    o_vkDestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkDestroyDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkDestroyDescriptorUpdateTemplateKHR") == 0) {
    o_vkDestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkDestroyDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0) {
    o_vkUpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSetWithTemplate;
  }
  if (strcmp(pName, "vkUpdateDescriptorSetWithTemplateKHR") == 0) {
    o_vkUpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSetWithTemplate;
  }
  if (strcmp(pName, "vkCreateWin32SurfaceKHR") == 0)
    return (PFN_vkVoidFunction)Detour_vkCreateWin32SurfaceKHR;
  if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
    return (PFN_vkVoidFunction)Detour_vkGetInstanceProcAddr;
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)Detour_vkGetDeviceProcAddr;
  
  // Also handle common device/extension functions in InstanceProcAddr 
  // as some games resolve everything through it.
  if (strcmp(pName, "vkCreateSwapchainKHR") == 0)
  {
    o_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)res;
    return (PFN_vkVoidFunction)Detour_vkCreateSwapchainKHR;
  }
  if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0)
  {
    o_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)res;
    return (PFN_vkVoidFunction)Detour_vkGetSwapchainImagesKHR;
  }
  if (strcmp(pName, "vkGetDeviceQueue") == 0)
  {
    o_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)res;
    return (PFN_vkVoidFunction)Detour_vkGetDeviceQueue;
  }
  if (strcmp(pName, "vkAcquireNextImageKHR") == 0)
  {
    o_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)res;
    return (PFN_vkVoidFunction)Detour_vkAcquireNextImageKHR;
  }
  if (strcmp(pName, "vkDestroySwapchainKHR") == 0)
  {
    o_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)res;
    return (PFN_vkVoidFunction)Detour_vkDestroySwapchainKHR;
  }
  if (strcmp(pName, "vkQueuePresentKHR") == 0)
  {
    o_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)res;
    return (PFN_vkVoidFunction)Detour_vkQueuePresentKHR;
  }
  if (strcmp(pName, "vkQueueSubmit") == 0)
  {
    o_vkQueueSubmit = (PFN_vkQueueSubmit)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit;
  }
  if (strcmp(pName, "vkQueueSubmit2") == 0)
  {
    o_vkQueueSubmit2 = (PFN_vkQueueSubmit2)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit2;
  }
  if (strcmp(pName, "vkQueueSubmit2KHR") == 0)
  {
    o_vkQueueSubmit2KHR = (PFN_vkQueueSubmit2)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit2KHR;
  }

  return res;
}

PFN_vkVoidFunction VKAPI_CALL Detour_vkGetDeviceProcAddr(VkDevice device,
                                                         const char *pName) {
  HookLog("Vulkan: vkGetDeviceProcAddr(%p, %s)", device, pName ? pName : "NULL");
  PFN_vkVoidFunction res = o_vkGetDeviceProcAddr(device, pName);
  if (!res || !pName)
    return res;

  if (g_Device == VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
      g_Device = device;
      HookLog("Vulkan: Recovered device handle from vkGetDeviceProcAddr");
      volkLoadDevice(g_Device);
  }

  if (strcmp(pName, "vkGetDeviceQueue") == 0) {
    o_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)res;
    return (PFN_vkVoidFunction)Detour_vkGetDeviceQueue;
  }
  if (strcmp(pName, "vkGetDeviceQueue2") == 0) {
    o_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)res;
    return (PFN_vkVoidFunction)Detour_vkGetDeviceQueue2;
  }
  if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
    o_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)res;
    return (PFN_vkVoidFunction)Detour_vkAcquireNextImageKHR;
  }
  if (strcmp(pName, "vkDestroySwapchainKHR") == 0) {
    o_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)res;
    return (PFN_vkVoidFunction)Detour_vkDestroySwapchainKHR;
  }
  if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
    o_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)res;
    return (PFN_vkVoidFunction)Detour_vkCreateSwapchainKHR;
  }
  if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
    o_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)res;
    return (PFN_vkVoidFunction)Detour_vkGetSwapchainImagesKHR;
  }
  if (strcmp(pName, "vkCreateSampler") == 0) {
    o_vkCreateSampler = (PFN_vkCreateSampler)res;
    return (PFN_vkVoidFunction)Detour_vkCreateSampler;
  }
  if (strcmp(pName, "vkUpdateDescriptorSets") == 0) {
    o_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSets;
  }
  if (strcmp(pName, "vkDestroySampler") == 0) {
    o_vkDestroySampler = (PFN_vkDestroySampler)res;
    return (PFN_vkVoidFunction)Detour_vkDestroySampler;
  }
  if (strcmp(pName, "vkCreateDescriptorUpdateTemplate") == 0) {
    o_vkCreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkCreateDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkCreateDescriptorUpdateTemplateKHR") == 0) {
    o_vkCreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkCreateDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkDestroyDescriptorUpdateTemplate") == 0) {
    o_vkDestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkDestroyDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkDestroyDescriptorUpdateTemplateKHR") == 0) {
    o_vkDestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkDestroyDescriptorUpdateTemplate;
  }
  if (strcmp(pName, "vkUpdateDescriptorSetWithTemplate") == 0) {
    o_vkUpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSetWithTemplate;
  }
  if (strcmp(pName, "vkUpdateDescriptorSetWithTemplateKHR") == 0) {
    o_vkUpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplate)res;
    return (PFN_vkVoidFunction)Detour_vkUpdateDescriptorSetWithTemplate;
  }
  if (strcmp(pName, "vkCreateImage") == 0) {
    o_vkCreateImage = (PFN_vkCreateImage)res;
    return (PFN_vkVoidFunction)Detour_vkCreateImage;
  }
  if (strcmp(pName, "vkCreateRenderPass") == 0) {
    o_vkCreateRenderPass = (PFN_vkCreateRenderPass)res;
    return (PFN_vkVoidFunction)Detour_vkCreateRenderPass;
  }
  if (strcmp(pName, "vkCreateRenderPass2") == 0 || strcmp(pName, "vkCreateRenderPass2KHR") == 0) {
    o_vkCreateRenderPass2 = (PFN_vkCreateRenderPass2)res;
    return (PFN_vkVoidFunction)Detour_vkCreateRenderPass2;
  }
  if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) {
    o_vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)res;
    return (PFN_vkVoidFunction)Detour_vkCreateGraphicsPipelines;
  }
  if (strcmp(pName, "vkQueuePresentKHR") == 0) {
    o_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)res;
    return (PFN_vkVoidFunction)Detour_vkQueuePresentKHR;
  }
  if (strcmp(pName, "vkQueueSubmit") == 0) {
    o_vkQueueSubmit = (PFN_vkQueueSubmit)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit;
  }
  if (strcmp(pName, "vkQueueSubmit2") == 0) {
    o_vkQueueSubmit2 = (PFN_vkQueueSubmit2)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit2;
  }
  if (strcmp(pName, "vkQueueSubmit2KHR") == 0) {
    o_vkQueueSubmit2KHR = (PFN_vkQueueSubmit2)res;
    return (PFN_vkVoidFunction)Detour_vkQueueSubmit2KHR;
  }
  if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)Detour_vkGetDeviceProcAddr;

  return res;
}

void VulkanHook::Init() {
  HookLog("VulkanHook::Init()");

  // Try to apply NVIDIA LOD Bias fix early
  // ApplyNvidiaLodBiasFix removed

  if (volkInitialize() != VK_SUCCESS) {
      HookLog("VulkanHook: Failed to initialize volk. Vulkan not available.");
      return;
  }

  HMODULE hVulkan = GetModuleHandleA("vulkan-1.dll");
  HookLog("VulkanHook: hVulkan=%p", hVulkan);
  if (!hVulkan)
    return;

  auto CreateHook = [&](const char *name, void *detour, void **original) {
    void *addr = (void *)GetProcAddress(hVulkan, name);
    HookLog("Vulkan: Hooking %s at %p", name, addr);
    if (addr) {
      if (MH_CreateHook(addr, detour, original) == MH_OK) {
        MH_EnableHook(addr);
        HookLog("Vulkan Hook: %s enabled", name);
      } else {
        HookLog("Vulkan Hook: %s FAILED (MH_CreateHook)", name);
      }
    } else {
      HookLog("Vulkan Hook: %s FAILED (GetProcAddress)", name);
    }
  };

  // IMPORTANT: Hook as little as possible via vulkan-1.dll exports.
  // Some exports are forwarded/stubs and MinHook patching can crash (observed for vkAcquireNextImageKHR).
  // We rely on vkGetInstanceProcAddr/vkGetDeviceProcAddr to detour most functions.
  CreateHook("vkGetInstanceProcAddr", (void *)&Detour_vkGetInstanceProcAddr,
             (void **)&o_vkGetInstanceProcAddr);
  CreateHook("vkGetDeviceProcAddr", (void *)&Detour_vkGetDeviceProcAddr,
             (void **)&o_vkGetDeviceProcAddr);
  CreateHook("vkCreateInstance", (void *)&Detour_vkCreateInstance,
             (void **)&o_vkCreateInstance);
  CreateHook("vkCreateDevice", (void *)&Detour_vkCreateDevice,
             (void **)&o_vkCreateDevice);
  CreateHook("vkCreateWin32SurfaceKHR", (void *)&Detour_vkCreateWin32SurfaceKHR,
             (void **)&o_vkCreateWin32SurfaceKHR);

  // Fallback: some games call vulkan-1.dll exports directly (no ProcAddr lookup).
  // These exports appear safe to hook and are required for overlay/capture to trigger.
  CreateHook("vkQueuePresentKHR", (void *)&Detour_vkQueuePresentKHR,
             (void **)&o_vkQueuePresentKHR);
  CreateHook("vkQueueSubmit", (void *)&Detour_vkQueueSubmit,
             (void **)&o_vkQueueSubmit);
  CreateHook("vkQueueSubmit2", (void *)&Detour_vkQueueSubmit2,
             (void **)&o_vkQueueSubmit2);
  CreateHook("vkQueueSubmit2KHR", (void *)&Detour_vkQueueSubmit2KHR,
             (void **)&o_vkQueueSubmit2KHR);
  CreateHook("vkUpdateDescriptorSets", (void *)&Detour_vkUpdateDescriptorSets,
             (void **)&o_vkUpdateDescriptorSets);
  CreateHook("vkDestroySampler", (void *)&Detour_vkDestroySampler,
             (void **)&o_vkDestroySampler);
  CreateHook("vkCreateSwapchainKHR", (void *)&Detour_vkCreateSwapchainKHR,
             (void **)&o_vkCreateSwapchainKHR);

  // Critical for AF forcing - game may call these directly without ProcAddr lookup
  CreateHook("vkCreateSampler", (void *)&Detour_vkCreateSampler,
             (void **)&o_vkCreateSampler);
  CreateHook("vkCreateDescriptorUpdateTemplate", (void *)&Detour_vkCreateDescriptorUpdateTemplate,
             (void **)&o_vkCreateDescriptorUpdateTemplate);
  CreateHook("vkCreateDescriptorUpdateTemplateKHR", (void *)&Detour_vkCreateDescriptorUpdateTemplate,
             (void **)&o_vkCreateDescriptorUpdateTemplateKHR);
  CreateHook("vkDestroyDescriptorUpdateTemplate", (void *)&Detour_vkDestroyDescriptorUpdateTemplate,
             (void **)&o_vkDestroyDescriptorUpdateTemplate);
  CreateHook("vkDestroyDescriptorUpdateTemplateKHR", (void *)&Detour_vkDestroyDescriptorUpdateTemplate,
             (void **)&o_vkDestroyDescriptorUpdateTemplateKHR);
  CreateHook("vkUpdateDescriptorSetWithTemplate", (void *)&Detour_vkUpdateDescriptorSetWithTemplate,
             (void **)&o_vkUpdateDescriptorSetWithTemplate);
  CreateHook("vkUpdateDescriptorSetWithTemplateKHR", (void *)&Detour_vkUpdateDescriptorSetWithTemplate,
             (void **)&o_vkUpdateDescriptorSetWithTemplateKHR);
}

void VulkanHook::Shutdown() {
  HookLog("VulkanHook::Shutdown()");
  if (g_Device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(g_Device);
    for (auto &sc : g_Swapchains) {
      sc.Cleanup();
    }
    if (g_CommandPool)
      vkDestroyCommandPool(g_Device, g_CommandPool, nullptr);
    if (g_DescriptorPool != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
    if (g_RenderPass != VK_NULL_HANDLE)
      vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
    for (auto fence : g_PrerenderFences) {
        vkDestroyFence(g_Device, fence, nullptr);
    }
    g_PrerenderFences.clear();
    g_PrerenderDevice = VK_NULL_HANDLE;
  }
  g_VulkanCapture.Cleanup();
}

void VulkanHook::OnHostDisconnect() {
  HookLog("VulkanHook::OnHostDisconnect() - ready for reconnection");
  g_VulkanCapture.Cleanup();
}
