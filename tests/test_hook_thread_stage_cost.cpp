#include <gtest/gtest.h>

#include "../hook/common/hook_thread_stage_cost.h"

TEST(HookThreadStageCostWindowTest, OptionalStageCostsAreNotDilutedByIdlePasses) {
    ce::HookThreadStageCostWindow<3> costs;
    costs.Observe(ce::HookThreadStage::kConfig, 30);
    EXPECT_FALSE(costs.FinishPass(100).has_value());
    costs.Observe(ce::HookThreadStage::kHookScan, 6000);
    EXPECT_FALSE(costs.FinishPass(6100).has_value());
    costs.Observe(ce::HookThreadStage::kConfig, 60);
    const auto snapshot = costs.FinishPass(120);
    ASSERT_TRUE(snapshot.has_value());
    if (!snapshot)
        return;

    const auto& snapshotValue = snapshot.value();
    const auto& config =
        snapshotValue.stages[static_cast<std::size_t>(ce::HookThreadStage::kConfig)];
    const auto& hooks =
        snapshotValue.stages[static_cast<std::size_t>(ce::HookThreadStage::kHookScan)];
    EXPECT_EQ(snapshotValue.passes, 3u);
    EXPECT_EQ(snapshotValue.maxUs, 6100u);
    EXPECT_EQ(snapshotValue.over1ms, 1u);
    EXPECT_EQ(config.calls, 2u);
    EXPECT_EQ(ce::AverageHookThreadStageUs(config), 45u);
    EXPECT_EQ(hooks.calls, 1u);
    EXPECT_EQ(hooks.maxUs, 6000u);
    EXPECT_EQ(hooks.over1ms, 1u);
}

TEST(HookThreadStageCostWindowTest, StartsAFreshDisjointWindow) {
    ce::HookThreadStageCostWindow<1> costs;
    costs.Observe(ce::HookThreadStage::kRetirement, 5000);
    ASSERT_TRUE(costs.FinishPass(6000).has_value());

    costs.Observe(ce::HookThreadStage::kRetirement, 4);
    const auto second = costs.FinishPass(10);
    ASSERT_TRUE(second.has_value());
    if (!second)
        return;
    const auto& secondValue = second.value();
    const auto& retirement =
        secondValue.stages[static_cast<std::size_t>(ce::HookThreadStage::kRetirement)];
    EXPECT_EQ(secondValue.maxUs, 10u);
    EXPECT_EQ(retirement.calls, 1u);
    EXPECT_EQ(retirement.maxUs, 4u);
}
