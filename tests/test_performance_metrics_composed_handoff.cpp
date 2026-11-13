// A swapchain replacement's hitch in the display-change frame-time series (hook/metrics/performance_metrics.cpp).

#include <gtest/gtest.h>

#include <algorithm>

#include "hook/metrics/performance_metrics.h"

namespace {

constexpr int64_t kRefreshUs = 6945;

float LargestDisplaySampleMs(const PerformanceMetrics& metrics) {
    const std::atomic<float>* history = metrics.GetHistoryArray();
    float largest = 0.0f;
    for (int i = 0; i < PerformanceMetrics::HISTORY_SIZE; ++i)
        largest = (std::max)(largest, history[i].load(std::memory_order_relaxed));
    return largest;
}

// GTA V Enhanced DLSS FG -> FSR FG (session 20261113_181813): the old chain's own flips stop, the game rebuilds its
// swapchain for 556-800 ms, and the new chain's first frames reach the screen through the compositor before it flips
// on its own. The overlay graph showed that hitch while the composed episode put it on the presentation series, and
// lost it when the display series came back: the series dropped the whole stretch, the hitch included.
TEST(PerformanceMetricsComposedHandoffTest, TheHitchBeforeAComposedEpisodeStaysInTheDisplaySeries) {
    PerformanceMetrics metrics;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenUs = 2'000'000;
    for (int i = 0; i < 40; ++i) {
        timing.Publish(screenUs, screenUs);
        screenUs += kRefreshUs;
    }
    metrics.ConsumeDisplayTiming(timing, screenUs);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    ASSERT_LT(LargestDisplaySampleMs(metrics), 8.0f);

    // The replacement: 800 ms without a new frame, then the compositor shows the new chain's first frames.
    screenUs += 800'000 - kRefreshUs;
    for (int i = 0; i < 4; ++i) {
        timing.Publish(screenUs, screenUs, 0, true, 0, true);
        screenUs += kRefreshUs;
    }
    metrics.ConsumeDisplayTiming(timing, screenUs);
    EXPECT_TRUE(metrics.IsDisplayStreamComposed());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);

    // Own flips again: the display series is back, the hitch with it and the composed stretch still not a frame.
    for (int i = 0; i < 40; ++i) {
        timing.Publish(screenUs, screenUs);
        screenUs += kRefreshUs;
    }
    metrics.ConsumeDisplayTiming(timing, screenUs);
    ASSERT_FALSE(metrics.IsDisplayStreamComposed());
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_NEAR(LargestDisplaySampleMs(metrics), 800.0f, 0.01f);
    // 39 own intervals before, the handoff, 39 after the restart.
    EXPECT_EQ(metrics.GetHistoryIndex(), 79);
}

// A stream that starts composed has no own flip to measure from: nothing is invented.
TEST(PerformanceMetricsComposedHandoffTest, AStreamThatStartsComposedAddsNoInterval) {
    PerformanceMetrics metrics;
    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    int64_t screenUs = 2'000'000;
    for (int i = 0; i < 8; ++i) {
        timing.Publish(screenUs, screenUs, 0, true, 0, true);
        screenUs += kRefreshUs;
    }
    screenUs += 500'000;
    for (int i = 0; i < 10; ++i) {
        timing.Publish(screenUs, screenUs);
        screenUs += kRefreshUs;
    }
    metrics.ConsumeDisplayTiming(timing, screenUs);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_EQ(metrics.GetHistoryIndex(), 9);
    EXPECT_LT(LargestDisplaySampleMs(metrics), 8.0f);
}

}  // namespace
