// ProcessFrame's handling of a swapchain a frame-generation runtime presents itself (ce::fg_presenter, XeFG): a new
// registration lifetime is a new swapchain even at a reused address, its overlay is rebuilt at once on the queue its
// creation proved, and a Streamline OFF observed there (the DLSS-G signal was a front) leaves that state drawable.

#include <gtest/gtest.h>

#include "hook/d3d12/dx12_overlay_policy/overlay_submission.h"
#include "hook/d3d12/dx12_overlay_policy/postsl_keepalive.h"

namespace {

using namespace ce::dx12_overlay_policy;

// FlowXeFGSwitch.AStreamlineFGSignalBesideXeFGIsAFront: XeFG's chain took the game's released chain's address, so
// ProcessFrame kept the old chain's render targets; only the Streamline OFF teardown happened to rebuild them.
TEST(RuntimePresentedSwapchainPolicyTest, ANewRegistrationLifetimeIsANewSwapchainAtAnyAddress) {
    EXPECT_TRUE(IsNewRuntimePresentedSwapchainLifetime(/*presenterLifetime=*/2, /*trackedPresenterLifetime=*/0));
    EXPECT_TRUE(IsNewRuntimePresentedSwapchainLifetime(3, 2));
    EXPECT_FALSE(IsNewRuntimePresentedSwapchainLifetime(2, 2));  // the tracked chain itself
    EXPECT_FALSE(IsNewRuntimePresentedSwapchainLifetime(0, 2));  // no runtime-presented chain: pointers decide
    EXPECT_TRUE(ShouldProcessLogicalSwapchainReplacement(/*pointerAddressChanged=*/false,
                                                         IsNewRuntimePresentedSwapchainLifetime(3, 2)));
}

TEST(RuntimePresentedSwapchainPolicyTest, ARuntimePresentedChainIsRebuiltAtOnceOnItsProvenQueue) {
    EXPECT_TRUE(ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain(true, true, false));
    // Without the queue its creation recorded there is no safe queue: the fail-closed route decides, not this.
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain(true, false, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain(false, true, false));
    EXPECT_FALSE(ShouldReinitOverlayImmediatelyOnRuntimePresentedSwapchain(true, true, /*deviceRemoved=*/true));
}

TEST(RuntimePresentedSwapchainPolicyTest, AStreamlineOffOnARuntimePresentedChainKeepsItsOwnOverlay) {
    EXPECT_TRUE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(true, true, true, true, false));
    // Any other chain may hold render targets on Streamline's back buffers: the teardown stays.
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(true, false, true, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(false, true, true, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(true, true, false, true, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(true, true, true, false, false));
    EXPECT_FALSE(ShouldKeepOverlayLiveAcrossStreamlineOffOnRuntimePresentedSwapchain(true, true, true, true, true));
}

}  // namespace
