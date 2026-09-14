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

// When CE's overlay may destroy the binary semaphores its composites signal.
//
// Every composited present waits on one of `OverlayState::semaphores`:
// `RenderOverlay` hands the slot's semaphore back and the present hook rewrites
// `VkPresentInfoKHR::pWaitSemaphores` to it. That wait is executed by the
// presentation engine, and *nothing CE can observe proves it has run*.
// `vkDeviceWaitIdle` does not: it covers queue operations, not a present that
// has already been handed to the presentation engine. CE's own submission-ring
// reuse already says so - a slot may only be reused once the image's acquire
// generation has moved on, "because the fence proves the submission retired and
// says nothing about the present that waits on the semaphore". The teardown
// path contradicted that and destroyed the whole ring behind a device-idle wait.
//
// DOOM Eternal `20260914_122133` is that failure, and it survived the
// release-before-destroy ordering above. `perf_metrics_25856.csv` holds exactly
// two frames: the overlay composited once into the startup swapchain at
// 12:27:18.158 and the game destroyed that swapchain at .223, 15 ms after the
// present. CE waited for device idle, destroyed the ring - semaphores included -
// at .223-.255, the driver's `vkDestroySwapchainKHR` ran, and `nvlddmkm` logged
// event 153 at .2572. The first present on the replacement returned
// VK_ERROR_DEVICE_LOST and the window stayed black. Session `20260913_193606`
// shows the discriminator: its first two swapchain destroys carried no composite
// at all and did not fault, while the third came after ten composited presents
// and faulted 1 ms into the teardown.
//
// The one point CE can prove is the swapchain's own destruction: a present is
// made against a swapchain, so once `vkDestroySwapchainKHR` has returned no
// pending present of that swapchain can still be waiting on anything. The ring's
// semaphores therefore outlive the rest of the overlay state by exactly that
// much - released into a deferred batch tagged with the swapchain they were
// presented against, and destroyed once the driver has destroyed it (or when the
// device itself goes away, by which point the application has destroyed every
// swapchain on it).
//
// Note the asymmetry with the image views and framebuffers above: those must be
// destroyed *before* the driver's destroy because the presentable images die
// with the swapchain, while the semaphores must be destroyed *after* it. They
// are independent objects, so both rules hold at once.

namespace ce::overlay_present_semaphore_lifetime {

struct Input {
    // The swapchain the deferred batch's semaphores were presented against.
    uint64_t deferredSwapchain = 0;
    // The swapchain the driver has just destroyed. Zero when the drain is not
    // driven by a swapchain destroy.
    uint64_t destroyedSwapchain = 0;
    // The device is being destroyed, so every swapchain on it is already gone.
    bool deviceTeardown = false;
};

constexpr bool MayDestroy(const Input& input) {
    if (input.deviceTeardown)
        return true;
    // A batch that names no swapchain was never presented against one, so no
    // present can refer to it.
    if (input.deferredSwapchain == 0)
        return true;
    return input.destroyedSwapchain != 0 && input.deferredSwapchain == input.destroyedSwapchain;
}

}  // namespace ce::overlay_present_semaphore_lifetime
