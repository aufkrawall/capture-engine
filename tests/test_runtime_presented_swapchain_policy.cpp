// ProcessFrame's handling of a swapchain a frame-generation runtime presents itself (ce::fg_presenter, XeFG): a new
// registration lifetime is a new swapchain even at a reused address, its overlay is rebuilt at once on the queue its
// creation proved, and a Streamline OFF observed there (the DLSS-G signal was a front) leaves that state drawable.

#include <gtest/gtest.h>

#include "hook/d3d12/dx12_overlay_policy/overlay_submission.h"
#include "hook/d3d12/dx12_overlay_policy/postsl_keepalive.h"
#include "hook/d3d12/dx12_overlay_policy/runtime_presented_swapchain.h"

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

// 20261023_221541: OptiScaler's FFX swapchain (FGOutput=FSRFG) had its staged queue retired by the game's
// slDLSSGSetOptions(on), which OptiScaler answered itself; CE then drew it from the game queue and the device was
// removed (0x887A002B).
TEST(RuntimePresentedSwapchainPolicyTest, AFrontNeverSupersedesAnotherRuntimesStartup) {
    EXPECT_TRUE(ShouldRetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable(true, true, false, false));
    EXPECT_FALSE(ShouldRetireProtectedOfficialFFXStartupForSuccessfulStreamlineEnable(true, true, false,
                                                                                      /*streamlineSignalFront=*/true));
}

TEST(RuntimePresentedSwapchainPolicyTest, AnUncapturedQueueIsNeverGuessedForARuntimesSwapchain) {
    EXPECT_EQ(DecideUncapturedQueueFallback(true, true, false), UncapturedQueueFallback::kDraw);
    EXPECT_EQ(DecideUncapturedQueueFallback(true, true, true), UncapturedQueueFallback::kDraw);
    // Another queue than the swapchain's own removes the device, whoever created it.
    EXPECT_EQ(DecideUncapturedQueueFallback(true, false, false), UncapturedQueueFallback::kSkipQueueMismatch);
    // Unreadable: a guess, kept only for a swapchain no frame-generation runtime created.
    EXPECT_EQ(DecideUncapturedQueueFallback(false, false, false), UncapturedQueueFallback::kDraw);
    EXPECT_EQ(DecideUncapturedQueueFallback(false, false, true), UncapturedQueueFallback::kSkipRuntimeChainUnproven);
}

}  // namespace
