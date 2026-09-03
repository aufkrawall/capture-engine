#include "../hook/common/hook_cpu_cost.h"
#include <gtest/gtest.h>

TEST(HookCpuCostTest, ReportsNothingBeforeAnyCall) {
    HookCpuCost cost;
    EXPECT_EQ(cost.calls(), 0u);
    EXPECT_EQ(cost.cycles(), 0u);
    EXPECT_EQ(cost.averageCycles(), 0u);
    EXPECT_EQ(cost.maxCycles(), 0u);
    EXPECT_EQ(cost.wallUs(), 0u);
    EXPECT_EQ(cost.averageWallUs(), 0u);
    EXPECT_EQ(cost.maxWallUs(), 0u);
}

TEST(HookCpuCostTest, AveragesAndKeepsTheWorstCall) {
    HookCpuCost cost;
    cost.Observe(100, 7);
    cost.Observe(300, 5);
    cost.Observe(200, 9);
    EXPECT_EQ(cost.calls(), 3u);
    EXPECT_EQ(cost.cycles(), 600u);
    EXPECT_EQ(cost.averageCycles(), 200u);
    EXPECT_EQ(cost.maxCycles(), 300u);
    EXPECT_EQ(cost.wallUs(), 21u);
    EXPECT_EQ(cost.averageWallUs(), 7u);
    EXPECT_EQ(cost.maxWallUs(), 9u);
}

// Wall time and cycles peak on different calls when a hook blocks: the blocking
// call burns the fewest cycles and holds the thread the longest, so the two
// maxima must be tracked independently or the blocking call is invisible.
TEST(HookCpuCostTest, WallTimeAndCyclesPeakIndependently) {
    HookCpuCost cost;
    cost.Observe(1'000'000, 3);   // busy: many cycles, short wall time
    cost.Observe(50, 4'000);      // blocked: almost no cycles, long wall time
    EXPECT_EQ(cost.maxCycles(), 1'000'000u);
    EXPECT_EQ(cost.maxWallUs(), 4'000u);
    EXPECT_EQ(cost.averageCycles(), 500'025u);
    EXPECT_EQ(cost.averageWallUs(), 2'001u);
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

// The same exclusion has to hold for wall time, or every hook that forwards a
// blocking Present reads as if IT were the thing blocking.
TEST(HookCpuCostTest, ForwardedWallTimeIsExcludedFromTheHooksOwnCost) {
    HookCpuCost cost;
    {
        ScopedHookCpuCost scope(cost, true);
        // A forwarded call that blocked far longer than this scope can have run.
        HookForwardedWallTicks() += 1'000'000'000;
    }
    EXPECT_EQ(cost.calls(), 1u);
    EXPECT_EQ(cost.wallUs(), 0u);
}

TEST(HookCpuCostTest, ANestedScopeRestoresTheOuterScopesForwardedWallTicks) {
    HookCpuCost outerSink;
    HookCpuCost innerSink;
    HookForwardedWallTicks() = 0;
    {
        ScopedHookCpuCost outer(outerSink, true);
        HookForwardedWallTicks() += 500;
        {
            ScopedHookCpuCost inner(innerSink, true);
            HookForwardedWallTicks() += 700;
        }
        EXPECT_EQ(HookForwardedWallTicks(), 500);
    }
    EXPECT_EQ(HookForwardedWallTicks(), 0);
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
