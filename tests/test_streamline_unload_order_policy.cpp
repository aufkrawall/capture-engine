#include <gtest/gtest.h>

#include "hook/streamline/streamline_unload_order_policy.h"

namespace {

using ce::streamline_unload_order::Capabilities;
using ce::streamline_unload_order::ForwardReason;
using ce::streamline_unload_order::LoadAction;
using ce::streamline_unload_order::RequestAction;
using ce::streamline_unload_order::UnloadOrderPolicy;

constexpr ce::streamline_unload_order::ChainId kChain = 0x86E0;
constexpr ce::streamline_unload_order::ChainId kOtherChain = 0x9000;
constexpr uint32_t kTitleThread = 1148;
constexpr uint32_t kOtherThread = 1904;

Capabilities Ready() {
    Capabilities capabilities;
    capabilities.observerInstalled = true;
    capabilities.destroyHookInstalled = true;
    capabilities.liveChain = true;
    return capabilities;
}

TEST(StreamlineUnloadOrderPolicy, AnUnloadWithALiveChainIsHeld) {
    UnloadOrderPolicy policy;
    const auto outcome = policy.OnUnloadRequested(Ready(), kChain);
    EXPECT_EQ(outcome.action, RequestAction::kHold);
    EXPECT_EQ(outcome.reason, ForwardReason::kNone);
    EXPECT_TRUE(policy.Pending());
    EXPECT_EQ(policy.PendingChain(), kChain);
    EXPECT_TRUE(policy.AnswersNotLoaded());
}

TEST(StreamlineUnloadOrderPolicy, EveryMissingPrerequisiteForwardsTheUnloadAsToday) {
    {
        UnloadOrderPolicy policy;
        auto capabilities = Ready();
        capabilities.observerInstalled = false;
        const auto outcome = policy.OnUnloadRequested(capabilities, kChain);
        EXPECT_EQ(outcome.action, RequestAction::kForward);
        EXPECT_EQ(outcome.reason, ForwardReason::kObserverMissing);
        EXPECT_FALSE(policy.Pending());
    }
    {
        UnloadOrderPolicy policy;
        auto capabilities = Ready();
        capabilities.destroyHookInstalled = false;
        const auto outcome = policy.OnUnloadRequested(capabilities, kChain);
        EXPECT_EQ(outcome.action, RequestAction::kForward);
        EXPECT_EQ(outcome.reason, ForwardReason::kDestroyHookMissing);
        EXPECT_FALSE(policy.Pending());
    }
    {
        UnloadOrderPolicy policy;
        auto capabilities = Ready();
        capabilities.liveChain = false;
        const auto outcome = policy.OnUnloadRequested(capabilities, kChain);
        EXPECT_EQ(outcome.action, RequestAction::kForward);
        EXPECT_EQ(outcome.reason, ForwardReason::kNoLiveChain);
        EXPECT_FALSE(policy.Pending());
    }
    {
        UnloadOrderPolicy policy;
        const auto outcome = policy.OnUnloadRequested(Ready(), 0);
        EXPECT_EQ(outcome.action, RequestAction::kForward);
        EXPECT_EQ(outcome.reason, ForwardReason::kNoLiveChain);
    }
}

TEST(StreamlineUnloadOrderPolicy, GTAOrderForwardsRightAfterTheProxyReleasedTheNativeChain) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    // The proxy's final Release: Streamline dispatches the destroy hook, DLSS-G drops its own references inside it
    // (releases that are not the proxy's), then Streamline releases the native chain.
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 2).forwardUnload);
    policy.OnDestroyReturned(kChain, kTitleThread);
    const auto release = policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 0);
    EXPECT_TRUE(release.forwardUnload);
    EXPECT_EQ(release.residualRefs, 0u);
    EXPECT_FALSE(policy.Pending());
    EXPECT_FALSE(policy.AnswersNotLoaded());
}

TEST(StreamlineUnloadOrderPolicy, AResidualReferenceAfterTheDispatchStillForwardsAndIsReported) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    policy.OnDestroyReturned(kChain, kTitleThread);
    const auto release = policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 3);
    EXPECT_TRUE(release.forwardUnload);
    EXPECT_EQ(release.residualRefs, 3u);
}

