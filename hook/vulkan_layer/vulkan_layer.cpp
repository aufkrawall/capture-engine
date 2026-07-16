/**
 * Vulkan Layer - Core Implementation
 *
 * Consolidates all Vulkan hooks and state management.
 */

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_layer.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "../../common/strict_float_parse.h"
#include "../common/capture_pacing.h"
#include "../common/fps_limiter.h"
#include "../common/perf_logger.h"
#include "../common/performance_metrics.h"
#include "../common/screenshot_hook.h"
#include "layer_main.h"  // For LayerLog and g_LayerState

// Reentrancy guard shared with other hooks (defined here for the layer)
thread_local bool g_InPresentHook = false;

// CPU Prerender Limit for Vulkan
//
// COMPROMISE SOLUTION:
// Forcing backbuffer_count=2 on games designed for triple buffering causes
// microstutter. Instead, use fence-based frame limiting WITH the game's
// preferred backbuffer count:
//
// RECOMMENDED CONFIG:
//   backbuffer_count=3  (match game's expectation for smoothness)
//   cpu_prerender_limit=1  (we enforce ~1 frame latency via fences)
//
// This gives: smooth triple buffering + ~1 frame effective latency

struct FrameLimitState {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkFence> fences;
    std::vector<bool> submitted;
    uint64_t frameIndex = 0;
    uint32_t lookback = UINT32_MAX;
};
static std::mutex g_FrameLimitMutex;
static std::unordered_map<VkQueue, FrameLimitState> g_FrameLimitStates;

static bool IsDXVKD3D9WrapperLoaded() {
    return IsDllFromProject("d3d9.dll", "dxvk");
}

// Returns true when d3d11.dll is a DXVK wrapper (outside System32, version info contains "dxvk").
// Used alongside IsDXVKD3D9WrapperLoaded() to distinguish pure DX9-DXVK games
// (only d3d9 from DXVK) from DX10/11-DXVK games where d3d9.dll is also present.
static bool IsDXVKD3D11WrapperLoaded() {
    return IsDllFromProject("d3d11.dll", "dxvk");
}

static void ApplyPrerenderLimitVulkan(VkDevice device, VkQueue queue, float limit) {
    if (limit < 0.0f)
        return;
    if (queue == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        return;

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateFence || !disp->fp_vkDestroyFence || !disp->fp_vkWaitForFences ||
        !disp->fp_vkResetFences || !disp->fp_vkQueueSubmit) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_FrameLimitMutex);
    FrameLimitState& state = g_FrameLimitStates[queue];
    if (state.device != device || state.fences.empty()) {
        DeviceDispatch* oldDisp =
            state.device != VK_NULL_HANDLE ? VulkanLayerState::Get().GetDeviceDispatch(state.device) : nullptr;
        for (VkFence fence : state.fences) {
            if (fence != VK_NULL_HANDLE && oldDisp && oldDisp->fp_vkDestroyFence)
                oldDisp->fp_vkDestroyFence(state.device, fence, nullptr);
        }
        state = {};
        state.device = device;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (int index = 0; index < 7; ++index) {
            VkFence fence = VK_NULL_HANDLE;
            if (disp->fp_vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
                break;
            }
            state.fences.push_back(fence);
        }
        if (state.fences.size() != 7) {
            for (VkFence fence : state.fences)
                disp->fp_vkDestroyFence(device, fence, nullptr);
            g_FrameLimitStates.erase(queue);
            LayerLog("Vulkan Prerender: failed to create complete fence ring for queue=%p", queue);
            return;
        }
        state.submitted.assign(state.fences.size(), false);
        LayerLog("Vulkan Prerender: fence ring ready device=%p queue=%p", device, queue);
    }

    const uint32_t lookback = static_cast<uint32_t>(std::clamp(static_cast<int>(limit), 0, 6));
    if (state.lookback != lookback) {
        for (size_t index = 0; index < state.fences.size(); ++index) {
            if (!state.submitted[index])
                continue;
            VkFence fence = state.fences[index];
            const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            if (waitResult != VK_SUCCESS) {
                LayerLog("Vulkan Prerender: mode-change drain failed result=%d queue=%p", static_cast<int>(waitResult),
                         queue);
                return;
            }
            disp->fp_vkResetFences(device, 1, &fence);
            state.submitted[index] = false;
        }
        state.frameIndex = 0;
        state.lookback = lookback;
        LayerLog("Vulkan Prerender: queue=%p depth changed to %u", queue, lookback);
    }

    if (lookback > 0 && state.frameIndex >= lookback) {
        const size_t oldIndex = (state.frameIndex - lookback) % state.fences.size();
        VkFence oldFence = state.fences[oldIndex];
        const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &oldFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) {
            LayerLog("Vulkan Prerender: wait failed result=%d queue=%p frame=%llu", static_cast<int>(waitResult), queue,
                     static_cast<unsigned long long>(state.frameIndex));
            return;
        }
        disp->fp_vkResetFences(device, 1, &oldFence);
        state.submitted[oldIndex] = false;
    }

    const size_t currentIndex = state.frameIndex % state.fences.size();
    VkFence currentFence = state.fences[currentIndex];
    if (state.submitted[currentIndex]) {
        // The seven-slot ring guarantees this is unnecessary at a stable depth,
        // but keep reuse valid across unexpected state transitions.
        const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &currentFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS)
            return;
        disp->fp_vkResetFences(device, 1, &currentFence);
        state.submitted[currentIndex] = false;
    }
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkResult submitResult = disp->fp_vkQueueSubmit(queue, 1, &submitInfo, currentFence);
    if (submitResult != VK_SUCCESS) {
        LayerLog("Vulkan Prerender: marker submit failed result=%d queue=%p", static_cast<int>(submitResult), queue);
        return;
    }
    state.submitted[currentIndex] = true;
    if (lookback == 0) {
        const VkResult waitResult = disp->fp_vkWaitForFences(device, 1, &currentFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) {
            LayerLog("Vulkan Prerender: serial wait failed result=%d queue=%p", static_cast<int>(waitResult), queue);
            return;
        }
        disp->fp_vkResetFences(device, 1, &currentFence);
        state.submitted[currentIndex] = false;
    }
    ++state.frameIndex;
}

static void CleanupPrerenderFences(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_FrameLimitMutex);

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkDestroyFence) {
        for (auto it = g_FrameLimitStates.begin(); it != g_FrameLimitStates.end();) {
            if (it->second.device != device) {
                ++it;
                continue;
            }
            for (VkFence fence : it->second.fences)
                disp->fp_vkDestroyFence(device, fence, nullptr);
            it = g_FrameLimitStates.erase(it);
        }
        LayerLog("Vulkan Prerender: cleaned up device fence rings");
    }
}

// VulkanLayerState Implementation
// ============================================================================

VulkanLayerState& VulkanLayerState::Get() {
    static VulkanLayerState instance;
    return instance;
}

VulkanLayerState::VulkanLayerState()
    : m_OverlayEnabled(true),
      m_CaptureEnabled(true),
      m_MaxAnisotropy(16),
      m_AnisotropyOverrideActive(false),
      m_MipLodBias(0.0f),
      m_MipBiasOverrideActive(false),
      m_ForceMipBiasClamp(false),
      m_MipBiasMode("strict"),
      m_MipMapping("default"),
      m_SamplerOverrideMode("safe"),
      m_VsyncMode("default"),
      m_BackbufferCount(0),
      m_PrerenderLimit(-1.0f) {}

void VulkanLayerState::RegisterInstance(VkInstance instance, InstanceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Instances[instance] = dispatch;
}

void VulkanLayerState::UnregisterInstance(VkInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    if (it != m_Instances.end()) {
        delete it->second;
        m_Instances.erase(it);
    }
}

InstanceDispatch* VulkanLayerState::GetInstanceDispatch(VkInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    return (it != m_Instances.end()) ? it->second : nullptr;
}

void VulkanLayerState::RegisterDevice(VkDevice device, DeviceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Devices[device] = dispatch;
}

void VulkanLayerState::UnregisterDevice(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end()) {
        delete it->second;
        m_Devices.erase(it);
    }
}

DeviceDispatch* VulkanLayerState::GetDeviceDispatch(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end())
        return it->second;
    return nullptr;
}

