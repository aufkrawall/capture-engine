#include <gtest/gtest.h>

#include "../hook/common/dxgi_video_memory_log_policy.h"

namespace policy = ce::dxgi_video_memory_log_policy;

TEST(DxgiVideoMemoryLogPolicyTest, LogsInitialSamplesThenThrottlesSteadyState) {
    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(1, 100, 0, false, false));
    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(policy::kFirstVideoMemoryQueryLogs, 100, 1, false, false));

    EXPECT_FALSE(policy::ShouldLogVideoMemoryQuery(policy::kFirstVideoMemoryQueryLogs + 1, 500, 100, false, false));
    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(policy::kFirstVideoMemoryQueryLogs + 2, 1100, 100, false, false));
    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(policy::kSuccessPeriodicCallInterval, 500, 100, false, false));
}

TEST(DxgiVideoMemoryLogPolicyTest, LogsMeaningfulMemoryChangesAndFailuresMoreOften) {
    EXPECT_TRUE(policy::HasMeaningfulVideoMemoryDelta(1024, 1024, 1024, 1024 + policy::kMeaningfulBudgetDeltaBytes,
                                                      1024, 1024));
    EXPECT_FALSE(policy::HasMeaningfulVideoMemoryDelta(1024, 1024, 1024, 2048, 2048, 2048));

    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(500, 200, 100, true, false));
    EXPECT_TRUE(policy::ShouldLogVideoMemoryQuery(policy::kFailurePeriodicCallInterval, 200, 100, false, true));
}
