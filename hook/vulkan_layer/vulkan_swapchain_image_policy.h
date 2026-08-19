#pragma once

#include <cstdint>

// Ownership rules for CE's `[Graphics] backbuffer_count` on a VkSwapchainKHR.
//
// A Vulkan swapchain is an explicit-acquire flip chain: the application decides
// how many images it holds at once, and the spec only guarantees forward
// progress for a blocking vkAcquireNextImageKHR while
//   acquiredImages <= swapchainImageCount - VkSurfaceCapabilitiesKHR::minImageCount.
// The application's own `minImageCount` request is therefore not a preference,
// it is the declaration of how deep its acquire pipeline is. Asking the driver
// for fewer images than the application asked for silently removes that
// headroom, and the application has no way to notice: it queries the actual
// count, sizes its rings to it, and then trips over the acquire limit.
//
// DOOM Eternal session `20260819_033816` is that failure. With "present from
// compute" off the game requests minImageCount=3 and keeps two images acquired;
// CE forced 3 -> 2, NVIDIA's WSI answered the second blocking acquire with
// VK_NOT_READY instead of deadlocking, and the game's own error path fatal-
// errored with "vkAcquireNextImageKHR failed with error (VK_NOT_READY)". With
// "present from compute" on the same game requests 2, the override was a no-op,
// and the session ran fine - which is exactly why the crash looked like an
// async-present problem rather than an image-count one.
//
// This mirrors the rule CE already applies on D3D: a flip-model BufferCount
// reduction that would violate the game's own allocation is skipped and stays
// physical-count preserving (`ApplyDX11BackbufferCountOverride`,
// `hook/apis/dx11_hook_helpers.cpp`). Vulkan is strictly more sensitive because
// the acquire limit is part of the API contract, so the same rule is mandatory
// here, not merely prudent.

namespace ce::vulkan_swapchain_image_policy {

struct Input {
    // [Graphics] backbuffer_count as published to the layer; anything below 2
    // means "no override" (-1 is the configured default).
    int32_t configuredBackbufferCount = -1;
    // VkSwapchainCreateInfoKHR::minImageCount as the application supplied it.
    uint32_t applicationMinImageCount = 0;
    // VkSurfaceCapabilitiesKHR for the surface being created against. When the
    // capabilities cannot be queried CE must not raise the count blindly: an
    // over-maximum request fails swapchain creation outright, which is a harder
    // failure than declining the override.
    bool surfaceCapabilitiesKnown = false;
    uint32_t surfaceMinImageCount = 0;
    // 0 means "no maximum" per VkSurfaceCapabilitiesKHR.
    uint32_t surfaceMaxImageCount = 0;
};

struct Decision {
    // The value the layer passes to vkCreateSwapchainKHR.
    uint32_t minImageCount = 0;
    // True when `minImageCount` differs from what the application requested.
    bool overrideApplied = false;
    // True when the configured count was below the application's request and was
    // therefore not applied.
    bool reductionSkipped = false;
    // True when the configured count exceeded the surface maximum.
    bool clampedToSurfaceMaximum = false;
    // True when the override was declined because the surface capabilities were
    // not available to validate it against.
    bool skippedUnknownCapabilities = false;
};

inline Decision Decide(const Input& input) {
    Decision decision = {};
    decision.minImageCount = input.applicationMinImageCount;

    if (input.configuredBackbufferCount < 2)
        return decision;

    if (!input.surfaceCapabilitiesKnown) {
        decision.skippedUnknownCapabilities = true;
        return decision;
    }

    uint32_t desired = static_cast<uint32_t>(input.configuredBackbufferCount);
    if (desired < input.applicationMinImageCount) {
        // The reduction the user asked for would eat the application's acquire
        // headroom. Keep the application's own count.
        desired = input.applicationMinImageCount;
        decision.reductionSkipped = true;
    }
    if (desired < input.surfaceMinImageCount)
        desired = input.surfaceMinImageCount;
    if (input.surfaceMaxImageCount != 0 && desired > input.surfaceMaxImageCount) {
        desired = input.surfaceMaxImageCount;
        decision.clampedToSurfaceMaximum = true;
    }
    // Clamping to the surface maximum must never become a back door for the
    // reduction this policy exists to prevent: if the application itself asked
    // for more than the surface reports, that is the application's own request
    // and the driver answers it exactly as it would without CE in the chain.
    if (desired < input.applicationMinImageCount) {
        desired = input.applicationMinImageCount;
        decision.clampedToSurfaceMaximum = false;
    }

    decision.minImageCount = desired;
    decision.overrideApplied = (desired != input.applicationMinImageCount);
    return decision;
}

}  // namespace ce::vulkan_swapchain_image_policy
