#include <gtest/gtest.h>

#include "../hook/common/dx12_overlay_policy.h"

namespace {

using ce::dx12_overlay_policy::BelowForeignChainFSRDeepDrawDecision;
using ce::dx12_overlay_policy::DecideBelowForeignChainFSRDeepDraw;

// The steady-state Talos FSR-FG + Steam shape (session 20260813_061015): CE intercepts below the
// foreign Present chain, the FFX present callback is the live overlay transport, and the routed
// submit queue is the swapchain-owning queue the foreign overlay's own command list went through.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, DrawsOnSwapchainQueueInTheHealthyAppCallbackState) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(
                  /*presentInterceptedBelowForeignChain=*/true, /*foreignOverlayLoaded=*/true,
                  /*nativeFSRActive=*/true, /*runtimeOwnedNativeFGPresentPath=*/true,
                  /*nativeFSRInternalNoCallbackComposition=*/false, /*ffxPresentCallbackActive=*/true,
                  /*ffxPresentCallbackStalled=*/false, /*explicitNativeFSROffPending=*/false,
                  /*protectedOfficialFFXStartupQuiesced=*/false, /*hasSwapchainQueue=*/true,
                  /*submitQueueIsSwapchainQueue=*/true, /*deviceRemoved=*/false, /*shuttingDown=*/false),
              BelowForeignChainFSRDeepDrawDecision::kDrawOnSwapchainQueue);
}

// Without a foreign overlay chain the callback draw already owns the frame and nothing composites on
// top of CE; the deep-body route must stay off so the no-chain behavior is byte-identical to today.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, RequiresTheForeignChainView) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(false, true, true, true, false, true, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, false, true, true, false, true, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// The no-callback internal-composition route must never take this path: a separate submit on that
// runtime queue is the documented ffxQuery wedge and ACCESS_DENIED boundary.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, NeverDrawsOnTheNoCallbackCompositionRoute) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, true, true, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// Only the live, un-stalled FFX present-callback state is eligible. A stalled callback means the
// runtime is suspending/menu-idle; the existing stall fallback rules own that state.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, RequiresALiveFFXPresentCallback) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, false, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, true, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// The explicit FSR-off teardown window and the protected startup quiescence are the proven device-
// removal seams; the deep-body route must close there and hand the frame back to the callback.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, ClosesDuringTeardownAndProtectedStartup) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, true, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, false, true, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// The draw must land on the swapchain-owning queue. A missing queue or a routed queue that is not the
// captured swapchain queue cannot guarantee ordering above the foreign overlay's own command list.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, RequiresTheSwapchainOwningQueue) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, false, false, false,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, false, false, true,
                                                 false, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// A removed device or a shutting-down hook must not record or submit any new GPU work.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, RefusesOnDeviceLossAndShutdown) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, false, false, true,
                                                 true, true, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, true, false, true, false, false, false, true,
                                                 true, false, true),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

// The route is native-FSR specific; Streamline/DLSS or any other runtime must keep today's behavior.
TEST(BelowForeignChainFSRDeepDrawPolicyTest, IsNativeFSRSpecific) {
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, false, true, false, true, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
    EXPECT_EQ(DecideBelowForeignChainFSRDeepDraw(true, true, true, false, false, true, false, false, false, true,
                                                 true, false, false),
              BelowForeignChainFSRDeepDrawDecision::kUnavailable);
}

}  // namespace