void VulkanLayerState::RegisterQueue(VkQueue queue, VkDevice device, uint32_t familyIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Queues[queue] = device;
    m_QueueFamilies[queue] = familyIndex;

    uint32_t queueFlags = 0;
    auto deviceIt = m_Devices.find(device);
    if (deviceIt != m_Devices.end() && deviceIt->second) {
        VkPhysicalDevice physicalDevice = deviceIt->second->physicalDevice;
        auto physToInstanceIt = m_PhysDevToInstance.find(physicalDevice);
        if (physToInstanceIt != m_PhysDevToInstance.end()) {
            auto instanceIt = m_Instances.find(physToInstanceIt->second);
            if (instanceIt != m_Instances.end() && instanceIt->second &&
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties) {
                uint32_t queueFamilyCount = 0;
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                nullptr);
                if (familyIndex < queueFamilyCount) {
                    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                    instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                    queueFamilies.data());
                    queueFlags = queueFamilies[familyIndex].queueFlags;
                }
            }
        }
    }

    m_QueueFlags[queue] = queueFlags;
}

DeviceDispatch* VulkanLayerState::GetDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        auto itDev = m_Devices.find(it->second);
        if (itDev != m_Devices.end())
            return itDev->second;
    }
    return nullptr;
}

VkDevice VulkanLayerState::GetVkDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        return it->second;
    }
    return VK_NULL_HANDLE;
}

uint32_t VulkanLayerState::GetQueueFamilyIndex(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFamilies.find(queue);
    return (it != m_QueueFamilies.end()) ? it->second : VK_QUEUE_FAMILY_IGNORED;
}

uint32_t VulkanLayerState::GetQueueFlags(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFlags.find(queue);
    return (it != m_QueueFlags.end()) ? it->second : 0;
}

bool VulkanLayerState::QueueSupportsGraphics(VkQueue queue) {
    return (GetQueueFlags(queue) & VK_QUEUE_GRAPHICS_BIT) != 0;
}

bool VulkanLayerState::QueueSupportsTransfer(VkQueue queue) {
    const uint32_t flags = GetQueueFlags(queue);
    return (flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
}

void VulkanLayerState::NoteQueueSubmit(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto queueIt = m_Queues.find(queue);
    if (queueIt == m_Queues.end()) {
        return;
    }
    m_DeviceLastSubmitThreadIds[queueIt->second] = GetCurrentThreadId();
}

uint32_t VulkanLayerState::GetLastSubmitThreadId(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_DeviceLastSubmitThreadIds.find(device);
    return (it != m_DeviceLastSubmitThreadIds.end()) ? it->second : 0;
}

void VulkanLayerState::RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Swapchains[swapchain] = data;
}

void VulkanLayerState::UnregisterSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        delete it->second;
        m_Swapchains.erase(it);
        // Increment generation to invalidate all TLS caches
        m_SwapchainGeneration.fetch_add(1, std::memory_order_release);
    }
}

SwapchainData* VulkanLayerState::GetSwapchainData(VkSwapchainKHR swapchain) {
    static thread_local VkSwapchainKHR tls_LastSwapchain = VK_NULL_HANDLE;
    static thread_local SwapchainData* tls_LastData = nullptr;
    static thread_local uint64_t tls_LastGeneration = 0;

    // Check if TLS cache is still valid (no swapchains unregistered since last lookup)
    uint64_t currentGen = m_SwapchainGeneration.load(std::memory_order_acquire);
    if (currentGen != tls_LastGeneration) {
        tls_LastSwapchain = VK_NULL_HANDLE;
        tls_LastData = nullptr;
        tls_LastGeneration = currentGen;
    }

    if (swapchain == tls_LastSwapchain && tls_LastData)
        return tls_LastData;

    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        tls_LastSwapchain = swapchain;
        tls_LastData = it->second;
        tls_LastGeneration = m_SwapchainGeneration.load(std::memory_order_acquire);
        return tls_LastData;
    }
    return nullptr;
}

void VulkanLayerState::RegisterSurface(VkSurfaceKHR surface, HWND window) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Surfaces[surface] = window;
}

void VulkanLayerState::UnregisterSurface(VkSurfaceKHR surface) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Surfaces.erase(surface);
}

HWND VulkanLayerState::GetSurfaceWindow(VkSurfaceKHR surface) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Surfaces.find(surface);
    return (it != m_Surfaces.end()) ? it->second : NULL;
}

void VulkanLayerState::TrackPhysicalDevice(VkPhysicalDevice pd, VkInstance inst) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_PhysDevToInstance[pd] = inst;
}

VkInstance VulkanLayerState::GetInstanceFromPhysicalDevice(VkPhysicalDevice pd) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_PhysDevToInstance.find(pd);
    return (it != m_PhysDevToInstance.end()) ? it->second : VK_NULL_HANDLE;
}

void VulkanLayerState::UpdateFromSharedMemory(IPCClient* ipc) {
    if (!ipc || !ipc->GetSharedMem())
        return;

    auto& cfg = ipc->GetSharedMem()->graphicsConfig;

    // Parse anisotropic filtering
    if (strncmp(cfg.anisotropicFiltering, "default", 7) == 0 || cfg.anisotropicFiltering[0] == '\0') {
        m_AnisotropyOverrideActive = false;
        m_MaxAnisotropy = 16;
    } else {
        m_AnisotropyOverrideActive = true;
        if (strncmp(cfg.anisotropicFiltering, "16x", 3) == 0)
            m_MaxAnisotropy = 16;
        else if (strncmp(cfg.anisotropicFiltering, "8x", 2) == 0)
            m_MaxAnisotropy = 8;
        else if (strncmp(cfg.anisotropicFiltering, "4x", 2) == 0)
            m_MaxAnisotropy = 4;
        else if (strncmp(cfg.anisotropicFiltering, "2x", 2) == 0)
            m_MaxAnisotropy = 2;
        else if (strncmp(cfg.anisotropicFiltering, "off", 3) == 0)
            m_MaxAnisotropy = 1;
        else {
            m_AnisotropyOverrideActive = false;
            m_MaxAnisotropy = 16;
        }
    }

    // Parse mip bias
    if (strncmp(cfg.mipBias, "default", 7) != 0 && cfg.mipBias[0] != '\0') {
        float parsedBias = 0.0f;
        if (ce::TryParseFiniteFloat(cfg.mipBias, parsedBias)) {
            m_MipLodBias = parsedBias;
            m_MipBiasOverrideActive = true;
        } else {
            m_MipLodBias = 0.0f;
            m_MipBiasOverrideActive = false;
            static std::atomic<bool> loggedInvalidMipBias{false};
            if (!loggedInvalidMipBias.exchange(true, std::memory_order_relaxed))
                LayerLog("VulkanLayerState: rejected malformed mip bias '%s'", cfg.mipBias);
        }
    } else {
        m_MipLodBias = 0.0f;
        m_MipBiasOverrideActive = false;
    }

    // Parse mip bias mode
    if (cfg.mipBiasMode[0] != '\0')
        m_MipBiasMode = cfg.mipBiasMode;
    else
        m_MipBiasMode = "strict";

    m_ForceMipBiasClamp = cfg.forceMipBiasClamp;
    m_MipMapping = cfg.mipMapping[0] ? cfg.mipMapping : "default";
    m_SamplerOverrideMode = cfg.samplerOverrideMode[0] ? cfg.samplerOverrideMode : "safe";

    // VSync mode
    m_VsyncMode = cfg.vsyncMode;

    // Backbuffer count
    m_BackbufferCount = cfg.backbufferCount;

    // Prerender limit
    m_PrerenderLimit = cfg.prerenderLimit;

    LayerLog(
        "VulkanLayerState: Updated from config - policy=%s AF=%d, MipBias=%.1f, "
        "MipMap=%s, Clamp=%d, VSync=%s, BBCount=%d",
        m_SamplerOverrideMode.c_str(), m_MaxAnisotropy, m_MipLodBias, m_MipMapping.c_str(), m_ForceMipBiasClamp ? 1 : 0,
        m_VsyncMode.c_str(), m_BackbufferCount);
}

// ============================================================================
// Helper to populate dispatch tables
// ============================================================================

void PopulateInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance, PFN_vkGetInstanceProcAddr gipa) {
    dispatch->instance = instance;
    dispatch->fp_vkGetInstanceProcAddr = gipa;
    dispatch->fp_vkDestroyInstance = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
    dispatch->fp_vkEnumeratePhysicalDevices =
        (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
    dispatch->fp_vkGetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    dispatch->fp_vkGetPhysicalDeviceProperties2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    dispatch->fp_vkGetPhysicalDeviceFeatures =
        (PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures");
    dispatch->fp_vkGetPhysicalDeviceFeatures2 =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    dispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    dispatch->fp_vkGetPhysicalDeviceMemoryProperties =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
    dispatch->fp_vkCreateDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    dispatch->fp_vkEnumerateDeviceExtensionProperties =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    dispatch->fp_vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfaceSupportKHR =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfaceFormatsKHR =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    dispatch->fp_vkGetPhysicalDeviceSurfacePresentModesKHR =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    dispatch->fp_vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR)gipa(instance, "vkCreateWin32SurfaceKHR");
#endif
}

void PopulateDeviceDispatch(DeviceDispatch* dispatch, VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    dispatch->device = device;
    dispatch->fp_vkGetDeviceProcAddr = gdpa;
    dispatch->fp_vkDestroyDevice = (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
    dispatch->fp_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(device, "vkGetDeviceQueue");
    dispatch->fp_vkGetDeviceQueue2 = (PFN_vkGetDeviceQueue2)gdpa(device, "vkGetDeviceQueue2");
    dispatch->fp_vkQueueSubmit = (PFN_vkQueueSubmit)gdpa(device, "vkQueueSubmit");
    dispatch->fp_vkQueueSubmit2 = (PFN_vkQueueSubmit2)gdpa(device, "vkQueueSubmit2");
    dispatch->fp_vkQueueSubmit2KHR = (PFN_vkQueueSubmit2KHR)gdpa(device, "vkQueueSubmit2KHR");
    dispatch->fp_vkQueueWaitIdle = (PFN_vkQueueWaitIdle)gdpa(device, "vkQueueWaitIdle");
    dispatch->fp_vkDeviceWaitIdle = (PFN_vkDeviceWaitIdle)gdpa(device, "vkDeviceWaitIdle");
    dispatch->fp_vkAllocateMemory = (PFN_vkAllocateMemory)gdpa(device, "vkAllocateMemory");
    dispatch->fp_vkFreeMemory = (PFN_vkFreeMemory)gdpa(device, "vkFreeMemory");
    dispatch->fp_vkMapMemory = (PFN_vkMapMemory)gdpa(device, "vkMapMemory");
    dispatch->fp_vkUnmapMemory = (PFN_vkUnmapMemory)gdpa(device, "vkUnmapMemory");
    dispatch->fp_vkBindBufferMemory = (PFN_vkBindBufferMemory)gdpa(device, "vkBindBufferMemory");
    dispatch->fp_vkBindImageMemory = (PFN_vkBindImageMemory)gdpa(device, "vkBindImageMemory");
    dispatch->fp_vkCreateBuffer = (PFN_vkCreateBuffer)gdpa(device, "vkCreateBuffer");
    dispatch->fp_vkDestroyBuffer = (PFN_vkDestroyBuffer)gdpa(device, "vkDestroyBuffer");
    dispatch->fp_vkCreateImage = (PFN_vkCreateImage)gdpa(device, "vkCreateImage");
    dispatch->fp_vkDestroyImage = (PFN_vkDestroyImage)gdpa(device, "vkDestroyImage");
    dispatch->fp_vkGetImageMemoryRequirements =
        (PFN_vkGetImageMemoryRequirements)gdpa(device, "vkGetImageMemoryRequirements");
    dispatch->fp_vkGetBufferMemoryRequirements =
        (PFN_vkGetBufferMemoryRequirements)gdpa(device, "vkGetBufferMemoryRequirements");
    dispatch->fp_vkCreateImageView = (PFN_vkCreateImageView)gdpa(device, "vkCreateImageView");
    dispatch->fp_vkDestroyImageView = (PFN_vkDestroyImageView)gdpa(device, "vkDestroyImageView");
    dispatch->fp_vkCreateSampler = (PFN_vkCreateSampler)gdpa(device, "vkCreateSampler");
    dispatch->fp_vkDestroySampler = (PFN_vkDestroySampler)gdpa(device, "vkDestroySampler");
    dispatch->fp_vkCreateFramebuffer = (PFN_vkCreateFramebuffer)gdpa(device, "vkCreateFramebuffer");
    dispatch->fp_vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer)gdpa(device, "vkDestroyFramebuffer");
    dispatch->fp_vkCreateRenderPass = (PFN_vkCreateRenderPass)gdpa(device, "vkCreateRenderPass");
    dispatch->fp_vkDestroyRenderPass = (PFN_vkDestroyRenderPass)gdpa(device, "vkDestroyRenderPass");
    dispatch->fp_vkCreateCommandPool = (PFN_vkCreateCommandPool)gdpa(device, "vkCreateCommandPool");
    dispatch->fp_vkDestroyCommandPool = (PFN_vkDestroyCommandPool)gdpa(device, "vkDestroyCommandPool");
    dispatch->fp_vkAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)gdpa(device, "vkAllocateCommandBuffers");
    dispatch->fp_vkFreeCommandBuffers = (PFN_vkFreeCommandBuffers)gdpa(device, "vkFreeCommandBuffers");
    dispatch->fp_vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer)gdpa(device, "vkBeginCommandBuffer");
    dispatch->fp_vkEndCommandBuffer = (PFN_vkEndCommandBuffer)gdpa(device, "vkEndCommandBuffer");
    dispatch->fp_vkResetCommandBuffer = (PFN_vkResetCommandBuffer)gdpa(device, "vkResetCommandBuffer");
    dispatch->fp_vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass)gdpa(device, "vkCmdBeginRenderPass");
    dispatch->fp_vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass)gdpa(device, "vkCmdEndRenderPass");
    dispatch->fp_vkCmdBindPipeline = (PFN_vkCmdBindPipeline)gdpa(device, "vkCmdBindPipeline");
    dispatch->fp_vkCmdDraw = (PFN_vkCmdDraw)gdpa(device, "vkCmdDraw");
    dispatch->fp_vkCmdDrawIndexed = (PFN_vkCmdDrawIndexed)gdpa(device, "vkCmdDrawIndexed");
    dispatch->fp_vkCmdPushConstants = (PFN_vkCmdPushConstants)gdpa(device, "vkCmdPushConstants");
    dispatch->fp_vkCmdSetViewport = (PFN_vkCmdSetViewport)gdpa(device, "vkCmdSetViewport");
    dispatch->fp_vkCmdSetScissor = (PFN_vkCmdSetScissor)gdpa(device, "vkCmdSetScissor");
    dispatch->fp_vkCmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers)gdpa(device, "vkCmdBindVertexBuffers");
    dispatch->fp_vkCmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer)gdpa(device, "vkCmdBindIndexBuffer");
    dispatch->fp_vkCmdCopyImage = (PFN_vkCmdCopyImage)gdpa(device, "vkCmdCopyImage");
    dispatch->fp_vkCmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)gdpa(device, "vkCmdCopyImageToBuffer");
    dispatch->fp_vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)gdpa(device, "vkCmdCopyBufferToImage");
    dispatch->fp_vkCmdBlitImage = (PFN_vkCmdBlitImage)gdpa(device, "vkCmdBlitImage");
    dispatch->fp_vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)gdpa(device, "vkCmdBindDescriptorSets");
    dispatch->fp_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier");
    dispatch->fp_vkCmdClearAttachments = (PFN_vkCmdClearAttachments)gdpa(device, "vkCmdClearAttachments");
    dispatch->fp_vkCreateFence = (PFN_vkCreateFence)gdpa(device, "vkCreateFence");
    dispatch->fp_vkDestroyFence = (PFN_vkDestroyFence)gdpa(device, "vkDestroyFence");
    dispatch->fp_vkWaitForFences = (PFN_vkWaitForFences)gdpa(device, "vkWaitForFences");
    dispatch->fp_vkResetFences = (PFN_vkResetFences)gdpa(device, "vkResetFences");
    dispatch->fp_vkCreateSemaphore = (PFN_vkCreateSemaphore)gdpa(device, "vkCreateSemaphore");
    dispatch->fp_vkDestroySemaphore = (PFN_vkDestroySemaphore)gdpa(device, "vkDestroySemaphore");
    dispatch->fp_vkCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
    dispatch->fp_vkDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)gdpa(device, "vkDestroySwapchainKHR");
    dispatch->fp_vkGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)gdpa(device, "vkGetSwapchainImagesKHR");
    dispatch->fp_vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gdpa(device, "vkAcquireNextImageKHR");
    dispatch->fp_vkQueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(device, "vkQueuePresentKHR");
    dispatch->fp_vkCreateDescriptorSetLayout =
        (PFN_vkCreateDescriptorSetLayout)gdpa(device, "vkCreateDescriptorSetLayout");
    dispatch->fp_vkDestroyDescriptorSetLayout =
        (PFN_vkDestroyDescriptorSetLayout)gdpa(device, "vkDestroyDescriptorSetLayout");
    dispatch->fp_vkCreateDescriptorPool = (PFN_vkCreateDescriptorPool)gdpa(device, "vkCreateDescriptorPool");
    dispatch->fp_vkDestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)gdpa(device, "vkDestroyDescriptorPool");
    dispatch->fp_vkAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)gdpa(device, "vkAllocateDescriptorSets");
    dispatch->fp_vkFreeDescriptorSets = (PFN_vkFreeDescriptorSets)gdpa(device, "vkFreeDescriptorSets");
    dispatch->fp_vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)gdpa(device, "vkUpdateDescriptorSets");
    dispatch->fp_vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout)gdpa(device, "vkCreatePipelineLayout");
    dispatch->fp_vkDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)gdpa(device, "vkDestroyPipelineLayout");
    dispatch->fp_vkCreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)gdpa(device, "vkCreateGraphicsPipelines");
    dispatch->fp_vkDestroyPipeline = (PFN_vkDestroyPipeline)gdpa(device, "vkDestroyPipeline");
    dispatch->fp_vkCreateShaderModule = (PFN_vkCreateShaderModule)gdpa(device, "vkCreateShaderModule");
    dispatch->fp_vkDestroyShaderModule = (PFN_vkDestroyShaderModule)gdpa(device, "vkDestroyShaderModule");
