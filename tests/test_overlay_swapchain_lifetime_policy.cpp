#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/overlay_swapchain_lifetime_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::overlay_swapchain_lifetime::Decide;
using ce::overlay_swapchain_lifetime::Decision;
using ce::overlay_swapchain_lifetime::Input;

constexpr uint64_t kSwapchainA = 0x1AE6AA500D0ull;
constexpr uint64_t kSwapchainB = 0x21EEFF200D0ull;

Input MakeInput(uint64_t stateSwapchain, uint64_t destroyedSwapchain) {
    Input input = {};
    input.overlayStateExists = true;
    input.overlayStateSwapchain = stateSwapchain;
    input.destroyedSwapchain = destroyedSwapchain;
    return input;
}

std::string StripComments(const std::string& source) {
    std::string stripped;
    stripped.reserve(source.size());
    for (size_t index = 0; index < source.size();) {
        if (source.compare(index, 2, "//") == 0) {
            const size_t lineEnd = source.find('\n', index);
            if (lineEnd == std::string::npos)
                break;
            index = lineEnd;
            continue;
        }
        if (source.compare(index, 2, "/*") == 0) {
            const size_t blockEnd = source.find("*/", index + 2);
            if (blockEnd == std::string::npos)
                break;
            index = blockEnd + 2;
            continue;
        }
        stripped.push_back(source[index]);
        ++index;
    }
    return stripped;
}

std::string ReadLayerSource(const char* name) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" / name);
}

}  // namespace

// The DOOM Eternal `20260913_174040` failure: the overlay's image views and
// framebuffers were built over the swapchain being destroyed, so they must be
// released before the driver frees the presentable images they reference.
TEST(OverlaySwapchainLifetimePolicyTest, ReleasesTheStateBuiltOverTheDestroyedSwapchain) {
    const Decision decision = Decide(MakeInput(kSwapchainA, kSwapchainA));
    EXPECT_TRUE(decision.release);
    EXPECT_TRUE(decision.waitForIdle);
}

// A device may own more than one swapchain. Destroying one the overlay was not
// built over must leave the running overlay alone - tearing it down there would
// blank the overlay for a swapchain that is still presenting.
TEST(OverlaySwapchainLifetimePolicyTest, LeavesAnUnrelatedSwapchainDestroyAlone) {
    const Decision decision = Decide(MakeInput(kSwapchainB, kSwapchainA));
    EXPECT_FALSE(decision.release);
    EXPECT_FALSE(decision.waitForIdle);
}

TEST(OverlaySwapchainLifetimePolicyTest, DoesNothingWithoutOverlayState) {
    Input input = {};
    input.overlayStateExists = false;
    input.destroyedSwapchain = kSwapchainA;
    EXPECT_FALSE(Decide(input).release);
}

// An overlay state that never recorded its swapchain owns no proven derivation
// from the one being destroyed, and releasing on that guess would take down a
// live overlay.
TEST(OverlaySwapchainLifetimePolicyTest, DoesNothingWhenTheStateNamesNoSwapchain) {
    const Decision decision = Decide(MakeInput(0, kSwapchainA));
    EXPECT_FALSE(decision.release);
}

// A lost device answers no wait. CleanupOverlayState skips the device-idle wait
// on a latched loss, and the policy has to report the same thing so the release
// cannot park the game's teardown on a dead queue.
TEST(OverlaySwapchainLifetimePolicyTest, SkipsTheIdleWaitOnALatchedDeviceLoss) {
    Input input = MakeInput(kSwapchainA, kSwapchainA);
    input.deviceLost = true;
    const Decision decision = Decide(input);
    EXPECT_TRUE(decision.release);
    EXPECT_FALSE(decision.waitForIdle);
}

// The ordering is the fix. Releasing after the driver call - or at the next
// vkCreateSwapchainKHR, which is where CE used to do it - hands the driver image
// views over presentable images it has already freed.
TEST(OverlaySwapchainLifetimeSourceTest, DestroyHookReleasesTheOverlayBeforeTheDriverDestroy) {
    const std::string source = StripComments(ReadLayerSource("vulkan_layer_swapchain.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t hook = source.find("Capture_vkDestroySwapchainKHR(VkDevice device");
    ASSERT_NE(hook, std::string::npos);
    const size_t release = source.find("ReleaseOverlayForSwapchain(device, swapchain)", hook);
    const size_t driverDestroy = source.find("fp_vkDestroySwapchainKHR(device, swapchain", hook);
    ASSERT_NE(release, std::string::npos) << "the destroy hook must release CE's swapchain-derived overlay objects";
    ASSERT_NE(driverDestroy, std::string::npos);
    EXPECT_LT(release, driverDestroy) << "the release must run before the driver frees the presentable images";
}

// The destroy hook can only identify the state it has to release if the state
// records which swapchain it was built over.
TEST(OverlaySwapchainLifetimeSourceTest, InitializeOverlayRecordsTheOwningSwapchain) {
    const std::string source = StripComments(ReadLayerSource("layer_overlay.cpp"));
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("state.swapchain = swapchain;"), std::string::npos);
}
