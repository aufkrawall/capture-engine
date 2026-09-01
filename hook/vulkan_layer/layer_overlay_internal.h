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

struct ComputePresentDiagnostics {
    uint64_t frames = 0;
    uint64_t commandCacheHits = 0;
    uint64_t phaseSamples = 0;
    uint64_t graphicsRecordUs = 0;
    uint64_t graphicsSubmitUs = 0;
    uint64_t computeSubmitUs = 0;
    uint64_t commandRecordMisses = 0;
    uint64_t computeRecordMissUs = 0;
};

// Overlay state per device - manages Vulkan frame resources
struct OverlayState {
    bool initialized = false;
    bool deviceLost = false;
    VkDevice device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> semaphores;
    // Per slot: the swapchain image its semaphore was presented with, and that
    // image's acquire generation at the time. A slot may only be reused once
    // the generation has moved on, because the fence proves the submission
    // retired and says nothing about the present that waits on the semaphore.
    std::vector<uint32_t> slotImageIndex;
    std::vector<uint64_t> slotAcquireGeneration;
    std::vector<uint8_t> slotEverUsed;
    // Per swapchain image: the acquire generation the overlay was last
    // composited into it at. A frame-generation runtime may present one image
    // again without the application re-acquiring it, and an application may not
    // alter a presented image before it does, so an unchanged generation proves
    // the overlay CE already blended in is still there. Compositing a second
    // time would blend the overlay's own alpha onto itself and show a more
    // opaque panel on those presents.
    std::vector<uint64_t> imageCompositeAcquireGeneration;
    uint64_t repeatPresentComposites = 0;
    uint32_t nextSubmissionSlot = 0;
    uint64_t submissionBackpressureWaits = 0;
    uint64_t submissionRingGrowths = 0;
    // Slots created up front. The timestamp query pool covers exactly these, so
    // a slot added later reports no GPU time rather than recreating the pool on
    // the present path.
    uint32_t timestampSlotCapacity = 0;
    // Growing the ring means one more command buffer, fence and semaphore - and,
    // while the compute-composite route is in use, that route's own per-slot
    // resources too. A slot without them would have to fall back to the direct
    // render-pass route for its presents, which is exactly the compute ->
    // graphics -> compute round trip the compositor exists to remove, and which
    // alternates composite routes from present to present on a compute-only
    // present queue. Growth is therefore all-or-nothing: see
    // AppendComputePresentSlot.
    bool submissionRingMayGrow = false;
    std::vector<VkSemaphore> preSignaledSemaphores;  // Always-signaled semaphores for skipped frames
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
    std::vector<VkImage> swapchainImages;
    VkExtent2D extent = {0, 0};
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkImageUsageFlags imageUsage = 0;
    bool storageFormatReadWithoutFormatSupported = false;
    bool storageFormatWriteWithoutFormatSupported = false;
    PerformanceMetrics* metrics = nullptr;
    bool needsWindowHook = false;
    uint32_t queueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    // GPU-side cost of the overlay's own command buffer. Two timestamps per
    // submission slot, read back without blocking after the fence for that slot
    // has already been waited on. CPU timings alone cannot tell an expensive overlay from an
    // expensively scheduled one.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    float timestampPeriodNs = 0.0f;
    std::vector<bool> timestampWritten;
    int32_t lastOverlayGpuUs = 0;

    // OverlayAdapter for content rendering
    OverlayAdapter* overlayAdapter = nullptr;

    // Present-from-compute route. The graphics queue renders the small overlay
    // concurrently into these transparent images; the present queue then
    // composites only the occupied rectangle onto its storage-capable
    // swapchain image. This removes the compute -> graphics -> compute round
    // trip from the present dependency chain.
    bool computePresentInitialized = false;
    bool computePresentUnavailable = false;
    uint32_t computeQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t computeGraphicsQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;
    std::vector<VkImage> offscreenImages;
    std::vector<VkDeviceMemory> offscreenMemory;
    std::vector<VkImageView> offscreenImageViews;
    std::vector<VkFramebuffer> offscreenFramebuffers;
    std::vector<VkSemaphore> offscreenReadySemaphores;
    // One full-resolution offscreen target per submission slot is the real
    // cost of this route, so a session can see it grow with the ring.
    uint64_t offscreenMemoryBytes = 0;
    uint64_t offscreenSlotBytes = 0;
    VkCommandPool computeCommandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> computeCommandBuffers;
    std::vector<ce::overlay_submit_queue_policy::ComputeCompositeBounds> computeCommandBounds;
    std::vector<uint8_t> computeCommandRecorded;
    std::vector<VkSemaphore> computeWaitSemaphores;
    std::vector<VkPipelineStageFlags> computeWaitStages;
    VkDescriptorSetLayout computeDescriptorSetLayout = VK_NULL_HANDLE;
    // One pool per submission slot, so extending the ring only adds a pool
    // instead of having to reallocate every existing descriptor set.
    std::vector<VkDescriptorPool> computeDescriptorPools;
    std::vector<VkDescriptorSet> computeDescriptorSets;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkSampler computeSampler = VK_NULL_HANDLE;
    ComputePresentDiagnostics computePresentDiagnostics;
};

extern std::mutex g_OverlayMutex;
extern std::unordered_map<VkDevice, OverlayState> g_OverlayStates;

void SyncOverlayActiveFlagLocked();
bool RecreateOverlayCommandResources(OverlayState& state, DeviceDispatch* disp, uint32_t queueFamilyIndex);
void CleanupComputePresentOverlay(OverlayState& state, DeviceDispatch* disp);
// Extends the compute-composite route by exactly one submission slot. Called
// while the ring grows so every slot keeps a complete composite route.
bool AppendComputePresentSlot(OverlayState& state, DeviceDispatch* disp);
bool RenderComputePresentOverlay(OverlayState& state, DeviceDispatch* disp, const OverlaySubmitTarget& graphicsTarget,
                                 VkQueue presentQueue, uint32_t submissionSlot, uint32_t imageIndex,
                                 const VkSemaphore* waitSemaphores,
                                 uint32_t waitSemaphoreCount, VkSemaphore signalSemaphore, bool* routeAttempted);
