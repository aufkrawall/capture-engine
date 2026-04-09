#include <gtest/gtest.h>

#include "../hook/common/streamline_runtime_policy.h"

namespace {

TEST(StreamlineRuntimePolicyTest, RequestedOptionsEnableBuildsActiveRuntimeUpdate) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
    EXPECT_EQ(3u, update.capabilityMax);
}

TEST(StreamlineRuntimePolicyTest, RequestedOptionsOffBuildsInactiveRuntimeUpdate) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 0, 3, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
    EXPECT_EQ(0, update.multiplier);
    EXPECT_EQ(0u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, RequestedOptionsFallbacksToTwoXForInvalidGeneratedFrameCount) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(true, true, 2, 0, 1);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateIgnoresEnableRequestsWithoutRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, false, false, 1, 1, 3);

    EXPECT_FALSE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
}

TEST(StreamlineRuntimePolicyTest, GetStateAllowsEnableWithRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, true, false, 1, 3, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(4, update.multiplier);
    EXPECT_EQ(3u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateAllowsDisableWithoutRuntimeEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, false, false, 0, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
    EXPECT_EQ(0, update.multiplier);
    EXPECT_EQ(0u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, GetStateKeepsKnownActiveViewportWithoutFreshFenceEvidence) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, false, false, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

TEST(StreamlineRuntimePolicyTest, FailedOrOptionlessCallsDoNotUpdateRuntimeState) {
    const auto failedUpdate =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(false, true, 1, 1, 3);
    const auto optionlessUpdate =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, false, false, true, false, 1, 1, 3);

    EXPECT_FALSE(failedUpdate.shouldUpdate);
    EXPECT_FALSE(optionlessUpdate.shouldUpdate);
}

TEST(StreamlineRuntimePolicyTest, GetStateSuppressesFreshActivationDuringRecentAuthoritativeTakeover) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, false, true, true, 1, 1, 3);

    EXPECT_FALSE(update.shouldUpdate);
    EXPECT_FALSE(update.active);
}

TEST(StreamlineRuntimePolicyTest, GetStateSuppressionDoesNotDisableAlreadyActiveViewport) {
    const auto update =
        ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromGetState(true, true, true, true, true, 1, 1, 3);

    EXPECT_TRUE(update.shouldUpdate);
    EXPECT_TRUE(update.active);
    EXPECT_EQ(2, update.multiplier);
    EXPECT_EQ(1u, update.generatedFrames);
}

}  // namespace
