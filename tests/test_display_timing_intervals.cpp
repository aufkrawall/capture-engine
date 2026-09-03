#include "../captureengine/display_timing_intervals.h"
#include <gtest/gtest.h>

namespace {

// Feeds a timestamp series built from the given interval pattern.
void Feed(DisplayIntervalStats& stats, std::initializer_list<int64_t> intervals, int startUs = 1'000'000) {
    int64_t now = startUs;
    stats.Observe(now);
    for (const int64_t interval : intervals) {
        now += interval;
        stats.Observe(now);
    }
}

}  // namespace

TEST(DisplayIntervalStatsTest, ReportsNothingBeforeTwoTimestamps) {
    DisplayIntervalStats stats;
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_EQ(stats.meanUs(), 0);
    EXPECT_EQ(stats.percentileUs(0.5), 0);
    stats.Observe(1'000'000);
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_EQ(stats.jaggednessUs(), 0);
}

TEST(DisplayIntervalStatsTest, AConstantSeriesHasNoSpreadAndNoJaggedness) {
    DisplayIntervalStats stats;
    Feed(stats, {6944, 6944, 6944, 6944, 6944, 6944});
    EXPECT_EQ(stats.count(), 6u);
    EXPECT_EQ(stats.meanUs(), 6944);
    EXPECT_EQ(stats.stdDevUs(), 0);
    EXPECT_EQ(stats.jaggednessUs(), 0);
    EXPECT_EQ(stats.minUs(), 6944);
    EXPECT_EQ(stats.maxUs(), 6944);
    EXPECT_EQ(stats.percentileUs(0.01), 6900);
    EXPECT_EQ(stats.percentileUs(0.99), 6900);
}

// The regression this exists to catch: a two-phase sawtooth at a perfectly
// correct average rate. Mean and count stay right, so only the shape shows it.
TEST(DisplayIntervalStatsTest, ExposesASawtoothWithACorrectMean) {
    DisplayIntervalStats stats;
    Feed(stats, {100, 13'788, 100, 13'788, 100, 13'788, 100, 13'788});
    EXPECT_EQ(stats.count(), 8u);
    EXPECT_EQ(stats.meanUs(), 6944);
    EXPECT_NEAR(static_cast<double>(stats.stdDevUs()), 6844.0, 2.0);
    EXPECT_EQ(stats.jaggednessUs(), 13'688);
    EXPECT_EQ(stats.percentileUs(0.01), 100);
    EXPECT_EQ(stats.percentileUs(0.99), 13'700);
}

TEST(DisplayIntervalStatsTest, SeparatesAlternatingIntervalsThatAveragedCorrectly) {
    DisplayIntervalStats stats;
    Feed(stats, {7070, 6840, 7070, 6840, 7070, 6840});
    EXPECT_EQ(stats.meanUs(), 6955);
    EXPECT_EQ(stats.jaggednessUs(), 230);
    EXPECT_EQ(stats.minUs(), 6840);
    EXPECT_EQ(stats.maxUs(), 7070);
}

TEST(DisplayIntervalStatsTest, IgnoresNonAdvancingTimestampsWithoutLosingThePosition) {
    DisplayIntervalStats stats;
    stats.Observe(1'000'000);
    stats.Observe(1'000'000);  // duplicate publication
    stats.Observe(990'000);    // regression the publisher would have dropped
    stats.Observe(1'006'944);
    EXPECT_EQ(stats.count(), 1u);
    EXPECT_EQ(stats.meanUs(), 6944);
}

TEST(DisplayIntervalStatsTest, AWindowBoundaryIsNotAGap) {
    DisplayIntervalStats stats;
    Feed(stats, {6944, 6944});
    stats.StartWindow();
    EXPECT_EQ(stats.count(), 0u);
    stats.Observe(1'000'000 + 3 * 6944);
    EXPECT_EQ(stats.count(), 1u);
    EXPECT_EQ(stats.meanUs(), 6944);
    // Jaggedness needs two intervals inside the window, so the first one after a
    // restart contributes none rather than a difference against the old window.
    EXPECT_EQ(stats.jaggednessUs(), 0);
}

TEST(DisplayIntervalStatsTest, SaturatesRatherThanOverflowingTheHistogram) {
    DisplayIntervalStats stats;
    Feed(stats, {6944, 5'000'000});
    EXPECT_EQ(stats.count(), 2u);
    EXPECT_EQ(stats.maxUs(), 5'000'000);
    EXPECT_EQ(stats.percentileUs(0.99),
              static_cast<int64_t>(DisplayIntervalStats::kBucketCount - 1) * DisplayIntervalStats::kBucketWidthUs);
}
