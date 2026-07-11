#include <gtest/gtest.h>

#include "../common/gpu_scheduling_policy.h"

using namespace ce::gpu_scheduling;

TEST(GpuSchedulingPolicyTest, AutomaticPriorityUsesHighOnlyForConfirmedHags) {
    HagsStatus status;
    status.querySucceeded = true;
    status.enabled = true;
    EXPECT_EQ(ResolveAutomaticProcessSchedulingPriority(status), kSchedulingPriorityHigh);

    status.enabled = false;
    EXPECT_EQ(ResolveAutomaticProcessSchedulingPriority(status), kSchedulingPriorityAboveNormal);
}

TEST(GpuSchedulingPolicyTest, AutomaticPriorityFallsBackWhenQueryIsUnavailable) {
    HagsStatus status;
    status.querySucceeded = false;
    status.enabled = true;
    EXPECT_EQ(ResolveAutomaticProcessSchedulingPriority(status), kSchedulingPriorityAboveNormal);

    status.querySucceeded = true;
    status.supportState = HagsSupportState::kUnsupported;
    EXPECT_STREQ(HagsSupportStateName(status.supportState), "unsupported");
    status.supportState = HagsSupportState::kStable;
    EXPECT_STREQ(HagsSupportStateName(status.supportState), "stable");
}
