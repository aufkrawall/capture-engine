#include "hook/metrics/performance_metrics.h"

#include <gtest/gtest.h>

namespace {
using State = PerformanceMetrics::FrameTimingState;

TEST(FrameTimingStateTest, StartupUsesTheFirstActualIntervalWithoutAStartupGapFrame) {
    PerformanceMetrics metrics;
    metrics.Update(1'000'000);
    metrics.Update(1'001'000);  // warm presentation history must not mean display is ready
    SharedDisplayTiming timing;
    timing.Reset(42, 0, DisplayTimingStatus::Starting);
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3'000'000);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::DisplayStarting);

    timing.Publish(2'000'000, 3'000'000);
    metrics.ConsumeDisplayTiming(timing, 3'000'000);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::DisplayStarting);
    EXPECT_FALSE(metrics.HasDisplayTimingSamples());
    timing.Publish(2'008'000, 3'001'000);
    metrics.ConsumeDisplayTiming(timing, 3'001'000);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::DisplayActive);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_EQ(metrics.GetSampleCount(), 1u);
    EXPECT_FLOAT_EQ(metrics.GetLastDisplayFrameTimeMs(), 8.0f);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 125.0f, 0.01f);
}

TEST(FrameTimingStateTest, ReasonsDistinguishCompositionStalenessAndCollectionFailure) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(42, 0, DisplayTimingStatus::Starting);
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    timing.Publish(2'000'000, 3'000'000, 0, true, 0, true);
    metrics.ConsumeDisplayTiming(timing, 3'000'000);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::Composed);
    metrics.ConsumeDisplayTiming(timing, 5'000'001);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::Stale);
    for (const auto status : {DisplayTimingStatus::Unavailable, DisplayTimingStatus::AccessDenied,
                              DisplayTimingStatus::Failed}) {
        timing.SetStatus(status);
        metrics.ConsumeDisplayTiming(timing, 5'000'002);
        EXPECT_EQ(metrics.GetFrameTimingState(), State::Unavailable);
        EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
    }
    metrics.SetFrameTimeSource(FrameTimeSource::Presentation);
    EXPECT_EQ(metrics.GetFrameTimingState(), State::Presentation);
}

TEST(FrameTimingStateTest, NewDisplayGenerationHasAnIdentityIndependentOfItsSampleCount) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    timing.Reset(42, 0, DisplayTimingStatus::Starting);
    timing.Publish(2'000'000, 3'000'000);
    timing.Publish(2'007'000, 3'000'000);
    metrics.ConsumeDisplayTiming(timing, 3'000'000);
    const auto first = metrics.GetHistoryIdentity();
    EXPECT_NE(first, 0u);
    timing.Reset(42, 0, DisplayTimingStatus::Starting);
    for (int i = 0; i < 100; ++i)
        timing.Publish(4'000'000 + i * 8'000, 5'000'000);
    metrics.ConsumeDisplayTiming(timing, 5'000'000);
    EXPECT_NE(metrics.GetHistoryIdentity(), first);
    EXPECT_EQ(metrics.GetSampleCount(), 99u);
    EXPECT_FLOAT_EQ(metrics.GetLastDisplayFrameTimeMs(), 8.0f);
    metrics.SetFrameTimeSource(FrameTimeSource::Presentation);
    EXPECT_EQ(metrics.GetHistoryIdentity(), 0u);
}
}  // namespace
