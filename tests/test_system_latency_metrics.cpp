#include <gtest/gtest.h>

#include "../captureengine/display_timing_policy.h"
#include "../hook/common/performance_metrics.h"
#include "../hook/common/reflex_defs.h"
#include "../hook/common/streamline_pcl_latency.h"
#include "../hook/common/system_latency_frame_begin.h"
#include "../hook/common/system_latency_metrics.h"
#include "../hook/common/system_latency_windows.h"

namespace {

using ce::system_latency::FrameBeginKind;
using ce::system_latency::NativeFrameReport;
using ce::system_latency::NativeReport;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

// Feeds a render-ahead pipeline: the game keeps `queueDepth` frames in flight,
// so by the time a frame is scanned out it has already presented the frames
// behind it. Every display carries the sensor's association with the runtime
// PresentStart of the frame that actually produced it.
void FeedRenderAheadPipeline(Tracker& tracker, int64_t firstPresentUs, int64_t intervalUs, int queueDepth,
                             int frameCount, int64_t frameBeginLeadUs = 0,
                             FrameBeginKind frameBeginKind = FrameBeginKind::Modelled) {
    const int64_t presentToDisplayUs = intervalUs * queueDepth;
    for (int i = 0; i < frameCount; ++i) {
        const int64_t presentUs = firstPresentUs + intervalUs * i;
        const int64_t frameBeginUs = frameBeginLeadUs > 0 ? presentUs - frameBeginLeadUs : 0;
        tracker.ObservePresent(presentUs, frameBeginUs, frameBeginKind);
        const int displayedFrame = i - queueDepth;
        if (displayedFrame < 0)
            continue;
        const int64_t displayedPresentUs = firstPresentUs + intervalUs * displayedFrame;
        // The runtime records PresentStart inside the Present call the wrapper
        // already observed, so it lands just after the hook's timestamp.
        tracker.ObserveDisplay(displayedPresentUs + presentToDisplayUs, displayedPresentUs + 200);
    }
}

struct FrameBeginClockReset {
    FrameBeginClockReset() {
        ce::system_latency::FrameBeginClock::Get().Reset();
    }
    ~FrameBeginClockReset() {
        ce::system_latency::FrameBeginClock::Get().Reset();
    }
};

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

bool BuildTestSupplementalReport(NativeReport& report) {
    report = {};
    report.frames[0] = MakeNativeFrame(91, 2'000'000);
    report.count = 1;
    return true;
}

struct SupplementalProviderReset {
    ~SupplementalProviderReset() {
        ce::system_latency::SetSupplementalNativeReportProvider(nullptr);
    }
};

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

TEST(SystemLatencyMetricsTest, SupplementalPclProviderPrecedesDeviceAndNvApiRequirements) {
    SupplementalProviderReset reset;
    ce::system_latency::SetSupplementalNativeReportProvider(BuildTestSupplementalReport);

    NativeReport report{};
    ASSERT_TRUE(ce::system_latency::QueryNativeReport(nullptr, report));
    ASSERT_EQ(report.count, 1u);
    EXPECT_EQ(report.frames[0].frameId, 91u);
}

TEST(SystemLatencyMetricsTest, StreamlinePclMarkersBuildChronologicalNativeReport) {
    ce::system_latency::PclMarkerHistory history;
    history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, 42, 1'000'000);
    history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, 43, 1'010'000);
    history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, 43, 1'018'000);
    history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, 42, 1'008'000);

    NativeReport report{};
    ASSERT_TRUE(history.BuildReport(report));
    ASSERT_EQ(report.count, 2u);
    EXPECT_EQ(report.frames[0].frameId, 42u);
    EXPECT_EQ(report.frames[0].simulationStartTimeUs, 1'000'000u);
    EXPECT_EQ(report.frames[0].presentStartTimeUs, 1'008'000u);
    EXPECT_EQ(report.frames[1].frameId, 43u);
}

