#pragma once

#include <cstdint>

// Whether CE may add VK_IMAGE_USAGE_TRANSFER_SRC_BIT to a VkSwapchainKHR.
//
// Reading a presentable image is not free in Vulkan the way it is in DXGI: a
// swapchain image can only be copied from when the swapchain was created with
// TRANSFER_SRC, and an application that never reads its own frames has no
// reason to ask for it. CE does read them - the sharpen pass needs an untouched
// copy of the frame to filter, and inject capture copies the frame to the
// encoder - so the bit has to be requested at creation or not at all.
//
// The rule is fail-closed. `VkSurfaceCapabilitiesKHR::supportedUsageFlags` is
// the only authority on what a surface allows, and asking for a bit outside it
// makes vkCreateSwapchainKHR fail - which would take the game's swapchain down
// with it. When the capabilities cannot be read, CE asks for nothing extra and
// the features that wanted the bit refuse themselves later.
namespace ce::vulkan_swapchain_usage {

// VK_IMAGE_USAGE_TRANSFER_SRC_BIT, spelled out so this header stays free of the
// Vulkan headers and testable on its own.
inline constexpr uint32_t kTransferSrcBit = 0x00000001u;

struct Input {
    // VkSwapchainCreateInfoKHR::imageUsage as the application asked for it.
    uint32_t applicationUsage = 0;
    // VkSurfaceCapabilitiesKHR::supportedUsageFlags.
    uint32_t supportedUsage = 0;
    bool surfaceCapabilitiesKnown = false;
};

struct Decision {
    bool overrideApplied = false;
    // The usage to pass to the driver: the application's own unless the
    // override applied.
    uint32_t usage = 0;
    // Stable identifier for logs; never null.
    const char* reason = "";
};

inline Decision Decide(const Input& input) {
    Decision decision;
    decision.usage = input.applicationUsage;

    if ((input.applicationUsage & kTransferSrcBit) != 0) {
        decision.reason = "already_requested";
        return decision;
    }
    if (!input.surfaceCapabilitiesKnown) {
        // Never request a bit that cannot be proven supported: a rejected
        // vkCreateSwapchainKHR is the game's swapchain, not CE's.
        decision.reason = "surface_capabilities_unknown";
        return decision;
    }
    if ((input.supportedUsage & kTransferSrcBit) == 0) {
        decision.reason = "surface_does_not_support_transfer_src";
        return decision;
    }

    decision.overrideApplied = true;
    decision.usage = input.applicationUsage | kTransferSrcBit;
    decision.reason = "added_transfer_src";
    return decision;
}

}  // namespace ce::vulkan_swapchain_usage
