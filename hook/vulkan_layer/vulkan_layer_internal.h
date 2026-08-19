#pragma once

struct FrameLimitState;

#define VK_USE_PLATFORM_WIN32_KHR

#include "vulkan_layer.h"

#include "vulkan_sampler_policy.h"

#include "vulkan_swapchain_image_policy.h"

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

void PopulateInstanceDispatch(InstanceDispatch* dispatch, VkInstance instance, PFN_vkGetInstanceProcAddr gipa);

void PopulateDeviceDispatch(DeviceDispatch* dispatch, VkDevice device, PFN_vkGetDeviceProcAddr gdpa);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);

VKAPI_ATTR void VKAPI_CALL Capture_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);

VKAPI_ATTR void VKAPI_CALL Capture_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSampler* pSampler);

struct FrameLimitState {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<VkFence> fences;
    std::vector<bool> submitted;
    uint64_t frameIndex = 0;
    uint32_t lookback = UINT32_MAX;
};

inline std::mutex vulkan_layer_g_FrameLimitMutex;

inline std::unordered_map<VkQueue, FrameLimitState> vulkan_layer_g_FrameLimitStates;

// Both wrapper probes are read on the present path, and the expensive half of
// `IsDllFromProject` is a version-resource read off disk. The answer can only
// change when the module behind the name changes, so cache it against the
// HMODULE: `GetModuleHandleA` is the cheap half and it catches a late load, an
// unload and a reload alike. One atomic word holds both, because an HMODULE is
// page-aligned and therefore has bit 0 free; that keeps the pair impossible to
// tear, which two separate atomics would not.
inline constexpr uintptr_t kWrapperProbeNotEvaluated = ~static_cast<uintptr_t>(0);

inline bool IsDllFromProjectCached(const char* dllName, const char* versionNeedle,
                                   std::atomic<uintptr_t>& cachedState) {
    const HMODULE module = GetModuleHandleA(dllName);
    const uintptr_t moduleBits = reinterpret_cast<uintptr_t>(module);
    const uintptr_t cached = cachedState.load(std::memory_order_acquire);
    if (cached != kWrapperProbeNotEvaluated && (cached & ~static_cast<uintptr_t>(1)) == moduleBits) {
        return (cached & 1u) != 0;
    }
    const bool answer = module != nullptr && IsDllFromProject(dllName, versionNeedle);
    cachedState.store(moduleBits | (answer ? 1u : 0u), std::memory_order_release);
    return answer;
}

inline std::atomic<uintptr_t> vulkan_layer_g_DXVKD3D9ProbeState{kWrapperProbeNotEvaluated};
inline std::atomic<uintptr_t> vulkan_layer_g_DXVKD3D11ProbeState{kWrapperProbeNotEvaluated};

inline bool IsDXVKD3D9WrapperLoaded() {
    return IsDllFromProjectCached("d3d9.dll", "dxvk", vulkan_layer_g_DXVKD3D9ProbeState);
}

// Returns true when d3d11.dll is a DXVK wrapper (outside System32, version info contains "dxvk").
// Used alongside IsDXVKD3D9WrapperLoaded() to distinguish pure DX9-DXVK games
// (only d3d9 from DXVK) from DX10/11-DXVK games where d3d9.dll is also present.
inline bool IsDXVKD3D11WrapperLoaded() {
    return IsDllFromProjectCached("d3d11.dll", "dxvk", vulkan_layer_g_DXVKD3D11ProbeState);
}

inline void ApplyPrerenderLimitVulkan(VkDevice device, VkQueue queue, float limit) {
    if (limit < 0.0f)
        return;
    if (queue == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
        return;

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (!disp || !disp->fp_vkCreateFence || !disp->fp_vkDestroyFence || !disp->fp_vkWaitForFences ||
        !disp->fp_vkResetFences || !disp->fp_vkQueueSubmit) {
        return;
    }

    std::lock_guard<std::mutex> lock(vulkan_layer_g_FrameLimitMutex);
    FrameLimitState& state = vulkan_layer_g_FrameLimitStates[queue];
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
            vulkan_layer_g_FrameLimitStates.erase(queue);
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
    ScopedBorrowedQueueSubmission prerenderSubmissionGuard(queue);
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

inline void CleanupPrerenderFences(VkDevice device) {
    std::lock_guard<std::mutex> lock(vulkan_layer_g_FrameLimitMutex);

    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkDestroyFence) {
        for (auto it = vulkan_layer_g_FrameLimitStates.begin(); it != vulkan_layer_g_FrameLimitStates.end();) {
            if (it->second.device != device) {
                ++it;
                continue;
            }
            for (VkFence fence : it->second.fences)
                disp->fp_vkDestroyFence(device, fence, nullptr);
            it = vulkan_layer_g_FrameLimitStates.erase(it);
        }
        LayerLog("Vulkan Prerender: cleaned up device fence rings");
    }
}

extern // Reentrancy guard shared with other hooks (defined here for the layer)
thread_local bool g_InPresentHook;
