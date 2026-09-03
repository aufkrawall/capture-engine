#include "../hook/common/hook_cpu_cost.h"
#include <gtest/gtest.h>

TEST(HookCpuCostTest, ReportsNothingBeforeAnyCall) {
    HookCpuCost cost;
    EXPECT_EQ(cost.calls(), 0u);
    EXPECT_EQ(cost.cycles(), 0u);
    EXPECT_EQ(cost.averageCycles(), 0u);
    EXPECT_EQ(cost.maxCycles(), 0u);
}

TEST(HookCpuCostTest, AveragesAndKeepsTheWorstCall) {
    HookCpuCost cost;
    cost.Observe(100);
    cost.Observe(300);
    cost.Observe(200);
    EXPECT_EQ(cost.calls(), 3u);
    EXPECT_EQ(cost.cycles(), 600u);
    EXPECT_EQ(cost.averageCycles(), 200u);
    EXPECT_EQ(cost.maxCycles(), 300u);
}

// The distinction the whole measurement rests on: a hook that forwards into the
// runtime must not be charged for the runtime's own work.
TEST(HookCpuCostTest, ForwardedCyclesAreExcludedFromTheHooksOwnCost) {
    HookCpuCost cost;
    {
        ScopedHookCpuCost scope(cost, true);
        HookForwardedCycles() += 100'000'000;  // as a forwarded runtime call would
    }
    // The scope's own span is a few counter reads, far below the forwarded
    // cycles, so the hook is charged nothing rather than a negative amount.
    EXPECT_EQ(cost.calls(), 1u);
    EXPECT_EQ(cost.cycles(), 0u);
}

TEST(HookCpuCostTest, ANestedScopeDoesNotStealTheOuterScopesForwardedCycles) {
    HookCpuCost outerSink;
    HookCpuCost innerSink;
    HookForwardedCycles() = 0;
    {
        ScopedHookCpuCost outer(outerSink, true);
        HookForwardedCycles() += 500;
        {
            ScopedHookCpuCost inner(innerSink, true);
            HookForwardedCycles() += 700;
        }
        // The inner scope restored the outer scope's tally instead of clearing it.
        EXPECT_EQ(HookForwardedCycles(), 500u);
    }
    EXPECT_EQ(outerSink.calls(), 1u);
    EXPECT_EQ(innerSink.calls(), 1u);
}

TEST(HookCpuCostTest, ForwardedTallyIsRestoredAfterTheOutermostScope) {
    HookCpuCost sink;
    HookForwardedCycles() = 42;
    {
        ScopedHookCpuCost scope(sink, true);
        HookForwardedCycles() += 10;
    }
    EXPECT_EQ(HookForwardedCycles(), 42u);
}
