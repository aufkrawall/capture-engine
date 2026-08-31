#include <gtest/gtest.h>

#include "../captureengine/display_timing_policy.h"
#include "../hook/common/performance_metrics.h"
#include "../hook/common/reflex_defs.h"
#include "../hook/common/system_latency_metrics.h"

namespace {

using ce::system_latency::NativeFrameReport;
using ce::system_latency::NativeReport;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

void FeedPresentationAndDisplay(Tracker& tracker, int64_t firstPresentUs, int64_t intervalUs,
                                int64_t presentToDisplayUs, int count) {
    for (int i = 0; i < count; ++i) {
        const int64_t presentUs = firstPresentUs + intervalUs * i;
        tracker.ObservePresent(presentUs);
        tracker.ObserveDisplay(presentUs + presentToDisplayUs);
    }
}

NativeFrameReport MakeNativeFrame(uint64_t frameId, uint64_t presentUs, uint64_t inputUs = 0) {
    NativeFrameReport frame{};
    frame.frameId = frameId;
    frame.inputSampleTimeUs = inputUs;
    frame.simulationStartTimeUs = presentUs - 8'000;
    frame.presentStartTimeUs = presentUs;
    frame.gpuRenderEndTimeUs = presentUs + 2'000;
    return frame;
}

}  // namespace

TEST(SystemLatencyMetricsTest, FallbackCombinesObservedPresentToDisplayWithCadenceEstimate) {
    Tracker tracker;
    FeedPresentationAndDisplay(tracker, 1'000'000, 10'000, 3'000, 8);

    const auto snapshot = tracker.GetSnapshot(1'073'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.01f);
    EXPECT_GE(snapshot.sampleCount, 6u);
}

TEST(SystemLatencyMetricsTest, DisplayTimingCollectionStaysActiveForLatencyWithPresentationFrameTimes) {
    EXPECT_TRUE(ShouldCollectDisplayTiming(false, FrameTimeSource::Presentation, false, true));
    EXPECT_FALSE(ShouldCollectDisplayTiming(true, FrameTimeSource::Presentation, false, true));
    EXPECT_TRUE(ShouldStartOverlayDisplayTiming(true, true));
    EXPECT_FALSE(ShouldStartOverlayDisplayTiming(false, true));
    EXPECT_FALSE(ShouldStartOverlayDisplayTiming(true, false));
}

TEST(SystemLatencyMetricsTest, OverlayLabelsKeepBothEstimatedSourcesExplicit) {
    EXPECT_STREQ(ce::system_latency::SourceOverlayLabel(Source::Unavailable), "PC Latency");
    EXPECT_STREQ(ce::system_latency::SourceOverlayLabel(Source::ReflexMarkers), "PC Latency~");
    EXPECT_STREQ(ce::system_latency::SourceOverlayLabel(Source::Estimated), "Latency est.");
}

TEST(SystemLatencyMetricsTest, ReflexLatencyResultMatchesNvApiAbi) {
    EXPECT_EQ(sizeof(NV_LATENCY_RESULT_PARAMS::FrameReport), 240u);
    EXPECT_EQ(sizeof(NV_LATENCY_RESULT_PARAMS), 15400u);
    EXPECT_EQ(NV_LATENCY_RESULT_PARAMS_VER, 0x00013C28u);
    EXPECT_EQ(offsetof(NV_LATENCY_RESULT_PARAMS, frameReport), 8u);
    EXPECT_EQ(offsetof(NV_LATENCY_RESULT_PARAMS::FrameReport, inputSampleTime), 8u);
    EXPECT_EQ(offsetof(NV_LATENCY_RESULT_PARAMS::FrameReport, presentStartTime), 48u);
    EXPECT_EQ(offsetof(NV_LATENCY_RESULT_PARAMS::FrameReport, gpuRenderEndTime), 104u);
    EXPECT_EQ(NVAPI_ID_D3D_GetLatency, 0x1A587F9Cu);
}

TEST(SystemLatencyMetricsTest, ReflexMarkersProduceMarkerEnhancedPcLatencyEstimate) {
    Tracker tracker;
    NativeReport report{};
    report.count = 6;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 2'000'000 + 10'000 * i;
        tracker.ObservePresent(static_cast<int64_t>(presentUs));
        tracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs);
    }

    tracker.SubmitNativeReport(report);
    const auto snapshot = tracker.GetSnapshot(2'054'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 6u);
}

TEST(SystemLatencyMetricsTest, ReflexInputSampleDoesNotMasqueradeAsPclInputPing) {
    Tracker tracker;
    NativeReport report{};
    report.count = 6;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 2'500'000 + 10'000 * i;
        tracker.ObservePresent(static_cast<int64_t>(presentUs));
        tracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs, presentUs - 6'000);
    }

    tracker.SubmitNativeReport(report);
    const auto snapshot = tracker.GetSnapshot(2'554'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
}

TEST(SystemLatencyMetricsTest, NativeCorrelationSkipsSupersededDroppedFrame) {
    Tracker tracker;
    tracker.ObserveDisplay(3'010'000);

    NativeReport report{};
    report.count = 2;
    report.frames[0] = MakeNativeFrame(1, 3'000'000);
    report.frames[1] = MakeNativeFrame(2, 3'005'000);
    tracker.SubmitNativeReport(report);

    const auto snapshot = tracker.GetSnapshot(3'010'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 15.5f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 1u);
}

