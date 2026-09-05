#include <gtest/gtest.h>

#include "../captureengine/display_timing_vblank.h"

namespace {
constexpr int64_t kStart = 1'000'000;
constexpr int64_t kPeriod = 6946;

TEST(VerticalBlankClockTest, RequiresObservedCadence) {
    VerticalBlankClock clock;
    EXPECT_EQ(clock.PeriodUs(0), 0);
    EXPECT_FALSE(clock.HasPeriodicCadence(0));
    clock.Observe(0, kStart);
    EXPECT_EQ(clock.observedBlanks(0), 1u);
    EXPECT_FALSE(clock.HasPeriodicCadence(0));
}

TEST(VerticalBlankClockTest, MeasuresFixedCadenceAcrossRingWrapAndReportingJitter) {
    VerticalBlankClock clock;
    for (int i = 0; i < 100; ++i)
        clock.Observe(0, kStart + i * kPeriod + (i % 2 == 0 ? 10 : -10));
    EXPECT_TRUE(clock.HasPeriodicCadence(0));
    EXPECT_NEAR(clock.PeriodUs(0), kPeriod, 2);
    EXPECT_EQ(clock.observedBlanks(0), 100u);
}

TEST(VerticalBlankClockTest, ReportsSparsePeriodicBlanksWithoutInventingObservations) {
    VerticalBlankClock clock;
    int64_t time = kStart;
    for (int i = 0; i < 60; ++i) {
        clock.Observe(0, time);
        time += (i % 3 + 1) * kPeriod;
    }
    EXPECT_TRUE(clock.HasPeriodicCadence(0));
    EXPECT_EQ(clock.PeriodUs(0), kPeriod);
    EXPECT_EQ(clock.observedBlanks(0), 60u);
}

TEST(VerticalBlankClockTest, FollowsVariableRefreshAfterLeavingTheCap) {
    VerticalBlankClock clock;
    int64_t time = kStart;
    for (int i = 0; i < 60; ++i) {
        clock.Observe(0, time);
        time += kPeriod;
    }
    ASSERT_TRUE(clock.HasPeriodicCadence(0));
    for (int i = 0; i < 60; ++i) {
        time += i % 2 == 0 ? 9000 : 14000;
        clock.Observe(0, time);
    }
    EXPECT_FALSE(clock.HasPeriodicCadence(0));
    EXPECT_EQ(clock.PeriodUs(0), 0);
}

TEST(VerticalBlankClockTest, KeepsDisplaysApartAndIgnoresNonadvancingReports) {
    VerticalBlankClock clock;
    for (int i = 0; i < 60; ++i) {
        clock.Observe(3, kStart + i * kPeriod);
        clock.Observe(7, kStart + i * 16667);
    }
    clock.Observe(3, kStart);
    clock.Observe(3, 0);
    EXPECT_EQ(clock.observedBlanks(3), 60u);
    EXPECT_EQ(clock.PeriodUs(3), kPeriod);
    EXPECT_EQ(clock.PeriodUs(7), 16667);
    clock.Observe(7, kStart + 60 * 16667);
    EXPECT_EQ(clock.busiestSource(), 7u);
    clock.Clear();
    EXPECT_EQ(clock.observedBlanks(7), 0u);
    EXPECT_FALSE(clock.HasPeriodicCadence(7));
}
}  // namespace
