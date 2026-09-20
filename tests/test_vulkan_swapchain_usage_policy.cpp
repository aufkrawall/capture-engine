#include <gtest/gtest.h>

#include "../hook/vulkan_layer/vulkan_swapchain_usage_policy.h"

using ce::vulkan_swapchain_usage::Decide;
using ce::vulkan_swapchain_usage::Decision;
using ce::vulkan_swapchain_usage::Input;
using ce::vulkan_swapchain_usage::kTransferSrcBit;

namespace {

constexpr uint32_t kColorAttachmentBit = 0x00000010u;
constexpr uint32_t kTransferDstBit = 0x00000002u;

}  // namespace

TEST(VulkanSwapchainUsagePolicy, AddsTransferSrcWhenTheSurfaceSupportsIt) {
    Input input;
    input.applicationUsage = kColorAttachmentBit;
    input.supportedUsage = kColorAttachmentBit | kTransferSrcBit | kTransferDstBit;
    input.surfaceCapabilitiesKnown = true;

    const Decision decision = Decide(input);
    EXPECT_TRUE(decision.overrideApplied);
    EXPECT_EQ(decision.usage, kColorAttachmentBit | kTransferSrcBit);
    EXPECT_STREQ(decision.reason, "added_transfer_src");
}

TEST(VulkanSwapchainUsagePolicy, KeepsEveryBitTheApplicationAskedFor) {
    // The application's own usage is never narrowed: the swapchain is the
    // game's, and a dropped bit would break its own reads or blits.
    Input input;
    input.applicationUsage = kColorAttachmentBit | kTransferDstBit;
    input.supportedUsage = 0xFFFFFFFFu;
    input.surfaceCapabilitiesKnown = true;

    const Decision decision = Decide(input);
    EXPECT_EQ(decision.usage & input.applicationUsage, input.applicationUsage);
}

TEST(VulkanSwapchainUsagePolicy, DoesNothingWhenTheApplicationAlreadyAskedForIt) {
    Input input;
    input.applicationUsage = kColorAttachmentBit | kTransferSrcBit;
    input.supportedUsage = 0xFFFFFFFFu;
    input.surfaceCapabilitiesKnown = true;

    const Decision decision = Decide(input);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_EQ(decision.usage, input.applicationUsage);
    EXPECT_STREQ(decision.reason, "already_requested");
}

TEST(VulkanSwapchainUsagePolicy, RefusesWhenTheSurfaceDoesNotSupportTransferSrc) {
    // Requesting an unsupported usage makes vkCreateSwapchainKHR fail, and that
    // swapchain is the game's - so an unsupported bit is never requested.
    Input input;
    input.applicationUsage = kColorAttachmentBit;
    input.supportedUsage = kColorAttachmentBit;
    input.surfaceCapabilitiesKnown = true;

    const Decision decision = Decide(input);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_EQ(decision.usage, kColorAttachmentBit);
    EXPECT_STREQ(decision.reason, "surface_does_not_support_transfer_src");
}

TEST(VulkanSwapchainUsagePolicy, FailsClosedWhenCapabilitiesCannotBeRead) {
    Input input;
    input.applicationUsage = kColorAttachmentBit;
    // A caller that could not query reports nothing, not an empty mask it made up.
    input.supportedUsage = 0xFFFFFFFFu;
    input.surfaceCapabilitiesKnown = false;

    const Decision decision = Decide(input);
    EXPECT_FALSE(decision.overrideApplied);
    EXPECT_EQ(decision.usage, kColorAttachmentBit);
    EXPECT_STREQ(decision.reason, "surface_capabilities_unknown");
}
