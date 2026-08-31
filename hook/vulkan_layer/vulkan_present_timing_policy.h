#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace ce::vulkan_present_timing_policy {

inline constexpr uint32_t kMaxChainNodes = 32;
inline constexpr uint32_t kTimingPropertyRefreshCadence = 256;

inline bool IsFifoPresentMode(VkPresentModeKHR mode) noexcept {
    return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
}

struct DeviceInput {
    bool vblankPacingRequested = false;
    bool surfaceCapabilities2Enabled = false;
    bool swapchainExtensionEnabled = false;
    bool presentTimingExtensionAvailable = false;
    bool presentId2ExtensionAvailable = false;
    bool calibratedTimestampsExtensionAvailable = false;
    bool featureQueryAvailable = false;
    bool presentTimingFeatureSupported = false;
    bool relativeTimeFeatureSupported = false;
    bool applicationFeatureChainInspectable = false;
    bool applicationSpecifiedFeature = false;
    bool applicationEnabledPresentTiming = false;
    bool applicationEnabledRelativeTime = false;
};

struct DeviceDecision {
    bool enable = false;
    bool addFeatureNode = false;
};

inline DeviceDecision DecideDevice(const DeviceInput& input) noexcept {
    DeviceDecision decision = {};
    if (!input.vblankPacingRequested || !input.surfaceCapabilities2Enabled ||
        !input.swapchainExtensionEnabled || !input.presentTimingExtensionAvailable ||
        !input.presentId2ExtensionAvailable || !input.calibratedTimestampsExtensionAvailable ||
        !input.featureQueryAvailable || !input.presentTimingFeatureSupported ||
        !input.relativeTimeFeatureSupported || !input.applicationFeatureChainInspectable) {
        return decision;
    }

    if (input.applicationSpecifiedFeature) {
        decision.enable = input.applicationEnabledPresentTiming && input.applicationEnabledRelativeTime;
        return decision;
    }

    decision.enable = true;
    decision.addFeatureNode = true;
    return decision;
}

struct SwapchainInput {
    bool vblankPacingRequested = false;
    bool deviceEnabled = false;
    bool surfaceQueryAvailable = false;
    bool surfaceQuerySucceeded = false;
    bool presentTimingSupportedForSurface = false;
    bool relativeTimeSupportedForSurface = false;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
};

inline bool ShouldEnableSwapchain(const SwapchainInput& input) noexcept {
    return input.vblankPacingRequested && input.deviceEnabled && input.surfaceQueryAvailable &&
           input.surfaceQuerySucceeded &&
           input.presentTimingSupportedForSurface && input.relativeTimeSupportedForSurface &&
           IsFifoPresentMode(input.presentMode);
}

struct ChainScan {
    bool found = false;
    bool complete = true;
};

inline ChainScan ScanChainForStructureType(const void* pNext, VkStructureType structureType) noexcept {
    const auto* node = static_cast<const VkBaseInStructure*>(pNext);
    for (uint32_t count = 0; node != nullptr && count < kMaxChainNodes; ++count) {
        if (node->sType == structureType)
            return {.found = true, .complete = true};
        if (node->pNext == node)
            return {.found = false, .complete = false};
        node = node->pNext;
    }
    return {.found = false, .complete = node == nullptr};
}

struct PresentInput {
    bool vblankPacingRequested = false;
    bool swapchainEnabled = false;
    bool fifoPresentModeActive = false;
    uint32_t swapchainCount = 0;
    uint64_t refreshDurationNs = 0;
    bool applicationChainInspectable = false;
    bool applicationAlreadySpecifiedTiming = false;
};

inline bool ShouldInjectRelativeTiming(const PresentInput& input) noexcept {
    return input.vblankPacingRequested && input.swapchainEnabled && input.fifoPresentModeActive &&
           input.swapchainCount == 1 &&
           input.refreshDurationNs != 0 &&
           input.applicationChainInspectable && !input.applicationAlreadySpecifiedTiming;
}

inline bool ShouldRefreshTimingProperties(uint32_t presentOccurrence, uint64_t refreshDurationNs) noexcept {
    return refreshDurationNs == 0 ||
           (presentOccurrence != 0 && (presentOccurrence % kTimingPropertyRefreshCadence) == 0);
}

inline bool ShouldRefreshTimeDomains(uint32_t presentOccurrence, bool timeDomainValid) noexcept {
    return !timeDomainValid ||
           (presentOccurrence != 0 && (presentOccurrence % kTimingPropertyRefreshCadence) == 0);
}

}  // namespace ce::vulkan_present_timing_policy