#ifdef VK_USE_PLATFORM_WIN32_KHR
    dispatch->fp_vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)gdpa(device, "vkGetMemoryWin32HandleKHR");
    dispatch->fp_vkGetMemoryWin32HandlePropertiesKHR =
        (PFN_vkGetMemoryWin32HandlePropertiesKHR)gdpa(device, "vkGetMemoryWin32HandlePropertiesKHR");
    dispatch->fp_vkGetSemaphoreWin32HandleKHR =
        (PFN_vkGetSemaphoreWin32HandleKHR)gdpa(device, "vkGetSemaphoreWin32HandleKHR");
#endif
}

// ============================================================================
// Hook Implementations
// ============================================================================

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkInstance* pInstance) {
    LayerLog("Vulkan Layer: Capture_vkCreateInstance BEGIN");

    if (!pCreateInfo) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateInstance called with NULL pCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pInstance) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateInstance called with NULL pInstance");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    uint32_t apiVersion = pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0;
    LayerLog(
        "Vulkan Layer: Instance create info - apiVersion=%u.%u.%u, "
        "enabledLayerCount=%u, enabledExtensionCount=%u",
        VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion),
        pCreateInfo->enabledLayerCount, pCreateInfo->enabledExtensionCount);

    // Mark Vulkan layer as active in shared memory
    auto* shm = g_IPCClient.GetSharedMem();
    if (shm)
        shm->runtimeState.vulkanLayerActive = true;

    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)NULL;
    VkLayerInstanceCreateInfo* chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    LayerLog("Vulkan Layer: Searching for VK_LAYER_LINK_INFO in pNext chain...");
    uint32_t chainDepth = 0;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        LayerLog("Vulkan Layer:   chain[%u] sType=%u, function=%u", chainDepth, chain_info->sType,
                 chain_info->function);
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
        chainDepth++;
    }
    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);
    gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    LayerLog("Vulkan Layer: pfnNextGetInstanceProcAddr=%p", (void*)gipa);
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance create_fn = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");

    VkResult res = VK_SUCCESS;

    if (!g_LayerState.whitelisted) {
        // Passthrough: call next layer directly without modification
        res = create_fn(pCreateInfo, pAllocator, pInstance);
    } else {
        // Inject required extensions
        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }

        bool hasProps2 = false;
        bool hasExtMemCaps = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
                hasProps2 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME) == 0)
                hasExtMemCaps = true;
        }

        if (!hasProps2)
            extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        if (!hasExtMemCaps)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

        VkInstanceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        // Enable validation layers in debug builds
#ifdef _DEBUG
        const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
        bool hasValidationLayer = false;

        // Check if already requested
        for (uint32_t i = 0; i < modifiedCreateInfo.enabledLayerCount; i++) {
            if (strcmp(modifiedCreateInfo.ppEnabledLayerNames[i], validationLayerName) == 0) {
                hasValidationLayer = true;
                break;
            }
        }

        if (!hasValidationLayer) {
            // Query available layers
            uint32_t layerCount = 0;
            PFN_vkEnumerateInstanceLayerProperties enumerateLayers =
                (PFN_vkEnumerateInstanceLayerProperties)gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
            if (enumerateLayers) {
                enumerateLayers(&layerCount, nullptr);
                std::vector<VkLayerProperties> availableLayers(layerCount);
                enumerateLayers(&layerCount, availableLayers.data());

                // Check if validation layer is available
                for (const auto& layer : availableLayers) {
                    if (strcmp(layer.layerName, validationLayerName) == 0) {
                        static const char* validationLayers[] = {validationLayerName};
                        modifiedCreateInfo.enabledLayerCount = 1;
                        modifiedCreateInfo.ppEnabledLayerNames = validationLayers;
                        LayerLog("Vulkan Layer: Enabling validation layer: %s", validationLayerName);
                        break;
                    }
                }
            }
        }
