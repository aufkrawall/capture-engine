#include <gtest/gtest.h>

#include "../common/capture_handoff_state.h"

namespace handoff = ce::capture_handoff;

TEST(CaptureHandoffStateTest, StartsIdleWithInjectAuthoritative) {
    handoff::InjectToWgcHandoff state;

    EXPECT_EQ(state.GetPhase(), handoff::Phase::kIdle);
    EXPECT_TRUE(state.MustKeepInjectActive());
    EXPECT_FALSE(state.IsTerminal());
}

TEST(CaptureHandoffStateTest, BeginStartsWgcWithoutStoppingInject) {
    handoff::InjectToWgcHandoff state;

    const auto transition = state.Begin();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kStarting);
    EXPECT_EQ(transition.action, handoff::Action::kStartWgcKeepInject);
    EXPECT_TRUE(state.MustKeepInjectActive());
    EXPECT_FALSE(state.IsTerminal());
}

TEST(CaptureHandoffStateTest, FirstWgcFrameCommitsAndOnlyThenStopsInject) {
    handoff::InjectToWgcHandoff state;
    ASSERT_TRUE(state.Begin().changed);

    const auto transition = state.OnWgcFirstFrame();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kCommitted);
    EXPECT_EQ(transition.action, handoff::Action::kCommitWgcStopInject);
    EXPECT_FALSE(state.MustKeepInjectActive());
    EXPECT_TRUE(state.IsTerminal());
}

TEST(CaptureHandoffStateTest, WgcFailureAbandonsWgcAndKeepsInject) {
    handoff::InjectToWgcHandoff state;
    ASSERT_TRUE(state.Begin().changed);

    const auto transition = state.OnWgcFailure();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kFailed);
    EXPECT_EQ(transition.action, handoff::Action::kStopWgcKeepInject);
    EXPECT_TRUE(state.MustKeepInjectActive());
    EXPECT_TRUE(state.IsTerminal());
}

TEST(CaptureHandoffStateTest, InjectWinsBeforeFallbackStarts) {
    handoff::InjectToWgcHandoff state;

    const auto transition = state.OnInjectFrame();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kInjectWon);
    EXPECT_EQ(transition.action, handoff::Action::kNone);
    EXPECT_TRUE(state.MustKeepInjectActive());
    EXPECT_TRUE(state.IsTerminal());
    EXPECT_FALSE(state.Begin().changed);
}

TEST(CaptureHandoffStateTest, InjectWinsRaceWhileWgcIsStarting) {
    handoff::InjectToWgcHandoff state;
    ASSERT_TRUE(state.Begin().changed);

    const auto transition = state.OnInjectFrame();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kInjectWon);
    EXPECT_EQ(transition.action, handoff::Action::kStopWgcKeepInject);
    EXPECT_TRUE(state.MustKeepInjectActive());

    const auto lateWgcFrame = state.OnWgcFirstFrame();
    EXPECT_FALSE(lateWgcFrame.changed);
    EXPECT_EQ(lateWgcFrame.phase, handoff::Phase::kInjectWon);
    EXPECT_EQ(lateWgcFrame.action, handoff::Action::kNone);
}

TEST(CaptureHandoffStateTest, ReadinessTimeoutAbandonsWgcAndKeepsInject) {
    handoff::InjectToWgcHandoff state;
    ASSERT_TRUE(state.Begin().changed);

    const auto transition = state.OnWgcReadinessTimeout();

    EXPECT_TRUE(transition.changed);
    EXPECT_EQ(transition.phase, handoff::Phase::kTimedOut);
    EXPECT_EQ(transition.action, handoff::Action::kStopWgcKeepInject);
    EXPECT_TRUE(state.MustKeepInjectActive());
    EXPECT_TRUE(state.IsTerminal());
}

TEST(CaptureHandoffStateTest, FirstTerminalEventWinsEveryRace) {
    handoff::InjectToWgcHandoff failed;
    ASSERT_TRUE(failed.Begin().changed);
    ASSERT_TRUE(failed.OnWgcFailure().changed);
    EXPECT_FALSE(failed.OnWgcFirstFrame().changed);
    EXPECT_FALSE(failed.OnInjectFrame().changed);
    EXPECT_EQ(failed.GetPhase(), handoff::Phase::kFailed);

    handoff::InjectToWgcHandoff timedOut;
    ASSERT_TRUE(timedOut.Begin().changed);
    ASSERT_TRUE(timedOut.OnWgcReadinessTimeout().changed);
    EXPECT_FALSE(timedOut.OnWgcFailure().changed);
    EXPECT_FALSE(timedOut.OnWgcFirstFrame().changed);
    EXPECT_EQ(timedOut.GetPhase(), handoff::Phase::kTimedOut);

    handoff::InjectToWgcHandoff committed;
    ASSERT_TRUE(committed.Begin().changed);
    ASSERT_TRUE(committed.OnWgcFirstFrame().changed);
    EXPECT_FALSE(committed.OnInjectFrame().changed);
    EXPECT_FALSE(committed.OnWgcFailure().changed);
    EXPECT_EQ(committed.GetPhase(), handoff::Phase::kCommitted);
}

TEST(CaptureHandoffStateTest, ResetAllowsANewTransaction) {
    handoff::InjectToWgcHandoff state;
    ASSERT_TRUE(state.Begin().changed);
    ASSERT_TRUE(state.OnWgcFailure().changed);

    const auto reset = state.Reset();
    EXPECT_TRUE(reset.changed);
    EXPECT_EQ(reset.phase, handoff::Phase::kIdle);
    EXPECT_EQ(reset.action, handoff::Action::kNone);
    EXPECT_TRUE(state.Begin().changed);
    EXPECT_EQ(state.GetPhase(), handoff::Phase::kStarting);
}
