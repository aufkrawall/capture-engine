#pragma once

#include <mutex>
#include <vector>

#include "../../common/sharpen_policy.h"
#include "../common/sharpen_constants.h"
#include "../common/sharpen_pass_log.h"
#include "layer_main.h"
#include "layer_sharpen.h"
#include "vulkan_layer.h"
#include "vulkan_sharpen_route_policy.h"
#include "vulkan_sharpen_state_registry.h"

// Shared state between the sharpen pass's lifecycle unit
// (layer_sharpen_setup.cpp) and its per-present recording unit
// (layer_sharpen.cpp). Both are halves of one logical translation unit; the
// split exists only to keep each file under the source-size ceiling.

// Command buffers and their fences ring. The source copy does not: every
// submission goes to one queue and each frame's copy barrier waits on the
// previous frame's fragment-shader read of the same image, so one copy is
// enough no matter how deep the ring is.
inline constexpr uint32_t kSharpenSlotCount = 3;

struct SharpenState {
    bool initialized = false;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t queueFamily = 0;
    // Graphics: a render pass into the presentable image. Compute: a dispatch
    // writing it as a storage image, for compute-only present queues. The two
    // routes share the source copy, sampler, image views, semaphores and
    // command ring; the objects below that differ are noted per field.
    ce::vulkan_sharpen_route::Route route = ce::vulkan_sharpen_route::Route::kNone;
    // Cached answer to "may a shader write this format without a format
    // qualifier", for the format it was asked about.
    VkFormat storageQueryFormat = VK_FORMAT_UNDEFINED;
    bool storageWritable = false;

    // Graphics route only.
    VkRenderPass renderPass = VK_NULL_HANDLE;
    // Graphics: the source sampler. Compute: the source sampler plus the
    // presentable image as a storage image, one set per image.
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    // Graphics route: the one set every image shares.
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    // Compute route: indexed by presentable image.
    std::vector<VkDescriptorSet> imageDescriptorSets;
    VkSampler sampler = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    // Graphics or compute pipelines, matching `route`.
    VkPipeline casPipeline = VK_NULL_HANDLE;
    VkPipeline rcasPipeline = VK_NULL_HANDLE;

    // The untouched copy of the frame the kernel reads.
    VkImage sourceImage = VK_NULL_HANDLE;
    VkDeviceMemory sourceMemory = VK_NULL_HANDLE;
    VkImageView sourceView = VK_NULL_HANDLE;
    // False until the first copy has written it; before that its contents are
    // undefined and its layout must be transitioned from UNDEFINED.
    bool sourceInitialized = false;

    // Per presentable image.
    std::vector<VkImageView> imageViews;
    // Graphics route only.
    std::vector<VkFramebuffer> framebuffers;
    // Indexed by presentable image, not by slot: reacquiring an image proves
    // the present that waited on its semaphore consumed it, which a fence on
    // CE's own submission never does. This is the same rule inject capture uses.
    std::vector<VkSemaphore> imageSemaphores;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers[kSharpenSlotCount] = {};
    VkFence fences[kSharpenSlotCount] = {};
    bool slotSubmitted[kSharpenSlotCount] = {};
    uint32_t nextSlot = 0;

    ce::sharpen::DecisionLogGate logGate;
};

// Non-dispatchable handles are pointers only on 64-bit builds.
inline uint64_t SharpenSwapchainKey(VkSwapchainKHR swapchain) {
#if (VK_USE_64_BIT_PTR_DEFINES == 1)
    return reinterpret_cast<uint64_t>(swapchain);
#else
    return static_cast<uint64_t>(swapchain);
#endif
}

// Per-image semaphores taken out of a destroyed SharpenState. The present of
// that image may still be waiting on one, and nothing but the destruction of
// the swapchain it was presented against proves otherwise.
struct DeferredSharpenSemaphores {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkSemaphore> semaphores;
};

extern std::mutex layer_sharpen_g_StateMutex;
// One state per (device, swapchain), plus the retired ones awaiting their
// fences. Guarded by layer_sharpen_g_StateMutex.
extern ce::vulkan_sharpen_registry::Registry<SharpenState, VkDevice> layer_sharpen_g_Registry;
// Guarded by layer_sharpen_g_StateMutex.
extern std::vector<DeferredSharpenSemaphores> layer_sharpen_g_DeferredSemaphores;

// Destroys the deferred batches that `destroyedSwapchain`'s destruction (or the
// device's) has made safe. Caller holds layer_sharpen_g_StateMutex.
void DrainDeferredSharpenSemaphoresLocked(VkDevice device, VkSwapchainKHR destroyedSwapchain, bool deviceTeardown);

// Builds everything that depends on the device, the swapchain format/extent and
// the presentable images. Returns false when any of it could not be created, in
// which case nothing is left half-built.
//
// DestroySharpenState never destroys `imageSemaphores`: it hands them to
// layer_sharpen_g_DeferredSemaphores tagged with the state's swapchain. Caller
// holds layer_sharpen_g_StateMutex for both.
bool InitializeSharpenState(SharpenState& state, DeviceDispatch* disp, VkDevice device, VkSwapchainKHR swapchain,
                            VkFormat format, VkExtent2D extent, uint32_t queueFamily,
                            ce::vulkan_sharpen_route::Route route, uint32_t imageCount, const VkImage* images);

// What `state` was built for, in the terms MustRebuild compares.
ce::vulkan_sharpen_route::Identity SharpenStateIdentity(const SharpenState& state);

// Compute route (layer_sharpen_compute.cpp): the descriptor sets and pipelines,
// built over image views and a source view that already exist.
bool CreateSharpenComputeObjects(SharpenState& state, DeviceDispatch* disp);

// Records the compute route's copy, dispatch and layout transitions into `cmd`.
void RecordSharpenCompute(SharpenState& state, DeviceDispatch* disp, VkCommandBuffer cmd, VkImage image,
                          uint32_t imageIndex, VkPipeline pipeline, const ce::sharpen::ShaderConstants& constants);

void DestroySharpenState(SharpenState& state, DeviceDispatch* disp);

// True when every submission `state` made has signalled its fence, i.e. a
// DestroySharpenState now would not wait. Never blocks.
bool SharpenStateSubmissionsRetired(const SharpenState& state, DeviceDispatch* disp);
