/**
 * Vulkan Layer - Core Implementation
 *
 * Consolidates all Vulkan hooks and state management.
 */

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_layer.h"
#include "vulkan_sampler_policy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "../../common/mip_mapping_policy.h"
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