TEST(SystemLatencyMetricsTest, StreamlinePclMarkersRejectIncompleteAndReversedPairs) {
    ce::system_latency::PclMarkerHistory history;
    EXPECT_FALSE(history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, 1, 1'000'000));
    EXPECT_TRUE(history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, 2, 1'010'000));
    EXPECT_FALSE(history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, 2, 1'009'000));
    EXPECT_FALSE(history.Record(99, 2, 1'020'000));

    NativeReport report{};
    EXPECT_FALSE(history.BuildReport(report));
    EXPECT_EQ(report.count, 0u);
}

TEST(SystemLatencyMetricsTest, StreamlinePclMarkerReportExpiresAfterFreshnessWindow) {
    ce::system_latency::PclMarkerHistory history;
    history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, 7, 1'000'000);
    history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, 7, 1'008'000);

    NativeReport report{};
    EXPECT_TRUE(history.BuildFreshReport(report, 3'008'000));
    EXPECT_FALSE(history.BuildFreshReport(report, 3'008'001));
    EXPECT_EQ(report.count, 0u);
    EXPECT_FALSE(history.BuildFreshReport(report, 1'007'999));
}

TEST(SystemLatencyMetricsTest, StreamlinePclHistoryRetainsNewestReportCapacityAcrossSlotReuse) {
    ce::system_latency::PclMarkerHistory history;
    for (uint64_t frameId = 1; frameId <= 140; ++frameId) {
        const int64_t simulationUs = 2'000'000 + static_cast<int64_t>(frameId) * 10'000;
        history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, frameId, simulationUs);
        history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, frameId, simulationUs + 8'000);
    }

    NativeReport report{};
    ASSERT_TRUE(history.BuildReport(report));
    ASSERT_EQ(report.count, NativeReport::kCapacity);
    EXPECT_EQ(report.frames.front().frameId, 77u);
    EXPECT_EQ(report.frames[report.count - 1].frameId, 140u);
}

