#pragma once

#include <cstdint>

// When CE's overlay objects built over a swapchain's presentable images have to
// be released.
//
// The overlay creates one VkImageView per presentable image and a VkFramebuffer
// over each of those views, and the compute-composite route writes those same
// views into its descriptor sets and bakes the images into its cached command
// buffers. Every one of those objects is derived from images the swapchain
// owns: `vkDestroySwapchainKHR` destroys the presentable images with it, so
// anything still referencing them afterwards refers to freed allocations.
//
// CE used to release them at the *next* `vkCreateSwapchainKHR` instead
// ("InitializeOverlay - Existing state found, cleaning it before re-init").
// That is one swapchain generation too late: between the application's destroy
// and its next create, CE holds views over images the driver has already freed,
// and the release itself then hands those stale views back to the driver.
//
// DOOM Eternal session `20260913_174040` is that failure. The game destroyed
// its startup swapchain at 17:41:45.923 and created the replacement at .927;
// CE tore its state down at .942-.951, and nvlddmkm logged event 153 ("Error
// occurred on GPUID") at .9535. The first present on the new swapchain then
// returned VK_ERROR_DEVICE_LOST from the prerender fence wait and the game
// window stayed black for the rest of the process lifetime. The second launch
// of the same build ran the identical sequence without faulting, which is what
// a use-after-free looks like: whether the freed allocation has been recycled
// yet decides the outcome.
//
// The rule is therefore ordering, not cleanup policy: CE releases the overlay
// state it built over a swapchain *before* letting `vkDestroySwapchainKHR`
// reach the driver. An application that retires a swapchain through
// `VkSwapchainCreateInfoKHR::oldSwapchain` is already correct under the same
// rule, because the create-time release runs while the old swapchain is only
// retired and its images are still alive.

namespace ce::overlay_swapchain_lifetime {

struct Input {
    // True when CE holds a live overlay state for the device being acted on.
    bool overlayStateExists = false;
    // The swapchain that state's image views and framebuffers were built over.
    uint64_t overlayStateSwapchain = 0;
    // The swapchain the application is destroying.
    uint64_t destroyedSwapchain = 0;
    // CE already latched a device loss for this device. A lost device answers
    // no wait, so the release must not ask it to idle first.
    bool deviceLost = false;
};

struct Decision {
    // Release CE's swapchain-derived overlay objects before the driver destroys
    // the swapchain.
    bool release = false;
    // Wait for CE's own submissions to retire before destroying them.
    bool waitForIdle = false;
};

constexpr Decision Decide(const Input& input) {
    Decision decision = {};
    if (!input.overlayStateExists)
        return decision;
    // A device may carry several swapchains. Only the one the live overlay
    // state was built over takes its objects down with it; destroying a
    // different swapchain must leave the overlay running.
    if (input.overlayStateSwapchain == 0 || input.overlayStateSwapchain != input.destroyedSwapchain)
        return decision;
    decision.release = true;
    decision.waitForIdle = !input.deviceLost;
    return decision;
}

}  // namespace ce::overlay_swapchain_lifetime