TEST(SystemLatencyMetricsTest, NativeEstimateExtendsInputWaitAcrossDroppedFrames) {
    Tracker tracker;
    NativeReport report{};
    report.count = 30;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 3'500'000 + 5'000 * i;
        report.frames[i] = MakeNativeFrame(i + 1, presentUs);
        if (i % 3 == 2)
            tracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000));
    }

    tracker.SubmitNativeReport(report);
    const auto snapshot = tracker.GetSnapshot(3'649'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 24.5f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 10u);
}

TEST(SystemLatencyMetricsTest, ReflexSimulationMarkersEstimateOnlyMissingInputSamplingSlice) {
    Tracker tracker;
    NativeReport report{};
    report.count = 6;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 4'000'000 + 10'000 * i;
        tracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs);
    }

    tracker.SubmitNativeReport(report);
    const auto snapshot = tracker.GetSnapshot(4'054'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 6u);
}

TEST(SystemLatencyMetricsTest, FreshFallbackReplacesStaleNativeReading) {
    Tracker tracker;
    tracker.ObserveDisplay(1'004'000);
    NativeReport report{};
    report.count = 2;
    report.frames[0] = MakeNativeFrame(1, 990'000, 980'000);
    report.frames[1] = MakeNativeFrame(2, 1'000'000, 988'000);
    tracker.SubmitNativeReport(report);
    ASSERT_EQ(tracker.GetSnapshot(1'004'000).source, Source::ReflexMarkers);

    FeedPresentationAndDisplay(tracker, 4'000'000, 10'000, 3'000, 6);
    const auto snapshot = tracker.GetSnapshot(4'053'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.01f);
}

TEST(SystemLatencyMetricsTest, IncompatibleNativeClockDomainCannotOverrideFallback) {
    Tracker tracker;
    FeedPresentationAndDisplay(tracker, 4'500'000, 10'000, 3'000, 8);

    NativeReport report{};
    report.count = 4;
    for (size_t i = 0; i < report.count; ++i)
        report.frames[i] = MakeNativeFrame(i + 1, 90'000'000 + 10'000 * i);
    tracker.SubmitNativeReport(report);

    const auto snapshot = tracker.GetSnapshot(4'573'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.01f);
}

TEST(SystemLatencyMetricsTest, SubTenFpsInputWaitHeuristicFailsClosed) {
    Tracker fallbackTracker;
    FeedPresentationAndDisplay(fallbackTracker, 4'800'000, 125'000, 3'000, 4);
    EXPECT_FALSE(fallbackTracker.GetSnapshot(5'178'000).valid);

    Tracker nativeTracker;
    NativeReport report{};
    report.count = 4;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 5'300'000 + 125'000 * i;
        nativeTracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs);
    }
    nativeTracker.SubmitNativeReport(report);
    EXPECT_FALSE(nativeTracker.GetSnapshot(5'679'000).valid);
}

TEST(SystemLatencyMetricsTest, FrameGenerationFallbackUsesBaseCadence) {
    Tracker tracker;
    tracker.SetFrameGeneration(100.0f, 2);
    FeedPresentationAndDisplay(tracker, 5'000'000, 5'000, 2'000, 8);

    const auto snapshot = tracker.GetSnapshot(5'037'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
}

TEST(SystemLatencyMetricsTest, FallbackExtendsInputWaitAcrossSupersededPresents) {
    Tracker tracker;
    for (int i = 0; i < 30; ++i)
        tracker.ObservePresent(5'500'000 + 5'000 * i);
    for (int i = 2; i < 30; i += 3) {
        const int64_t presentUs = 5'500'000 + 5'000 * i;
        tracker.ObserveDisplay(presentUs + 3'000);
    }

    const auto snapshot = tracker.GetSnapshot(5'648'000);

    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 20.5f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 10u);
}

TEST(SystemLatencyMetricsTest, ResetDropsMeasurementsFromPreviousDisplayGeneration) {
    Tracker tracker;
    FeedPresentationAndDisplay(tracker, 6'000'000, 10'000, 3'000, 6);
    ASSERT_TRUE(tracker.GetSnapshot(6'053'000).valid);

    tracker.ResetDisplayHistory();

    const auto snapshot = tracker.GetSnapshot(6'053'000);
    EXPECT_FALSE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Unavailable);
}

TEST(SystemLatencyMetricsTest, PerformanceMetricsFeedsPresentationAndDisplayCorrelation) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 900'000);  // Establish this publication generation.

    for (int i = 0; i < 6; ++i) {
        const int64_t presentUs = 1'000'000 + 10'000 * i;
        metrics.Update(presentUs);
        timing.Publish(presentUs + 3'000, presentUs + 3'500);
    }
    metrics.ConsumeDisplayTiming(timing, 1'054'000);

    const auto snapshot = metrics.GetSystemLatency(1'053'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.01f);
}

TEST(SystemLatencyMetricsTest, PerformanceMetricsAcceptsNativeMarkerReport) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 1'900'000);

    NativeReport report{};
    report.count = 4;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 2'000'000 + 10'000 * i;
        timing.Publish(static_cast<int64_t>(presentUs + 4'000), static_cast<int64_t>(presentUs + 4'500));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs, presentUs - 6'000);
    }
    metrics.ConsumeDisplayTiming(timing, 2'035'000);
    metrics.SubmitNativeLatencyReport(report);

    const auto snapshot = metrics.GetSystemLatency(2'034'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
}