#endif

        LayerLog("Vulkan Layer: Calling next vkCreateInstance...");
        res = create_fn(&modifiedCreateInfo, pAllocator, pInstance);
    }

    LayerLog("Vulkan Layer: next vkCreateInstance returned %d", res);
    if (res != VK_SUCCESS)
        return res;

    auto* dispatch = new InstanceDispatch();
    PopulateInstanceDispatch(dispatch, *pInstance, gipa);
    VulkanLayerState::Get().RegisterInstance(*pInstance, dispatch);

    LayerLog("Vulkan Layer: Capture_vkCreateInstance END - success, instance=%p", (void*)*pInstance);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (disp && disp->fp_vkDestroyInstance)
        disp->fp_vkDestroyInstance(instance, pAllocator);
    VulkanLayerState::Get().UnregisterInstance(instance);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                                                                  VkPhysicalDevice* pPhysicalDevices) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (!disp || !disp->fp_vkEnumeratePhysicalDevices)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = disp->fp_vkEnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if (res >= VK_SUCCESS && pPhysicalDevices && pPhysicalDeviceCount && *pPhysicalDeviceCount > 0) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            VulkanLayerState::Get().TrackPhysicalDevice(pPhysicalDevices[i], instance);
        }
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                      const VkDeviceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    LayerLog("Vulkan Layer: Capture_vkCreateDevice BEGIN");

    if (!pCreateInfo) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pCreateInfo");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (!pDevice) {
        LayerLog("Vulkan Layer: [Error] Capture_vkCreateDevice called with NULL pDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    LayerLog(
        "Vulkan Layer: Device create info - queueCreateInfoCount=%u, "
        "enabledExtensionCount=%u, enabledLayerCount=%u",
        pCreateInfo->queueCreateInfoCount, pCreateInfo->enabledExtensionCount, pCreateInfo->enabledLayerCount);

    VkInstance instance = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physicalDevice);
    if (instance == VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: [Error] Could not find instance for physical device %p", physicalDevice);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found instance %p for physical device %p", (void*)instance, (void*)physicalDevice);

    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    LayerLog("Vulkan Layer: Searching for VK_LAYER_LINK_INFO in device pNext chain...");
    uint32_t chainDepth = 0;
    while (chain_info && !(chain_info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                           chain_info->function == VK_LAYER_LINK_INFO)) {
        LayerLog("Vulkan Layer:   chain[%u] sType=%u, function=%u", chainDepth, chain_info->sType,
                 chain_info->function);
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
        chainDepth++;
    }
    if (!chain_info) {
        LayerLog(
            "Vulkan Layer: [Error] VK_LAYER_LINK_INFO not found in device pNext chain "
            "after %u iterations",
            chainDepth);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LayerLog("Vulkan Layer: Found VK_LAYER_LINK_INFO at depth %u", chainDepth);

    PFN_vkGetDeviceProcAddr gdpa = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr gipa = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice create_fn = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (!create_fn) {
        LayerLog(
            "Vulkan Layer: [Error] Failed to get next vkCreateDevice from "
            "instance %p",
            instance);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = VK_SUCCESS;
    bool captureInteropEnabled = false;
    bool samplerAnisotropyEnabled = false;
    float maxSamplerAnisotropy = 1.0f;
    float maxSamplerLodBias = 0.0f;

    if (!g_LayerState.whitelisted) {
        // Passthrough: call next layer directly without modification
        result = create_fn(physicalDevice, pCreateInfo, pAllocator, pDevice);
    } else {
        InstanceDispatch* instanceDispatch = VulkanLayerState::Get().GetInstanceDispatch(instance);
        std::vector<VkExtensionProperties> availableExtensions;
        if (instanceDispatch && instanceDispatch->fp_vkEnumerateDeviceExtensionProperties) {
            uint32_t extensionCount = 0;
            if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount,
                                                                          nullptr) == VK_SUCCESS &&
                extensionCount > 0) {
                availableExtensions.resize(extensionCount);
                if (instanceDispatch->fp_vkEnumerateDeviceExtensionProperties(
                        physicalDevice, nullptr, &extensionCount, availableExtensions.data()) != VK_SUCCESS) {
                    availableExtensions.clear();
                }
            }
        }
        auto extensionAvailable = [&](const char* name) {
            return std::any_of(availableExtensions.begin(), availableExtensions.end(),
                               [&](const VkExtensionProperties& ext) { return strcmp(ext.extensionName, name) == 0; });
        };

        VkPhysicalDeviceProperties physicalProperties = {};
        if (instanceDispatch && instanceDispatch->fp_vkGetPhysicalDeviceProperties)
            instanceDispatch->fp_vkGetPhysicalDeviceProperties(physicalDevice, &physicalProperties);
        maxSamplerAnisotropy = physicalProperties.limits.maxSamplerAnisotropy;
        maxSamplerLodBias = physicalProperties.limits.maxSamplerLodBias;
        const bool externalMemoryCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool externalSemaphoreCore = physicalProperties.apiVersion >= VK_API_VERSION_1_1;
        const bool timelineCore = physicalProperties.apiVersion >= VK_API_VERSION_1_2;

        PFN_vkGetPhysicalDeviceFeatures2 getFeatures2 =
            instanceDispatch ? instanceDispatch->fp_vkGetPhysicalDeviceFeatures2 : nullptr;
        if (!getFeatures2) {
            getFeatures2 =
                reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures supportedTimeline = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        if (getFeatures2) {
            VkPhysicalDeviceFeatures2 supportedFeatures = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            supportedFeatures.pNext = &supportedTimeline;
            getFeatures2(physicalDevice, &supportedFeatures);
        }

        const bool requiredExtensionsAvailable =
            (externalMemoryCore || extensionAvailable(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
            (externalSemaphoreCore || extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME)) &&
            extensionAvailable(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) &&
            (timelineCore || extensionAvailable(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));

        const VkPhysicalDeviceTimelineSemaphoreFeatures* requestedTimeline = nullptr;
        const VkPhysicalDeviceVulkan12Features* requestedVulkan12 = nullptr;
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                requestedTimeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
            } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                requestedVulkan12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node);
            }
        }
        const bool canEnableTimeline = supportedTimeline.timelineSemaphore == VK_TRUE;
        const bool appSpecifiedTimeline = requestedTimeline || requestedVulkan12;
        const bool appAlreadyEnabledTimeline = (requestedTimeline && requestedTimeline->timelineSemaphore == VK_TRUE) ||
                                               (requestedVulkan12 && requestedVulkan12->timelineSemaphore == VK_TRUE);
        captureInteropEnabled =
            requiredExtensionsAvailable && canEnableTimeline && (!appSpecifiedTimeline || appAlreadyEnabledTimeline);

        // Inject capture extensions only when the physical device actually
        // advertises the complete Win32 external-memory/fence contract. Never
        // make the game's vkCreateDevice fail merely because capture is absent.
        std::vector<const char*> extensions;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            extensions.push_back(pCreateInfo->ppEnabledExtensionNames[i]);
        }

        bool hasExtMem = false;
        bool hasExtMemWin32 = false;
        bool hasExtSem = false;
        bool hasExtSemWin32 = false;
        bool hasTimeline = false;
        for (const char* ext : extensions) {
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
                hasExtMem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0)
                hasExtMemWin32 = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) == 0)
                hasExtSem = true;
            if (strcmp(ext, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME) == 0)
                hasExtSemWin32 = true;
            if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0)
                hasTimeline = true;
        }

        if (captureInteropEnabled && !hasExtMem && !externalMemoryCore)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtMemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSem && !externalSemaphoreCore)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME);
        if (captureInteropEnabled && !hasExtSemWin32)
            extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        if (captureInteropEnabled && !hasTimeline && !timelineCore)
            extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

        VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.enabledExtensionCount = (uint32_t)extensions.size();
        modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();

        VkPhysicalDeviceFeatures enabledFeatures = {};
        if (pCreateInfo->pEnabledFeatures) {
            enabledFeatures = *pCreateInfo->pEnabledFeatures;
            samplerAnisotropyEnabled = enabledFeatures.samplerAnisotropy == VK_TRUE;
            modifiedCreateInfo.pEnabledFeatures = &enabledFeatures;
        } else {
            for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext); node;
                 node = node->pNext) {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                    const auto* features2 = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node);
                    samplerAnisotropyEnabled = features2->features.samplerAnisotropy == VK_TRUE;
                    break;
                }
            }
        }

        // Enable timeline semaphores only when the app did not already include
        // the feature structure. Duplicating an sType in pNext is invalid.
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        timelineFeatures.timelineSemaphore = VK_TRUE;
        if (captureInteropEnabled && !appSpecifiedTimeline) {
            timelineFeatures.pNext = (void*)modifiedCreateInfo.pNext;
            modifiedCreateInfo.pNext = &timelineFeatures;
        }

        if (!captureInteropEnabled) {
            LayerLog(
                "Vulkan Layer: Win32 external capture unavailable; creating device without capture-only "
                "extensions (extensions=%d timelineSupported=%d timelineRequested=%d)",
                requiredExtensionsAvailable ? 1 : 0, canEnableTimeline ? 1 : 0,
                appSpecifiedTimeline ? (appAlreadyEnabledTimeline ? 1 : 0) : -1);
        }

        LayerLog("Vulkan Layer: Calling next vkCreateDevice...");
        result = create_fn(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    }

    LayerLog("Vulkan Layer: next vkCreateDevice returned %d", result);
    if (result != VK_SUCCESS)
        return result;

    auto* dispatch = new DeviceDispatch();
    dispatch->physicalDevice = physicalDevice;
    dispatch->captureInteropEnabled = captureInteropEnabled;
    dispatch->samplerAnisotropyEnabled = samplerAnisotropyEnabled;
    dispatch->maxSamplerAnisotropy = maxSamplerAnisotropy;
    dispatch->maxSamplerLodBias = maxSamplerLodBias;
    PopulateDeviceDispatch(dispatch, *pDevice, gdpa);
    VulkanLayerState::Get().RegisterDevice(*pDevice, dispatch);

    LayerLog("Vulkan Layer: Capture_vkCreateDevice END - success, device=%p", (void*)*pDevice);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    CleanupOverlay(device);
    CleanupCapture(device);
    CleanupPrerenderFences(device);
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkDestroyDevice)
        disp->fp_vkDestroyDevice(device, pAllocator);
    VulkanLayerState::Get().UnregisterDevice(device);
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                                    VkQueue* pQueue) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkGetDeviceQueue) {
        disp->fp_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue != VK_NULL_HANDLE) {
            VulkanLayerState::Get().RegisterQueue(*pQueue, device, queueFamilyIndex);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                                                     VkQueue* pQueue) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkGetDeviceQueue2 || !pQueueInfo) {
        return;
    }

    disp->fp_vkGetDeviceQueue2(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) {
        VulkanLayerState::Get().RegisterQueue(*pQueue, device, pQueueInfo->queueFamilyIndex);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(VkDevice device,
                                                            const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkSwapchainKHR* pSwapchain) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateSwapchainKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    // Apply config overrides
    VkSwapchainCreateInfoKHR modifiedCI = *pCreateInfo;
    bool modified = false;

    if (g_LayerState.whitelisted) {
        // VSync / Present mode override
        const char* vsyncMode = VulkanLayerState::Get().GetVsyncMode();
        if (vsyncMode && strcmp(vsyncMode, "default") != 0) {
            VkPresentModeKHR desiredMode = pCreateInfo->presentMode;
            if (strcmp(vsyncMode, "off") == 0)
                desiredMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (strcmp(vsyncMode, "fifo") == 0)
                desiredMode = VK_PRESENT_MODE_FIFO_KHR;
            else if (strcmp(vsyncMode, "mailbox") == 0)
                desiredMode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (strcmp(vsyncMode, "adaptive") == 0)
                desiredMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;

            if (desiredMode != pCreateInfo->presentMode) {
                // Validate that the desired present mode is supported
                bool modeSupported = false;
                VkPhysicalDevice physDev = disp->physicalDevice;
                VkInstance inst = VulkanLayerState::Get().GetInstanceFromPhysicalDevice(physDev);
                InstanceDispatch* instDisp = VulkanLayerState::Get().GetInstanceDispatch(inst);
                if (instDisp && instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR) {
                    uint32_t modeCount = 0;
                    instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, pCreateInfo->surface, &modeCount,
                                                                           nullptr);
                    if (modeCount > 0) {
                        std::vector<VkPresentModeKHR> modes(modeCount);
                        instDisp->fp_vkGetPhysicalDeviceSurfacePresentModesKHR(physDev, pCreateInfo->surface,
                                                                               &modeCount, modes.data());
                        for (uint32_t i = 0; i < modeCount; i++) {
                            if (modes[i] == desiredMode) {
                                modeSupported = true;
                                break;
                            }
                        }
                    }
                } else {
                    // Can't validate, assume FIFO is always supported per Vulkan spec
                    modeSupported = (desiredMode == VK_PRESENT_MODE_FIFO_KHR);
                }

                if (modeSupported) {
                    modifiedCI.presentMode = desiredMode;
                    modified = true;
                    LayerLog("Vulkan Layer: Overriding present mode %d -> %d (%s)", pCreateInfo->presentMode,
                             desiredMode, vsyncMode);
                } else {
                    LayerLog(
                        "Vulkan Layer: Present mode %d (%s) not supported, keeping "
                        "original %d",
                        desiredMode, vsyncMode, pCreateInfo->presentMode);
                }
            }
        }

        // Backbuffer count override
        int32_t bbCount = VulkanLayerState::Get().GetBackbufferCount();
        if (bbCount >= 2 && bbCount != (int32_t)pCreateInfo->minImageCount) {
            modifiedCI.minImageCount = (uint32_t)bbCount;
            modified = true;
            LayerLog("Vulkan Layer: Overriding minImageCount %u -> %u", pCreateInfo->minImageCount,
                     modifiedCI.minImageCount);
        }
    }

    const VkSwapchainCreateInfoKHR* pFinalCI = modified ? &modifiedCI : pCreateInfo;

    // CRITICAL: Clean up old swapchain resources before creating new one
    // This prevents fence/semaphore conflicts when the game recreates the swapchain
    if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: Cleaning up old swapchain %p before recreation", pCreateInfo->oldSwapchain);
        SwapchainData* oldSd = VulkanLayerState::Get().GetSwapchainData(pCreateInfo->oldSwapchain);
        if (oldSd) {
            CleanupOverlay(oldSd->device);
        }
        VulkanLayerState::Get().UnregisterSwapchain(pCreateInfo->oldSwapchain);
    }

    VkResult res = disp->fp_vkCreateSwapchainKHR(device, pFinalCI, pAllocator, pSwapchain);
    LayerLog("Vulkan Layer: vkCreateSwapchainKHR driver returned: %d", res);
    if (res == VK_SUCCESS && g_LayerState.whitelisted) {
        auto* sd = new SwapchainData();
        sd->swapchain = *pSwapchain;
        sd->device = device;
        sd->format = pCreateInfo->imageFormat;
        sd->extent = pCreateInfo->imageExtent;

        uint32_t count = 0;
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        sd->images.resize(count);
        disp->fp_vkGetSwapchainImagesKHR(device, *pSwapchain, &count, sd->images.data());
        sd->imageCount = count;

        HWND window = VulkanLayerState::Get().GetSurfaceWindow(pCreateInfo->surface);
        LayerLog("Vulkan Layer: Initializing overlay for swapchain %p, images=%d", *pSwapchain, count);
        const bool isTinySwapchain = (sd->extent.width < 320 || sd->extent.height < 180);
        const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
        if (isTinySwapchain) {
            LayerLog(
                "Vulkan Layer: [Info] Skipping overlay/capture init for tiny "
                "swapchain %ux%u",
                sd->extent.width, sd->extent.height);
        } else {
            if (preferDX9Path) {
                // DXVK d3d9: skip overlay (DX9 hook handles it) but still init capture for zero-copy
                LayerLog("Vulkan Layer: DXVK d3d9 - skipping overlay, initializing Vulkan capture (%ux%u)",
                         sd->extent.width, sd->extent.height);
                InitializeCapture(device, *pSwapchain, sd->format, sd->extent, count);
                // Ensure vulkanLayerActive is set: may have been missed at vkCreateInstance if IPC
                // wasn't ready yet. DX9 hook checks this flag to decide whether to use Vulkan
                // capture vs its own staging path. Setting it here (before any Present) guarantees
                // the Vulkan layer capture path is used for all frames.
                auto* shmPtr = g_IPCClient.GetSharedMem();
                if (shmPtr && !shmPtr->runtimeState.vulkanLayerActive.load(std::memory_order_acquire)) {
                    LayerLog(
                        "Vulkan Layer: DXVK d3d9 swapchain - setting vulkanLayerActive=true (deferred from "
                        "vkCreateInstance)");
                    shmPtr->runtimeState.vulkanLayerActive.store(true, std::memory_order_release);
                }
            } else {
                InitializeOverlay(device, *pSwapchain, sd->format, sd->extent, count, sd->images.data(), window);
                LayerLog(
                    "Vulkan Layer: InitializeOverlay returned, registering "
                    "swapchain");
                InitializeCapture(device, *pSwapchain, sd->format, sd->extent, count);
            }
        }

        VulkanLayerState::Get().RegisterSwapchain(*pSwapchain, sd);
        LayerLog("Vulkan Layer: Swapchain registration complete");
    }
    LayerLog("Vulkan Layer: vkCreateSwapchainKHR returning: %d", res);
    return res;
}

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                         const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    RetireCaptureSwapchain(device, swapchain);
    if (disp && disp->fp_vkDestroySwapchainKHR)
        disp->fp_vkDestroySwapchainKHR(device, swapchain, pAllocator);
    VulkanLayerState::Get().UnregisterSwapchain(swapchain);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                               uint32_t* pSwapchainImageCount,
                                                               VkImage* pSwapchainImages) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkGetSwapchainImagesKHR)
        return VK_ERROR_INITIALIZATION_FAILED;
    return disp->fp_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strcpy(perfMetrics.api, "Vulkan");
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;

    // Set flag to inform DXGI hooks that Vulkan is presenting on this thread.
    // This prevents double-drawing and incorrect API labeling when Vulkan
    // presents via DXGI.
    auto* shm = g_IPCClient.GetSharedMem();
    if (shm) {
        perfMetrics.sourceCapturePhase = shm->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = shm->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (shm->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags = shm->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (shm) {
        shm->runtimeState.vulkanPresentThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        shm->runtimeState.vulkanPresentTick.store(GetTickCount64(), std::memory_order_release);
    }

    const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
    bool isFirstHook = !g_InPresentHook;
    g_InPresentHook = true;

    SwapchainData* sd = nullptr;
    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {
        sd = VulkanLayerState::Get().GetSwapchainData(pPresentInfo->pSwapchains[0]);
    }

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceFromQueue(queue);
    VkDevice queueDevice = VulkanLayerState::Get().GetVkDeviceFromQueue(queue);

    bool asyncPresentDetected = false;
    if (sd) {
        const uint32_t acquireThreadId = sd->lastAcquireThreadId.load(std::memory_order_acquire);
        const uint64_t lastAcquireTick = sd->lastAcquireTick.load(std::memory_order_acquire);
        if (acquireThreadId != 0 && acquireThreadId != GetCurrentThreadId() && lastAcquireTick != 0 &&
            (GetTickCount64() - lastAcquireTick) < 2000ULL) {
            if (!sd->asyncPresentDetected.exchange(true, std::memory_order_acq_rel)) {
                LayerLog("Vulkan Layer: Async present detected for swapchain %p; moving limiter off present thread",
                         sd->swapchain);
            }
        }
        asyncPresentDetected = sd->asyncPresentDetected.load(std::memory_order_acquire);
    }
    if (!asyncPresentDetected && queueDevice != VK_NULL_HANDLE) {
        const uint32_t lastSubmitThreadId = VulkanLayerState::Get().GetLastSubmitThreadId(queueDevice);
        if (lastSubmitThreadId != 0 && lastSubmitThreadId != GetCurrentThreadId()) {
            asyncPresentDetected = true;
            if (sd) {
                sd->asyncPresentDetected.store(true, std::memory_order_release);
            }
        }
    }

    if (isFirstHook) {
        // For DXVK: FPS limiter runs in DX9 hook (game thread) instead.
        // This vkQueuePresent runs on DXVK's CS thread — blocking here
        // doesn't limit the game's actual render rate.
        if (!preferDX9Path && !asyncPresentDetected) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            g_SharedFpsLimiter.Apply();
        }

        // Apply CPU prerender limit - only if we have valid device and queue
        // tracking
        float prerenderLimit = VulkanLayerState::Get().GetPrerenderLimit();
        if (prerenderLimit >= 0.0f && !asyncPresentDetected && VulkanLayerState::Get().QueueSupportsGraphics(queue)) {
            if (queueDevice != VK_NULL_HANDLE && queue != VK_NULL_HANDLE) {
                ApplyPrerenderLimitVulkan(queueDevice, queue, prerenderLimit);
            }
        }
    }
    if (auto* perf = GetOverlayPerformanceMetrics(queueDevice)) {
        perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(perf->GetCurrentFPS() * 100.0f + 0.5f);
        perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(perf->Get1PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(perf->Get01PercentLowFPS() * 100.0f + 0.5f);
        perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(perf->GetWindowStdDev() + 0.5);
    }

    const VkSemaphore* currentWaitSemaphores =
        (pPresentInfo && pPresentInfo->waitSemaphoreCount > 0) ? pPresentInfo->pWaitSemaphores : nullptr;
    uint32_t currentWaitSemaphoreCount = pPresentInfo ? pPresentInfo->waitSemaphoreCount : 0;
    std::vector<VkSemaphore> chainedWaitSemaphores;
    bool modified = false;
    if (preferDX9Path) {
        static int dxvkPresentSkipLogCount = 0;
        if (dxvkPresentSkipLogCount < 6) {
            LayerLog("Vulkan Layer: DXVK d3d9 wrapper detected, skipping Vulkan present-time overlay only");
            dxvkPresentSkipLogCount++;
        }
    }

    if (g_LayerState.whitelisted && pPresentInfo && pPresentInfo->swapchainCount > 0) {
        if (sd) {
            uint32_t idx = pPresentInfo->pImageIndices[0];
            perfMetrics.sourceFrameIndex = idx + 1;

            // OPTIMIZATION: Keep overlay/capture queue work GPU-ordered while letting config choose whether
            // screenshots and capture happen before or after overlay submission.
            VkSemaphore overlayDone = GetOverlaySemaphore(sd->device, idx);
            OverlayConfig overlayCfg{};
            overlayCfg.captureIncludeOverlay = true;
            overlayCfg.screenshotIncludeOverlay = true;
            if (shm) {
                overlayCfg = shm->ReadOverlayConfig();
            }
            const bool overlayEnabled = !preferDX9Path && shm && overlayCfg.showOverlay;
            const bool captureRequested = shm && shm->runtimeState.IsInjectVideoCaptureRequested();
            const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
            const bool screenshotRequested = screenshotRequestId != 0;
            const bool captureAfterOverlay = captureRequested && overlayEnabled && overlayCfg.captureIncludeOverlay;
            const bool captureBeforeOverlay = captureRequested && !captureAfterOverlay;
            const bool screenshotAfterOverlay =
                screenshotRequested && overlayEnabled && overlayCfg.screenshotIncludeOverlay;
            const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;

            auto doOverlay = [&]() {
                if (!overlayEnabled)
                    return;

                // Measure ONLY the actual CPU overhead of overlay work.
                // Fence wait is tracked separately (it's GPU sync, not our overhead).
                int32_t fenceWaitUs = 0;
                int64_t overlayStartUs = PerfLogger::GetQpcUs();
                bool overlayRendered = RenderOverlay(sd->device, queue, idx, currentWaitSemaphores,
                                                     currentWaitSemaphoreCount, overlayDone, &fenceWaitUs);
                perfMetrics.overlayUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);
                perfMetrics.fenceWaitUs = fenceWaitUs;
                if (fenceWaitUs > 0 && perfMetrics.overlayUs > fenceWaitUs) {
                    perfMetrics.overlayUs -= fenceWaitUs;
                }
                if (overlayRendered) {
                    chainedWaitSemaphores.assign(1, overlayDone);
                    currentWaitSemaphores = chainedWaitSemaphores.data();
                    currentWaitSemaphoreCount = 1;
                    modified = true;
                }
            };

            auto doCapture = [&]() {
                if (!captureRequested || !shm)
                    return;
                if (shm->throttleCapture.load(std::memory_order_acquire)) {
                    return;
                }
                if (ShouldSkipCaptureForTargetCadence(shm, "Vulkan")) {
                    return;
                }

                int64_t captureStartUs = PerfLogger::GetQpcUs();
                VkSemaphore captureDone = GetCaptureSemaphore(sd->device, sd->swapchain, idx);
                if (captureDone == VK_NULL_HANDLE) {
                    // Initialization may have been deferred while the previous
                    // swapchain generation's media leases drained. Retry without
                    // blocking the present path.
                    InitializeCapture(sd->device, sd->swapchain, sd->format, sd->extent, sd->imageCount);
                    captureDone = GetCaptureSemaphore(sd->device, sd->swapchain, idx);
                }
                NoteCaptureSwapchainImagePresented(sd->device, sd->swapchain, idx);
                const bool captureSubmitted =
                    CaptureFrame(sd->device, sd->swapchain, queue, sd->images[idx], currentWaitSemaphores,
                                 currentWaitSemaphoreCount, captureDone);
                perfMetrics.captureUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - captureStartUs);
                // Capture may intentionally skip before queue submission (for
                // example while its non-blocking fence is busy). Preserve the
                // original Present wait chain unless captureDone was really signaled.
                if (captureSubmitted && captureDone != VK_NULL_HANDLE) {
                    chainedWaitSemaphores.assign(1, captureDone);
                    currentWaitSemaphores = chainedWaitSemaphores.data();
                    currentWaitSemaphoreCount = 1;
                    modified = true;
                }
            };

            auto doScreenshot = [&]() {
                if (!screenshotRequested || !shm)
                    return;
                bool waitsConsumed = false;
                DeviceDispatch* vkDisp = VulkanLayerState::Get().GetDeviceDispatch(sd->device);
                if (vkDisp && idx < sd->images.size()) {
                    waitsConsumed = TakeVulkanScreenshot(vkDisp, sd->device, queue, sd->images[idx], sd->extent.width,
                                                         sd->extent.height, sd->format, currentWaitSemaphores,
                                                         currentWaitSemaphoreCount, shm, screenshotRequestId);
                }
                if (waitsConsumed) {
                    currentWaitSemaphores = nullptr;
                    currentWaitSemaphoreCount = 0;
                    modified = true;
                } else {
                    CompleteScreenshotRequest(shm, screenshotRequestId, ScreenshotRequestStatus::Failed,
                                              ERROR_READ_FAULT);
                }
            };

            if (captureBeforeOverlay) {
                doCapture();
            }
            if (screenshotBeforeOverlay) {
                doScreenshot();
            }
            doOverlay();
            if (captureAfterOverlay) {
                doCapture();
            }
            if (screenshotAfterOverlay) {
                doScreenshot();
            }
        }
    }

    // Create modified PresentInfo with chained semaphore
    VkPresentInfoKHR presentInfoCopy;
    if (pPresentInfo && modified) {
        presentInfoCopy = *pPresentInfo;
        if (currentWaitSemaphores && currentWaitSemaphoreCount > 0) {
            presentInfoCopy.waitSemaphoreCount = currentWaitSemaphoreCount;
            presentInfoCopy.pWaitSemaphores = currentWaitSemaphores;
        } else {
            // If we have no wait semaphore (e.g. game didn't provide one and we
            // didn't add one), we must ensure we don't pass garbage.
            presentInfoCopy.waitSemaphoreCount = 0;
            presentInfoCopy.pWaitSemaphores = nullptr;
        }
    }

    VkResult res = VK_SUCCESS;
    if (disp && disp->fp_vkQueuePresentKHR) {
        res = disp->fp_vkQueuePresentKHR(queue, (pPresentInfo && modified) ? &presentInfoCopy : pPresentInfo);
    }

    if (isFirstHook)
        g_InPresentHook = false;

    if (shm)
        shm->runtimeState.vulkanPresentThreadId.store(0, std::memory_order_release);

    // Log performance metrics
    if (PerfLogger::Get().IsEnabled()) {
        perfMetrics.totalUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - perfMetrics.qpcUs);
        PerfLogger::Get().LogFrame(perfMetrics);
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                             uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                                                             uint32_t* pImageIndex) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkAcquireNextImageKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    SwapchainData* sd = VulkanLayerState::Get().GetSwapchainData(swapchain);
    const bool preferDX9Path = IsDXVKD3D9WrapperLoaded() && !IsDXVKD3D11WrapperLoaded();
    if (sd) {
        sd->lastAcquireThreadId.store(GetCurrentThreadId(), std::memory_order_release);
        sd->lastAcquireTick.store(GetTickCount64(), std::memory_order_release);
        if (sd->asyncPresentDetected.load(std::memory_order_acquire) && !preferDX9Path) {
            g_SharedFpsLimiter.SetIPCClient(&g_IPCClient);
            g_SharedFpsLimiter.Apply();
        }
    }

    return disp->fp_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator, VkSampler* pSampler) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateSampler || !pCreateInfo)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkSamplerCreateInfo modified = *pCreateInfo;
    if (g_LayerState.whitelisted) {
        auto& state = VulkanLayerState::Get();

        bool specialReduction = false;
        for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(modified.pNext); node;
             node = node->pNext) {
            if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO) {
                const auto* reduction = reinterpret_cast<const VkSamplerReductionModeCreateInfo*>(node);
                specialReduction = reduction->reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
                break;
            }
        }
        const auto isMaterialAddress = [](VkSamplerAddressMode mode) {
            return mode == VK_SAMPLER_ADDRESS_MODE_REPEAT || mode == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        };
        const bool materialAddress = isMaterialAddress(modified.addressModeU) &&
                                     isMaterialAddress(modified.addressModeV) &&
                                     isMaterialAddress(modified.addressModeW);
        const bool borderAddress = modified.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                   modified.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                                   modified.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        const bool mipmapped = modified.maxLod > 0.0f && modified.minLod < modified.maxLod;
        const bool linearMinMag = modified.minFilter == VK_FILTER_LINEAR && modified.magFilter == VK_FILTER_LINEAR;
        const bool standardMinMag =
            (modified.minFilter == VK_FILTER_NEAREST || modified.minFilter == VK_FILTER_LINEAR) &&
            (modified.magFilter == VK_FILTER_NEAREST || modified.magFilter == VK_FILTER_LINEAR);
        const bool overridesAllowed = mipmapped && modified.compareEnable == VK_FALSE && !specialReduction &&
                                      !borderAddress && modified.unnormalizedCoordinates == VK_FALSE &&
                                      standardMinMag &&
                                      (state.IsAggressiveSamplerOverride() || (materialAddress && linearMinMag));

        if (overridesAllowed) {
            // Anisotropic filtering override
            if (state.IsAnisotropyOverrideActive()) {
                uint32_t maxAniso = state.GetMaxAnisotropy();
                if (maxAniso <= 1) {
                    // "off" - disable anisotropic filtering
                    modified.anisotropyEnable = VK_FALSE;
                    modified.maxAnisotropy = 1.0f;
                } else if (disp->samplerAnisotropyEnabled) {
                    modified.anisotropyEnable = VK_TRUE;
                    modified.maxAnisotropy =
                        std::min(static_cast<float>(maxAniso), std::max(1.0f, disp->maxSamplerAnisotropy));
                } else {
                    static std::atomic<int> s_anisotropyFeatureLogCount{0};
                    if (s_anisotropyFeatureLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                        LayerLog(
                            "Vulkan sampler: forced AF skipped because samplerAnisotropy was not enabled at "
                            "vkCreateDevice");
                    }
                }
            }

            const char* mipMapping = state.GetMipMapping();
            if (strcmp(mipMapping, "trilinear") == 0) {
                modified.minFilter = VK_FILTER_LINEAR;
                modified.magFilter = VK_FILTER_LINEAR;
                modified.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            } else if (strcmp(mipMapping, "bilinear") == 0) {
                modified.minFilter = VK_FILTER_LINEAR;
                modified.magFilter = VK_FILTER_LINEAR;
                modified.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            }

            // Mip bias override with mode support
            if (state.IsForceMipBiasClampEnabled()) {
                modified.mipLodBias = 0.0f;
            } else if (state.IsMipBiasOverrideActive()) {
                float userBias = state.GetMipLodBias();
                const char* mode = state.GetMipBiasMode();
                float originalBias = pCreateInfo->mipLodBias;

                if (strcmp(mode, "offset") == 0) {
                    modified.mipLodBias = originalBias + userBias;
                } else if (strcmp(mode, "base") == 0) {
                    if (originalBias >= 0.0f)
                        modified.mipLodBias = originalBias;
                    else
                        modified.mipLodBias = originalBias + userBias;
                } else {
                    // "strict" - absolute override
                    modified.mipLodBias = userBias;
                }

                const float maxBias = disp->maxSamplerLodBias > 0.0f ? disp->maxSamplerLodBias : 16.0f;
                modified.mipLodBias = std::clamp(modified.mipLodBias, -maxBias, maxBias);
            }
        }
    }
    const bool changed = std::memcmp(&modified, pCreateInfo, sizeof(modified)) != 0;
    VkResult result = disp->fp_vkCreateSampler(device, &modified, pAllocator, pSampler);
    if (result != VK_SUCCESS && changed) {
        static std::atomic<int> s_fallbackLogCount{0};
        if (s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            LayerLog("Vulkan sampler: overridden descriptor failed result=%d; retrying original transactionally",
                     static_cast<int>(result));
        }
        result = disp->fp_vkCreateSampler(device, pCreateInfo, pAllocator, pSampler);
    }
    return result;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR
VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateWin32SurfaceKHR(VkInstance instance,
                                                               const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks* pAllocator,
                                                               VkSurfaceKHR* pSurface) {
    InstanceDispatch* disp = VulkanLayerState::Get().GetInstanceDispatch(instance);
    if (!disp || !disp->fp_vkCreateWin32SurfaceKHR)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = disp->fp_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    if (res == VK_SUCCESS) {
        VulkanLayerState::Get().RegisterSurface(*pSurface, pCreateInfo->hwnd);
    }
    return res;
}
#endif
