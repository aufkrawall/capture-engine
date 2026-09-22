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

namespace {

using ce::overlay_present_semaphore_lifetime::MayDestroy;

ce::overlay_present_semaphore_lifetime::Input MakeSemaphoreInput(uint64_t deferred, uint64_t destroyed) {
    ce::overlay_present_semaphore_lifetime::Input input = {};
    input.deferredSwapchain = deferred;
    input.destroyedSwapchain = destroyed;
    return input;
}

}  // namespace

// The DOOM Eternal `20260914_122133` failure: one composited present was still
// outstanding when the game destroyed the swapchain, and CE destroyed the
// semaphore that present waits on behind a device-idle wait that proves nothing
// about it. Destroying the swapchain is the point that does.
TEST(OverlayPresentSemaphoreLifetimeTest, DestroysWithTheSwapchainItWasPresentedAgainst) {
    EXPECT_TRUE(MayDestroy(MakeSemaphoreInput(kSwapchainA, kSwapchainA)));
}

// Another swapchain's destroy says nothing about presents still pending on this
// one, so the batch has to keep waiting.
TEST(OverlayPresentSemaphoreLifetimeTest, HoldsAcrossAnUnrelatedSwapchainDestroy) {
    EXPECT_FALSE(MayDestroy(MakeSemaphoreInput(kSwapchainA, kSwapchainB)));
}

// A drain that is not driven by a swapchain destroy cannot release a batch that
// still names a live swapchain.
TEST(OverlayPresentSemaphoreLifetimeTest, HoldsWhenNoSwapchainWasDestroyed) {
    EXPECT_FALSE(MayDestroy(MakeSemaphoreInput(kSwapchainA, 0)));
}

// An application destroys every swapchain before the device, so device teardown
// releases the whole store.
TEST(OverlayPresentSemaphoreLifetimeTest, ReleasesEverythingOnDeviceTeardown) {
    auto input = MakeSemaphoreInput(kSwapchainA, 0);
    input.deviceTeardown = true;
    EXPECT_TRUE(MayDestroy(input));
}

// A state that never recorded a swapchain never composited into one, so nothing
// can be presenting its semaphores.
TEST(OverlayPresentSemaphoreLifetimeTest, ReleasesABatchThatNamesNoSwapchain) {
    EXPECT_TRUE(MayDestroy(MakeSemaphoreInput(0, 0)));
}

