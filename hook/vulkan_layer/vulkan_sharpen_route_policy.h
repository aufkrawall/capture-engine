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
