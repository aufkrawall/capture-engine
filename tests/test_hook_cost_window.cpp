#include <gtest/gtest.h>

#include "../hook/common/hook_cost_window.h"

TEST(HookCostWindowTest, ExcludesForwardedTimeAndCountsOwnStalls) {
    ce::HookCostWindow<3> cost;
    EXPECT_FALSE(cost.Observe(10'100, 10'000));
    EXPECT_FALSE(cost.Observe(10'600, 10'000));
    const auto result = cost.Observe(11'500, 10'000);
    ASSERT_TRUE(result.has_value());
    const ce::HookCostSnapshot snapshot = result.value_or(ce::HookCostSnapshot{});
    EXPECT_EQ(snapshot.calls, 3u);
    EXPECT_EQ(snapshot.selfUs, 2200u);
    EXPECT_EQ(snapshot.wrappedUs, 30'000u);
    EXPECT_EQ(snapshot.selfMaxUs, 1500u);
    EXPECT_EQ(snapshot.over500Us, 2u);
    EXPECT_EQ(snapshot.over1ms, 1u);
}

TEST(HookCostWindowTest, StartupMaximumDoesNotMaskLaterWindows) {
    ce::HookCostWindow<2> cost;
    EXPECT_FALSE(cost.Observe(15'000, 0));
    const auto startup = cost.Observe(100, 0);
    ASSERT_TRUE(startup.has_value());
    EXPECT_EQ(startup.value_or(ce::HookCostSnapshot{}).selfMaxUs, 15'000u);
    EXPECT_FALSE(cost.Observe(600, 0));
    const auto steady = cost.Observe(700, 0);
    ASSERT_TRUE(steady.has_value());
    const ce::HookCostSnapshot settled = steady.value_or(ce::HookCostSnapshot{});
    EXPECT_EQ(settled.selfMaxUs, 700u);
    EXPECT_EQ(settled.over1ms, 0u);
    EXPECT_EQ(settled.selfUs, 1300u);
}

TEST(HookCostWindowTest, IndependentlyOwnedWindowsNeverResetEachOther) {
    ce::HookCostWindow<2> first, second;
    EXPECT_FALSE(first.Observe(500, 0));
    EXPECT_FALSE(second.Observe(2000, 0));
    EXPECT_EQ(first.Observe(100, 0).value_or(ce::HookCostSnapshot{}).selfUs, 600u);
    EXPECT_EQ(second.Observe(3000, 0).value_or(ce::HookCostSnapshot{}).selfUs, 5000u);
}
