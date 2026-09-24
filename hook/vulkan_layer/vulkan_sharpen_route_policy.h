#pragma once

#include <cstdint>

// Which pipeline the Vulkan sharpen pass may run on a given present queue, and
// when its per-swapchain state has to be rebuilt.
//
// The pass submits on the queue the application presents from. A game may
// present from a queue family without VK_QUEUE_GRAPHICS_BIT - DOOM Eternal's
// "present from compute" moves its present from family 0 to the compute-only
// family 2 in the middle of a running swapchain. A render pass recorded from a
// family-0 command pool is invalid on that queue twice over: the pool belongs to
// another family, and the family cannot execute graphics work at all. Session
// `20260923_003755` submitted it anyway and lost the device two seconds later.
//
// A compute-only present queue gets the compute variant of the same kernel,
// writing the presentable image as a formatless storage image - the route the
// compute-present overlay composite already uses, for the same reason: it keeps
// the work on the engine the game presents from instead of sending the present
// dependency back to the graphics queue.

namespace ce::vulkan_sharpen_route {

enum class Route : uint8_t {
    kNone,
    kGraphics,
    kCompute,
};

struct Input {
    bool queueFamilyKnown = false;
    bool queueSupportsGraphics = false;
    bool queueSupportsCompute = false;
    // The swapchain was created with VK_IMAGE_USAGE_STORAGE_BIT.
    bool swapchainHasStorageUsage = false;
    // shaderStorageImageWriteWithoutFormat on the device and
    // STORAGE_WRITE_WITHOUT_FORMAT for the swapchain format.
    bool storageWriteWithoutFormat = false;
};

constexpr Route Choose(const Input& input) {
    if (!input.queueFamilyKnown)
        return Route::kNone;
    if (input.queueSupportsGraphics)
        return Route::kGraphics;
    if (input.queueSupportsCompute && input.swapchainHasStorageUsage && input.storageWriteWithoutFormat)
        return Route::kCompute;
    return Route::kNone;
}

// Logged when Choose refuses, so a session says why a present was not filtered.
constexpr const char* RefusalReason(const Input& input) {
    if (!input.queueFamilyKnown)
        return "present_queue_family_unknown";
    if (!input.queueSupportsCompute)
        return "present_queue_has_no_graphics_or_compute";
    if (!input.swapchainHasStorageUsage)
        return "compute_present_without_storage_usage";
    if (!input.storageWriteWithoutFormat)
        return "compute_present_format_not_storage_writable";
    return "none";
}

// What the pass's command pool, pipelines and per-image objects were built for.
struct Identity {
    uint64_t swapchain = 0;
    int32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
    uint32_t queueFamily = 0;
    Route route = Route::kNone;
};

// Any difference invalidates the state. The queue family is part of it because
// command buffers may only be submitted to the family their pool was created
// for, and a game may move its present between families without recreating the
// swapchain.
constexpr bool MustRebuild(const Identity& built, const Identity& current) {
    return built.swapchain != current.swapchain || built.format != current.format || built.width != current.width ||
           built.height != current.height || built.imageCount != current.imageCount ||
           built.queueFamily != current.queueFamily || built.route != current.route;
}

// The pass keeps ONE state per device while a device may own several live
// swapchains. A present of a swapchain the state was not built over must skip
// the pass entirely: feeding it to MustRebuild instead destroyed and rebuilt
// the whole pipeline on every present of the second chain - on the present
// thread, with up to kSharpenSlotCount one-second fence waits in the teardown.
// The first swapchain to present owns the pass; the destroy hook (or an
// `oldSwapchain` retirement) releases the state and re-arms the choice, which
// is also what keeps a handle-reusing recreate honest.
constexpr bool MustSkipUnownedSwapchain(bool stateInitialized, uint64_t builtSwapchain, uint64_t presentSwapchain) {
    return stateInitialized && builtSwapchain != presentSwapchain;
}

// The overlay's runtime-eligibility floor, applied to the filter as well.
// Decide's own minimum is only 32 px; below this floor a swapchain is a tiny
// auxiliary surface and must not build the full pipeline for it.
constexpr uint32_t kMinimumTargetWidth = 320;
constexpr uint32_t kMinimumTargetHeight = 180;

constexpr bool MeetsMinimumTargetSize(uint32_t width, uint32_t height) {
    return width >= kMinimumTargetWidth && height >= kMinimumTargetHeight;
}

inline const char* RouteName(Route route) {
    switch (route) {
        case Route::kGraphics:
            return "graphics";
        case Route::kCompute:
            return "compute";
        case Route::kNone:
        default:
            return "none";
    }
}

}  // namespace ce::vulkan_sharpen_route