TEST(StreamlineUnloadOrderPolicy, OnlyTheProxysReleaseAfterTheDispatchOnTheSameThreadForwards) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    policy.OnDestroyReturned(kOtherChain, kTitleThread);  // another chain's destroy
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 1).forwardUnload);
    policy.OnDestroyReturned(kChain, kTitleThread);
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kOtherChain, true, kTitleThread, 0).forwardUnload);
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, true, kOtherThread, 0).forwardUnload);
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, false, kTitleThread, 0).forwardUnload);
    EXPECT_TRUE(policy.Pending());
    EXPECT_TRUE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 0).forwardUnload);
}

TEST(StreamlineUnloadOrderPolicy, AReleaseBeforeTheDestroyDispatchNeverForwards) {
    // The title's own early release of a native reference must not end the hold: Streamline would not dispatch the
    // destroy for it.
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 1).forwardUnload);
    EXPECT_TRUE(policy.Pending());
}

TEST(StreamlineUnloadOrderPolicy, ALoadRequestWithdrawsTheHeldUnload) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    EXPECT_EQ(policy.OnLoadRequested(), LoadAction::kCancelled);
    EXPECT_FALSE(policy.Pending());
    EXPECT_FALSE(policy.AnswersNotLoaded());
    // Nothing is held any more: the next load request goes to Streamline.
    EXPECT_EQ(policy.OnLoadRequested(), LoadAction::kForward);
    // ...and a late release of the chain forwards nothing.
    policy.OnDestroyReturned(kChain, kTitleThread);
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 0).forwardUnload);
}

TEST(StreamlineUnloadOrderPolicy, ARepeatedUnloadWhileHeldStaysHeldUntilTheTitleKeptPresenting) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    const auto repeated = policy.OnUnloadRequested(Ready(), kChain);
    EXPECT_EQ(repeated.action, RequestAction::kHold);
    EXPECT_EQ(repeated.reason, ForwardReason::kNone);
    EXPECT_TRUE(policy.Pending());
}

TEST(StreamlineUnloadOrderPolicy, ATitleThatKeepsPresentingGetsTheUnloadAtItsNextRequest) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    policy.OnPresent(kOtherChain);
    EXPECT_FALSE(policy.TitleKeptChain());
    policy.OnPresent(kChain);
    EXPECT_TRUE(policy.TitleKeptChain());
    // Calls other than a repeated unload request never end the hold.
    EXPECT_TRUE(policy.AnswersNotLoaded());
    const auto again = policy.OnUnloadRequested(Ready(), kChain);
    EXPECT_EQ(again.action, RequestAction::kForward);
    EXPECT_EQ(again.reason, ForwardReason::kTitleKeptChain);
    EXPECT_FALSE(policy.Pending());
}

TEST(StreamlineUnloadOrderPolicy, APresentWithNothingHeldChangesNothing) {
    UnloadOrderPolicy policy;
    policy.OnPresent(kChain);
    EXPECT_FALSE(policy.TitleKeptChain());
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    EXPECT_FALSE(policy.TitleKeptChain());
}

TEST(StreamlineUnloadOrderPolicy, ADeadChainWithoutAReleaseReportEndsTheHold) {
    UnloadOrderPolicy policy;
    EXPECT_FALSE(policy.OnChainGone());
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    EXPECT_TRUE(policy.OnChainGone());
    EXPECT_FALSE(policy.Pending());
    EXPECT_FALSE(policy.OnChainGone());
}

TEST(StreamlineUnloadOrderPolicy, DroppingDiscardsTheHoldWithoutForwarding) {
    UnloadOrderPolicy policy;
    EXPECT_FALSE(policy.Drop());
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    EXPECT_TRUE(policy.Drop());
    EXPECT_FALSE(policy.Pending());
    EXPECT_FALSE(policy.AnswersNotLoaded());
}

TEST(StreamlineUnloadOrderPolicy, ANewHoldStartsCleanAfterAForward) {
    UnloadOrderPolicy policy;
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kChain).action, RequestAction::kHold);
    policy.OnPresent(kChain);
    policy.OnDestroyReturned(kChain, kTitleThread);
    ASSERT_TRUE(policy.OnNativeReleaseReturned(kChain, true, kTitleThread, 0).forwardUnload);
    ASSERT_EQ(policy.OnUnloadRequested(Ready(), kOtherChain).action, RequestAction::kHold);
    EXPECT_EQ(policy.PendingChain(), kOtherChain);
    EXPECT_FALSE(policy.TitleKeptChain());
    // The previous chain's destroy report does not carry over.
    EXPECT_FALSE(policy.OnNativeReleaseReturned(kOtherChain, true, kTitleThread, 0).forwardUnload);
}

}  // namespace
