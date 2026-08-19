#pragma once

// Shared state between the overlay's lifecycle unit (layer_overlay.cpp) and its
// per-present recording/submission unit (layer_overlay_render.cpp). Both are
// halves of one logical translation unit; the split exists only to keep each
// file under the source-size ceiling.

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/custom_overlay_vk.h"  // For VulkanBackend access
#include "../common/input_manager.h"
#include "../common/ipc_client.h"
#include "../common/overlay_adapter.h"
#include "../common/overlay_metrics_publisher.h"
#include "../common/perf_logger.h"
#include "../common/performance_metrics.h"
#include "layer_main.h"
#include "overlay_submit_queue_policy.h"
#include "vulkan_layer.h"

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
    std::vector<VkSemaphore> preSignaledSemaphores;  // Always-signaled semaphores for skipped frames
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent = {0, 0};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    PerformanceMetrics* metrics = nullptr;
    bool needsWindowHook = false;
    uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    // GPU-side cost of the overlay's own command buffer. Two timestamps per
    // image index, read back without blocking after the fence for that index
    // has already been waited on, so the result is last time this image came
    // round. CPU timings alone cannot tell an expensive overlay from an
    // expensively scheduled one.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    float timestampPeriodNs = 0.0f;
    std::vector<bool> timestampWritten;
    int32_t lastOverlayGpuUs = 0;

    // OverlayAdapter for content rendering
    OverlayAdapter* overlayAdapter = nullptr;
};

extern std::mutex g_OverlayMutex;
extern std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

void SyncOverlayActiveFlagLocked();
bool RecreateOverlayCommandResources(OverlayState& state, DeviceDispatch* disp, uint32_t queueFamilyIndex);