// The mirror image of the image-view rule: views die before the driver's
// destroy, the present-wait semaphores after it.
TEST(OverlaySwapchainLifetimeSourceTest, DestroyHookReleasesPresentSemaphoresAfterTheDriverDestroy) {
    const std::string source = StripComments(ReadLayerSource("vulkan_layer_swapchain.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t hook = source.find("Capture_vkDestroySwapchainKHR(VkDevice device");
    ASSERT_NE(hook, std::string::npos);
    const size_t driverDestroy = source.find("fp_vkDestroySwapchainKHR(device, swapchain", hook);
    const size_t drain = source.find("DestroyDeferredOverlayPresentSemaphores(device, swapchain)", hook);
    ASSERT_NE(driverDestroy, std::string::npos);
    ASSERT_NE(drain, std::string::npos) << "the destroy hook must release the deferred present semaphores";
    EXPECT_LT(driverDestroy, drain) << "a pending present of this swapchain may still wait on them until it is gone";
}

// Both overlay teardown paths run while presents can still be pending - the
// swapchain-destroy one, and the `oldSwapchain` retirement that goes through
// CleanupOverlay - so neither may destroy the ring's semaphores itself.
TEST(OverlaySwapchainLifetimeSourceTest, OverlayTeardownDefersThePresentSemaphores) {
    const std::string source = StripComments(ReadLayerSource("layer_overlay.cpp"));
    ASSERT_FALSE(source.empty());

    size_t defers = 0;
    for (size_t index = source.find("DeferPresentSemaphoresLocked(state, device)"); index != std::string::npos;
         index = source.find("DeferPresentSemaphoresLocked(state, device)", index + 1)) {
        ++defers;
    }
    EXPECT_EQ(defers, 2u) << "CleanupOverlayState and CleanupOverlay both have to defer";

    // The loop both paths used to run. A slot the ring rolls back in
    // PopSubmissionRingSlot was never presented and is still destroyed inline,
    // so only the whole-ring teardown is checked for here.
    EXPECT_EQ(source.find("for (auto s : state.semaphores)"), std::string::npos)
        << "the ring's semaphores may only be destroyed from the deferred store";
}

// DOOM Eternal `20260922_235937`: the sharpen pass holds a view and framebuffer
// per presentable image but was only released at the next create's
// `oldSwapchain`. DOOM destroys and recreates without one, NVIDIA handed the new
// swapchain the old handle, the present-time generation check matched, and the
// first sharpen draw went through views of freed images: VK_ERROR_DEVICE_LOST
// and a black window.
TEST(SharpenSwapchainLifetimeSourceTest, DestroyHookReleasesSharpenBeforeTheDriverDestroy) {
    const std::string source = StripComments(ReadLayerSource("vulkan_layer_swapchain.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t hook = source.find("Capture_vkDestroySwapchainKHR(VkDevice device");
    ASSERT_NE(hook, std::string::npos);
    const size_t release = source.find("ReleaseSharpenForSwapchain(device, swapchain)", hook);
    const size_t driverDestroy = source.find("fp_vkDestroySwapchainKHR(device, swapchain", hook);
    const size_t drain = source.find("DestroyDeferredSharpenSemaphores(device, swapchain)", hook);
    ASSERT_NE(release, std::string::npos) << "the destroy hook must release the sharpen pass's image views";
    ASSERT_NE(driverDestroy, std::string::npos);
    ASSERT_NE(drain, std::string::npos) << "the destroy hook must release the deferred sharpen semaphores";
    EXPECT_LT(release, driverDestroy) << "the views must go before the driver frees the presentable images";
    EXPECT_LT(driverDestroy, drain) << "a pending present of this swapchain may still wait on the semaphores";
}

// When no overlay or capture stage runs after it, the present waits on the
// sharpen semaphore directly, so the pass's teardown - swapchain release,
// `oldSwapchain` retirement, a live switch to off - must defer it like the
// overlay ring's semaphores and destroy it only from the drain.
TEST(SharpenSwapchainLifetimeSourceTest, SharpenTeardownDefersItsPresentSemaphores) {
    const std::string source = StripComments(ReadLayerSource("layer_sharpen_setup.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t destroyState = source.find("void DestroySharpenState(SharpenState& state");
    const size_t drain = source.find("void DrainDeferredSharpenSemaphoresLocked(");
    ASSERT_NE(destroyState, std::string::npos);
    ASSERT_NE(drain, std::string::npos);
    const size_t firstDestroy = source.find("fp_vkDestroySemaphore(");
    ASSERT_NE(firstDestroy, std::string::npos);
    EXPECT_GT(firstDestroy, drain) << "only the deferred drain may destroy the sharpen semaphores";
    EXPECT_EQ(source.find("fp_vkDestroySemaphore(", firstDestroy + 1), std::string::npos);
    EXPECT_NE(source.find("layer_sharpen_g_DeferredSemaphores.push_back", destroyState), std::string::npos);
}

TEST(SharpenSwapchainLifetimeSourceTest, DeviceTeardownDrainsEveryDeferredSharpenSemaphore) {
    const std::string source = StripComments(ReadLayerSource("layer_sharpen.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t cleanup = source.find("void CleanupSharpen(VkDevice device)");
    ASSERT_NE(cleanup, std::string::npos);
    const size_t drain = source.find("DrainDeferredSharpenSemaphoresLocked(device, VK_NULL_HANDLE, true)", cleanup);
    EXPECT_NE(drain, std::string::npos);
}

// The same session's earlier DOOM run: the host published `sharpen=cas` twice
// while the game ran and the layer never filtered a frame, because it only
// read the setting once at IPC connect. The D3D hooks read the live copy.
TEST(SharpenSwapchainLifetimeSourceTest, SharpenFollowsTheLiveSharedConfig) {
    const std::string source = StripComments(ReadLayerSource("layer_sharpen.cpp"));
    ASSERT_FALSE(source.empty());

    const size_t entry = source.find("bool SharpenPresentedFrame(");
    ASSERT_NE(entry, std::string::npos);
    const size_t refresh = source.find("StoreSharpenSettings(", entry);
    const size_t read = source.find("GetSharpenRequest()", entry);
    ASSERT_NE(refresh, std::string::npos) << "the pass must refresh the request from shared memory per present";
    ASSERT_NE(read, std::string::npos);
    EXPECT_LT(refresh, read);
}