TEST(SystemLatencyMetricsTest, StreamlinePclReportSelectsMarkerEnhancedOverlaySource) {
    ce::system_latency::PclMarkerHistory history;
    Tracker tracker;
    for (uint64_t frameId = 1; frameId <= 6; ++frameId) {
        const int64_t simulationUs = 3'000'000 + static_cast<int64_t>(frameId - 1) * 10'000;
        const int64_t presentUs = simulationUs + 8'000;
        history.Record(ce::system_latency::PclMarkerHistory::kSimulationStartMarker, frameId, simulationUs);
        history.Record(ce::system_latency::PclMarkerHistory::kPresentStartMarker, frameId, presentUs);
        tracker.ObserveDisplay(presentUs + 4'000);
    }

    NativeReport report{};
    ASSERT_TRUE(history.BuildReport(report));
    tracker.SubmitNativeReport(report);
    const auto snapshot = tracker.GetSnapshot(3'062'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
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

TEST(SystemLatencyMetricsTest, NativeCorrelationUsesAssociatedPresentUnderAsyncFrameGeneration) {
    Tracker tracker;
    // Frame 2 has already submitted by the time frame 1 reaches the screen.
    // The display service's runtime-Present association is the causal bound;
    // screen time alone would incorrectly select frame 2 and undercount 10 ms.
    tracker.ObserveDisplay(3'030'000, 3'001'000);

    NativeReport report{};
    report.count = 2;
    report.frames[0] = MakeNativeFrame(1, 3'000'000);
    report.frames[1] = MakeNativeFrame(2, 3'010'000);
    tracker.SubmitNativeReport(report);

    const auto snapshot = tracker.GetSnapshot(3'030'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 43.0f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 1u);
}

TEST(SystemLatencyMetricsTest, NativeMarkerCadenceOverridesNominalFrameGenerationMetadata) {
    Tracker tracker;
    // Native markers are emitted only for application-rendered frames, so a
    // measured marker cadence is authoritative even if nominal FG telemetry
    // is stale or briefly publishes a different inferred base rate.
    tracker.SetFrameGeneration(50.0f, 2);

    NativeReport report{};
    report.count = 6;
    for (size_t i = 0; i < report.count; ++i) {
        const uint64_t presentUs = 3'200'000 + 10'000 * i;
        tracker.ObserveDisplay(static_cast<int64_t>(presentUs + 4'000),
                               static_cast<int64_t>(presentUs + 500));
        report.frames[i] = MakeNativeFrame(i + 1, presentUs);
    }
    tracker.SubmitNativeReport(report);

    const auto snapshot = tracker.GetSnapshot(3'254'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 17.0f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 6u);
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

TEST(SystemLatencyMetricsTest, PerformanceMetricsCarriesDisplayPresentAssociationToMarkerMatcher) {
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 2'900'000);

    timing.Publish(3'030'000, 3'031'000, 3'001'000);
    metrics.ConsumeDisplayTiming(timing, 3'030'000);

    NativeReport report{};
    report.count = 2;
    report.frames[0] = MakeNativeFrame(1, 3'000'000);
    report.frames[1] = MakeNativeFrame(2, 3'010'000);
    metrics.SubmitNativeLatencyReport(report);

    const auto snapshot = metrics.GetSystemLatency(3'030'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
    EXPECT_NEAR(snapshot.milliseconds, 43.0f, 0.01f);
    EXPECT_EQ(snapshot.sampleCount, 1u);
}

TEST(SystemLatencyMetricsTest, RenderAheadQueueIsMeasuredThroughThePresentAssociation) {
    // Regression: matching a displayed transition to the newest Present that
    // preceded it collapses every render-ahead depth to about one frame,
    // because the game has already queued the frames behind the one on screen.
    // That is exactly the latency a low-latency mode removes, so without the
    // sensor's association the estimate cannot tell the two configurations
    // apart at equal frame rate.
    Tracker tracker;
    FeedRenderAheadPipeline(tracker, 1'000'000, 16'667, /*queueDepth=*/3, /*frameCount=*/20);

    const auto snapshot = tracker.GetSnapshot(1'000'000 + 16'667 * 20);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    // 50.0 ms queue + 16.7 ms modelled CPU frame + 8.3 ms average input wait.
    EXPECT_NEAR(snapshot.milliseconds, 75.0f, 0.1f);

    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastPresentToDisplayUs, 50'001);
    EXPECT_EQ(diagnostics.displaysWithPresentAssociation, diagnostics.displaysObserved);
}

TEST(SystemLatencyMetricsTest, ShallowerQueueReportsLowerLatencyAtIdenticalFrameRate) {
    // The ordering the overlay has to get right: same cadence, different render
    // queue depth. Each frame of queue must show up as a frame of latency.
    Tracker deepTracker;
    FeedRenderAheadPipeline(deepTracker, 1'000'000, 16'667, /*queueDepth=*/3, /*frameCount=*/20);
    Tracker shallowTracker;
    FeedRenderAheadPipeline(shallowTracker, 1'000'000, 16'667, /*queueDepth=*/1, /*frameCount=*/20);

    const auto deep = deepTracker.GetSnapshot(1'000'000 + 16'667 * 20);
    const auto shallow = shallowTracker.GetSnapshot(1'000'000 + 16'667 * 20);
    ASSERT_TRUE(deep.valid);
    ASSERT_TRUE(shallow.valid);
    EXPECT_NEAR(deep.milliseconds - shallow.milliseconds, 33.3f, 0.1f);
}

TEST(SystemLatencyMetricsTest, MeasuredFrameBeginReplacesTheModelledFrameOfCpuWork) {
    // A low-latency runtime holds the game back so that simulation starts late
    // in the interval instead of immediately after the previous Present. With
    // the boundary observed, that shortening is measured rather than modelled
    // away as a fixed full interval.
    Tracker modelledTracker;
    FeedRenderAheadPipeline(modelledTracker, 2'000'000, 16'667, /*queueDepth=*/1, /*frameCount=*/20);
    Tracker anchoredTracker;
    FeedRenderAheadPipeline(anchoredTracker, 2'000'000, 16'667, /*queueDepth=*/1, /*frameCount=*/20,
                            /*frameBeginLeadUs=*/4'000, FrameBeginKind::LowLatencySleepReturn);

    const int64_t nowUs = 2'000'000 + 16'667 * 20;
    const auto modelled = modelledTracker.GetSnapshot(nowUs);
    const auto anchored = anchoredTracker.GetSnapshot(nowUs);
    ASSERT_TRUE(modelled.valid);
    ASSERT_TRUE(anchored.valid);
    EXPECT_NEAR(modelled.milliseconds, 41.7f, 0.1f);
    EXPECT_NEAR(anchored.milliseconds, 29.0f, 0.1f);

    const auto diagnostics = anchoredTracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.lastAnchorToPresentUs, 4'000);
    EXPECT_EQ(diagnostics.lastBaseIntervalUs, 16'667);
    EXPECT_EQ(diagnostics.lastFrameBeginKind, FrameBeginKind::LowLatencySleepReturn);
    EXPECT_STREQ(ce::system_latency::FrameBeginKindLabel(diagnostics.lastFrameBeginKind), "low-latency-sleep");
}

TEST(SystemLatencyMetricsTest, FrameBeginCadenceMakesTheEstimateIndependentOfFrameGenerationTelemetry) {
    // Frame-generation base-rate telemetry is published asynchronously and is
    // briefly zero across every transition, which used to collapse the modelled
    // input-sampling interval onto the generated output cadence. Frame-begin
    // boundaries occur once per application frame, so the same stream must
    // produce the same number whatever the telemetry currently says.
    const auto feed = [](Tracker& tracker, float baseFps) {
        tracker.SetFrameGeneration(baseFps, 2);
        for (int i = 0; i < 24; ++i) {
            const int64_t applicationUs = 3'000'000 + 16'667 * i;
            const int64_t frameBeginUs = applicationUs - 3'000;
            tracker.ObservePresent(applicationUs, frameBeginUs, FrameBeginKind::PresentReturn);
            // The generated present carries no simulation of its own, so it
            // reuses the application frame's boundary.
            tracker.ObservePresent(applicationUs + 8'333, frameBeginUs, FrameBeginKind::PresentReturn);
            tracker.ObserveDisplay(applicationUs + 4'000, applicationUs + 200);
            tracker.ObserveDisplay(applicationUs + 12'333, applicationUs + 8'533);
        }
    };

    Tracker staleTelemetryTracker;
    feed(staleTelemetryTracker, 0.0f);
    Tracker publishedTelemetryTracker;
    feed(publishedTelemetryTracker, 60.0f);

    const int64_t nowUs = 3'000'000 + 16'667 * 24;
    const auto stale = staleTelemetryTracker.GetSnapshot(nowUs);
    const auto published = publishedTelemetryTracker.GetSnapshot(nowUs);
    ASSERT_TRUE(stale.valid);
    ASSERT_TRUE(published.valid);
    EXPECT_FLOAT_EQ(stale.milliseconds, published.milliseconds);
    EXPECT_EQ(staleTelemetryTracker.GetDiagnostics().lastBaseIntervalUs, 16'667);
}

TEST(SystemLatencyMetricsTest, DiagnosticsReportUnassociatedAndUnmatchedDisplays) {
    Tracker tracker;
    tracker.ObservePresent(4'000'000);
    tracker.ObservePresent(4'016'667);
    // Older than every observed present: nothing can be attributed to it.
    tracker.ObserveDisplay(3'900'000, 3'899'000);
    tracker.ObserveDisplay(4'020'000);

    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_EQ(diagnostics.displaysObserved, 2u);
    EXPECT_EQ(diagnostics.displaysWithPresentAssociation, 1u);
    EXPECT_EQ(diagnostics.displaysWithoutMatchedPresent, 1u);
}

TEST(SystemLatencyMetricsTest, CrossCheckReportsTheSourceThatWasNotPublished) {
    Tracker tracker;
    FeedRenderAheadPipeline(tracker, 5'000'000, 10'000, /*queueDepth=*/1, /*frameCount=*/12);
    NativeReport report{};
    report.count = 6;
    for (size_t i = 0; i < report.count; ++i)
        report.frames[i] = MakeNativeFrame(i + 1, 5'000'000 + 10'000 * i);
    tracker.SubmitNativeReport(report);

    const int64_t nowUs = 5'000'000 + 10'000 * 12;
    ASSERT_EQ(tracker.GetSnapshot(nowUs).source, Source::ReflexMarkers);
    const auto diagnostics = tracker.GetDiagnostics(nowUs);
    EXPECT_EQ(diagnostics.crossCheckSource, Source::Estimated);
    EXPECT_GT(diagnostics.crossCheckMilliseconds, 0.0f);
}

TEST(SystemLatencyMetricsTest, SampleWindowTrimsMisPairedOutliersFromBothTails) {
    ce::system_latency::SampleWindow window;
    int64_t sampleTimeUs = 1'000'000;
    for (int i = 0; i < 28; ++i)
        window.Add(20.0f, sampleTimeUs += 7'000);
    // Two frames paired against the wrong Present, in both directions.
    window.Add(2.0f, sampleTimeUs += 7'000);
    window.Add(3.0f, sampleTimeUs += 7'000);
    window.Add(300.0f, sampleTimeUs += 7'000);
    window.Add(400.0f, sampleTimeUs += 7'000);

    const auto snapshot = window.MakeSnapshot(Source::Estimated);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.sampleCount, 32u);
    EXPECT_FLOAT_EQ(snapshot.milliseconds, 20.0f);
    EXPECT_FLOAT_EQ(snapshot.medianMilliseconds, 20.0f);
    EXPECT_FLOAT_EQ(snapshot.minimumMilliseconds, 2.0f);
    EXPECT_FLOAT_EQ(snapshot.maximumMilliseconds, 400.0f);
}

TEST(SystemLatencyClockTest, LatestFrameBeginPrefersTheNewestBoundaryAndRejectsUnusableOnes) {
    FrameBeginClockReset reset;
    FrameBeginKind kind = FrameBeginKind::Modelled;

    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'000'000, kind), 0);
    EXPECT_EQ(kind, FrameBeginKind::Modelled);

    ce::system_latency::NoteFrameBegin(1'000'000, FrameBeginKind::PresentReturn);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'005'000, kind), 1'000'000);
    EXPECT_EQ(kind, FrameBeginKind::PresentReturn);

    // A low-latency wait returns after the previous Present did, so it wins
    // without any per-runtime special casing.
    ce::system_latency::NoteFrameBegin(1'004'000, FrameBeginKind::LowLatencySleepReturn);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'005'000, kind), 1'004'000);
    EXPECT_EQ(kind, FrameBeginKind::LowLatencySleepReturn);

    // A boundary from another presenting thread that is already ahead of this
    // present, and one whose producer has stopped running, are both unusable.
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'003'000, kind), 0);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'400'000, kind), 0);
    EXPECT_EQ(kind, FrameBeginKind::Modelled);
}

TEST(SystemLatencyMetricsTest, PerformanceMetricsResolvesTheFrameBeginBoundaryFromTheProcessClock) {
    FrameBeginClockReset reset;
    PerformanceMetrics metrics;
    SharedDisplayTiming timing;
    timing.Reset(4321, 0, DisplayTimingStatus::Starting);
    metrics.ConsumeDisplayTiming(timing, 6'900'000);

    for (int i = 0; i < 12; ++i) {
        const int64_t presentUs = 7'000'000 + 10'000 * i;
        ce::system_latency::NoteFrameBegin(presentUs - 3'000, FrameBeginKind::LowLatencySleepReturn);
        metrics.Update(presentUs);
        if (i > 0) {
            const int64_t displayedPresentUs = presentUs - 10'000;
            timing.Publish(displayedPresentUs + 10'000, presentUs, displayedPresentUs + 200);
        }
    }
    metrics.ConsumeDisplayTiming(timing, 7'120'000);

    const auto snapshot = metrics.GetSystemLatency(7'120'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    // 10.0 ms queue + 3.0 ms measured CPU frame + 5.0 ms average input wait.
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.1f);
    EXPECT_EQ(metrics.GetSystemLatencyDiagnostics(7'120'000).lastFrameBeginKind,
              FrameBeginKind::LowLatencySleepReturn);
}
