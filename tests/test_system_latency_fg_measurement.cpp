#include <gtest/gtest.h>

#include "../hook/common/performance_metrics.h"
#include "../hook/common/system_latency_frame_begin.h"
#include "../hook/common/system_latency_metrics.h"

namespace {

using ce::system_latency::FrameBeginKind;
using ce::system_latency::NativeFrameReport;
using ce::system_latency::NativeReport;
using ce::system_latency::Source;
using ce::system_latency::Tracker;

struct FrameBeginClockReset {
    FrameBeginClockReset() {
        ce::system_latency::FrameBeginClock::Get().Reset();
    }
    ~FrameBeginClockReset() {
        ce::system_latency::FrameBeginClock::Get().Reset();
    }
};

NativeFrameReport MakeNativeFrame(uint64_t frameId, uint64_t presentUs) {
    NativeFrameReport frame{};
    frame.frameId = frameId;
    frame.simulationStartTimeUs = presentUs - 8'000;
    frame.presentStartTimeUs = presentUs;
    frame.gpuRenderEndTimeUs = presentUs + 2'000;
    return frame;
}

float MeasureNoGeneration(int64_t frameBeginLeadUs) {
    Tracker tracker;
    constexpr int64_t firstPresentUs = 13'000'000;
    constexpr int64_t intervalUs = 10'000;
    for (int frame = 0; frame < 30; ++frame) {
        const int64_t presentUs = firstPresentUs + intervalUs * frame;
        tracker.ObservePresent(
            presentUs, frameBeginLeadUs > 0 ? presentUs - frameBeginLeadUs : 0,
            frameBeginLeadUs > 0 ? FrameBeginKind::LowLatencySleepReturn : FrameBeginKind::Modelled);
        if (frame > 0) {
            const int64_t displayedPresentUs = presentUs - intervalUs;
            tracker.ObserveDisplay(displayedPresentUs + intervalUs, displayedPresentUs + 200);
        }
    }
    return tracker.GetSnapshot(13'300'000).milliseconds;
}

float MeasureGenerated(int multiplier, bool lowLatency) {
    Tracker tracker;
    constexpr int64_t outputIntervalUs = 5'000;
    const int64_t applicationIntervalUs = outputIntervalUs * multiplier;
    tracker.SetFrameGeneration(1'000'000.0f / static_cast<float>(applicationIntervalUs), multiplier);
    int64_t lastScreenUs = 0;
    for (int frame = 0; frame < 30; ++frame) {
        const int64_t sourceUs = 14'000'000 + applicationIntervalUs * frame;
        tracker.ObserveApplicationPresent(
            sourceUs, lowLatency ? sourceUs - 2'000 : 0,
            lowLatency ? FrameBeginKind::LowLatencySleepReturn : FrameBeginKind::Modelled);
        if (frame == 0)
            continue;
        for (int output = 0; output < multiplier; ++output) {
            const int64_t runtimePresentUs = sourceUs + 1'000 + outputIntervalUs * output;
            lastScreenUs = runtimePresentUs + 2'000;
            tracker.ObserveDisplay(lastScreenUs, runtimePresentUs);
        }
    }
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_TRUE(diagnostics.frameGenerationObserved);
    EXPECT_NEAR(diagnostics.observedOutputRatioPermille, multiplier * 1000, 2);
    return tracker.GetSnapshot(lastScreenUs).milliseconds;
}

}  // namespace

TEST(SystemLatencyFGMeasurementTest, OutputRateMarkersAreRejectedWhileGenerationIsMeasured) {
    Tracker tracker;
    tracker.SetFrameGeneration(50.0f, 2);
    for (int frame = 0; frame < 24; ++frame) {
        const int64_t sourceUs = 12'000'000 + 20'000 * frame;
        tracker.ObserveApplicationPresent(sourceUs + 2'000, sourceUs,
                                          FrameBeginKind::LowLatencySleepReturn);
        if (frame == 0)
            continue;
        const int64_t heldSourceUs = sourceUs - 20'000;
        tracker.ObserveDisplay(heldSourceUs + 25'000, heldSourceUs + 22'500);
        tracker.ObserveDisplay(heldSourceUs + 35'000, heldSourceUs + 32'500);
    }

    NativeReport outputRateReport{};
    outputRateReport.count = 48;
    for (size_t frame = 0; frame < outputRateReport.count; ++frame)
        outputRateReport.frames[frame] = MakeNativeFrame(frame + 1, 12'002'000 + 10'000 * frame);
    tracker.SubmitNativeReport(outputRateReport);

    const auto snapshot = tracker.GetSnapshot(12'500'000);
    ASSERT_TRUE(snapshot.valid);
    EXPECT_EQ(snapshot.source, Source::Estimated);
    const auto diagnostics = tracker.GetDiagnostics();
    EXPECT_TRUE(diagnostics.frameGenerationObserved);
    EXPECT_FALSE(diagnostics.markerCadenceTrusted);
    EXPECT_EQ(diagnostics.markerReportsRejectedForOutputCadence, 1u);
    EXPECT_NEAR(diagnostics.observedOutputRatioPermille, 2000, 1);
}

TEST(SystemLatencyFGMeasurementTest, MeasuredPipelinesPreserveExpectedReflexAndFrameGenerationOrdering) {
    const float noFgReflexOff = MeasureNoGeneration(0);
    const float noFgReflexOn = MeasureNoGeneration(2'000);
    const float fsr2xReflexOff = MeasureGenerated(2, false);
    const float dlss2xReflexOn = MeasureGenerated(2, true);
    const float dlss3xReflexOn = MeasureGenerated(3, true);
    const float dlss4xReflexOn = MeasureGenerated(4, true);

    EXPECT_LT(noFgReflexOn, noFgReflexOff);
    EXPECT_LT(noFgReflexOff, fsr2xReflexOff);
    EXPECT_LT(noFgReflexOn, dlss2xReflexOn);
    EXPECT_LT(dlss2xReflexOn, dlss3xReflexOn);
    EXPECT_LT(dlss3xReflexOn, dlss4xReflexOn);
}

TEST(SystemLatencyFGMeasurementTest, MultiplierChangeStartsANewMeasurementEpoch) {
    Tracker tracker;
    tracker.SetFrameGeneration(50.0f, 2);
    int64_t lastScreenUs = 0;
    for (int frame = 0; frame < 16; ++frame) {
        const int64_t sourceUs = 15'000'000 + 20'000 * frame;
        tracker.ObserveApplicationPresent(sourceUs, sourceUs - 2'000,
                                          FrameBeginKind::LowLatencySleepReturn);
        if (frame == 0)
            continue;
        tracker.ObserveDisplay(lastScreenUs = sourceUs + 3'000, sourceUs + 1'000);
        tracker.ObserveDisplay(lastScreenUs = sourceUs + 13'000, sourceUs + 11'000);
    }
    ASSERT_TRUE(tracker.GetSnapshot(lastScreenUs).valid);
    const uint64_t resetsBefore = tracker.GetDiagnostics().measurementEpochResets;

    tracker.SetFrameGeneration(25.0f, 4);

    const auto afterChange = tracker.GetSnapshot(lastScreenUs);
    EXPECT_FALSE(afterChange.valid);
    EXPECT_TRUE(afterChange.frameGenerationConfigured);
    EXPECT_TRUE(afterChange.frameGenerationStateKnown);
    EXPECT_GT(tracker.GetDiagnostics().measurementEpochResets, resetsBefore);
}

TEST(SystemLatencyFGMeasurementTest, LatencyReportingDoesNotDieAfterFGSwitch) {
    Tracker tracker;
    // Step 1: Run with FSR 2x (50 fps base, 100 fps output)
    tracker.SetFrameGeneration(50.0f, 2);
    int64_t screenUs = 16'000'000;
    for (int frame = 0; frame < 16; ++frame) {
        const int64_t sourceUs = 16'000'000 + 20'000 * frame;
        tracker.ObserveApplicationPresent(sourceUs, sourceUs - 2'000,
                                          FrameBeginKind::LowLatencySleepReturn);
        tracker.ObserveDisplay(screenUs = sourceUs + 5'000, sourceUs + 2'000);
        tracker.ObserveDisplay(screenUs = sourceUs + 15'000, sourceUs + 12'000);
    }
    const auto snapshot2x = tracker.GetSnapshot(screenUs);
    ASSERT_TRUE(snapshot2x.valid);
    EXPECT_GT(snapshot2x.milliseconds, 0.0f);

    // Step 2: Switch to DLSS 4x (25 fps base, 100 fps output)
    tracker.SetFrameGeneration(25.0f, 4);

    // Step 3: Displays continue arriving on the new chain (even if presents bypass ProcessFrame)
    for (int frame = 0; frame < 8; ++frame) {
        const int64_t displayUs = screenUs + 10'000 * (frame + 1);
        tracker.ObserveDisplay(displayUs, displayUs - 3'000);
    }
    const auto snapshot4x = tracker.GetSnapshot(screenUs + 80'000);
    EXPECT_TRUE(snapshot4x.valid);
    EXPECT_NE(snapshot4x.source, Source::Unavailable);
    EXPECT_GT(snapshot4x.milliseconds, 0.0f);
    EXPECT_GT(snapshot4x.milliseconds, snapshot2x.milliseconds);
}

TEST(SystemLatencyFGMeasurementTest, NativeReportsPreserveLowerLatencyForLowerMultiplier) {
    auto measureNativeFG = [](float baseFps, int multiplier) {
        Tracker tracker;
        tracker.SetFrameGeneration(baseFps, multiplier);
        const int64_t baseIntervalUs = std::llround(1'000'000.0 / static_cast<double>(baseFps));
        const int64_t displayIntervalUs = baseIntervalUs / multiplier;
        NativeReport report{};
        report.count = 24;
        for (size_t frame = 0; frame < report.count; ++frame) {
            const int64_t sourceUs = 20'000'000 + baseIntervalUs * static_cast<int64_t>(frame);
            tracker.ObserveApplicationPresent(sourceUs + 5'000);
            report.frames[frame] = MakeNativeFrame(frame + 1, static_cast<uint64_t>(sourceUs + 5'000));
        }
        int64_t lastScreenUs = 0;
        for (int frame = 0; frame < 20; ++frame) {
            const int64_t sourceUs = 20'000'000 + baseIntervalUs * frame;
            for (int mult = 0; mult < multiplier; ++mult) {
                const int64_t presentUs = sourceUs + 5'000 + displayIntervalUs * (mult + 1);
                lastScreenUs = presentUs + 2'000;
                tracker.ObserveDisplay(lastScreenUs, presentUs);
            }
        }
        tracker.SubmitNativeReport(report);
        const auto snapshot = tracker.GetSnapshot(lastScreenUs);
        EXPECT_TRUE(snapshot.valid);
        EXPECT_EQ(snapshot.source, Source::ReflexMarkers);
        EXPECT_TRUE(tracker.GetDiagnostics().frameGenerationObserved);
        return snapshot.milliseconds;
    };

    const float latency2x = measureNativeFG(60.0f, 2);
    const float latency3x = measureNativeFG(45.0f, 3);
    const float latency4x = measureNativeFG(35.0f, 4);
    EXPECT_LT(latency2x, latency3x);
    EXPECT_LT(latency3x, latency4x);
}

TEST(SystemLatencyFGMeasurementTest, FSRFGSwitchPreservesEstimatedLatencyAndIgnoresForeignNativeMarkers) {
    Tracker tracker;
    // Step 1: Run FSR FG 2x (45 fps base, 90 fps output)
    tracker.SetFrameGeneration(45.0f, 2, /*fgType=*/2);
    constexpr int64_t baseIntervalUs = 22'222;
    constexpr int64_t displayIntervalUs = 11'111;
    int64_t screenUs = 30'000'000;
    for (int frame = 0; frame < 20; ++frame) {
        const int64_t sourceUs = 30'000'000 + baseIntervalUs * frame;
        tracker.ObserveApplicationPresent(sourceUs + 2'000);
        tracker.ObserveDisplay(screenUs = sourceUs + 5'000, sourceUs + 2'000);
        tracker.ObserveDisplay(screenUs = sourceUs + 5'000 + displayIntervalUs, sourceUs + 2'000 + displayIntervalUs);
    }
    const auto initialFSRSnapshot = tracker.GetSnapshot(screenUs);
    ASSERT_TRUE(initialFSRSnapshot.valid);
    EXPECT_EQ(initialFSRSnapshot.source, Source::Estimated);
    const float initialLatency = initialFSRSnapshot.milliseconds;
    EXPECT_GT(initialLatency, 30.0f);
    EXPECT_LT(initialLatency, 75.0f);

    // Step 2: Switch to DLSS FG (fgType = 1)
    tracker.SetFrameGeneration(35.0f, 4, /*fgType=*/1);
    EXPECT_EQ(tracker.GetSnapshot(screenUs).source, Source::Unavailable);

    // Step 3: Switch back to FSR FG (fgType = 2)
    tracker.SetFrameGeneration(45.0f, 2, /*fgType=*/2);

    // Run displays again for FSR FG
    const int64_t startUs = screenUs + 100'000;
    for (int frame = 0; frame < 20; ++frame) {
        const int64_t sourceUs = startUs + baseIntervalUs * frame;
        tracker.ObserveApplicationPresent(sourceUs + 2'000);
        tracker.ObserveDisplay(screenUs = sourceUs + 5'000, sourceUs + 2'000);
        tracker.ObserveDisplay(screenUs = sourceUs + 5'000 + displayIntervalUs, sourceUs + 2'000 + displayIntervalUs);
    }

    // A foreign Streamline PCL report arrives (game engine still emitting markers)
    NativeReport foreignPclReport{};
    foreignPclReport.count = 20;
    for (size_t i = 0; i < foreignPclReport.count; ++i) {
        const uint64_t presentUs = static_cast<uint64_t>(screenUs - baseIntervalUs * (foreignPclReport.count - i));
        foreignPclReport.frames[i] = MakeNativeFrame(i + 1, presentUs);
    }
    tracker.SubmitNativeReport(foreignPclReport);

    // The snapshot must remain Source::Estimated and MUST NOT jump from ~40-48ms to 80-100ms!
    const auto postSwitchSnapshot = tracker.GetSnapshot(screenUs);
    ASSERT_TRUE(postSwitchSnapshot.valid);
    EXPECT_EQ(postSwitchSnapshot.source, Source::Estimated);
    EXPECT_NEAR(postSwitchSnapshot.milliseconds, initialLatency, 3.0f);
}

TEST(SystemLatencyFGMeasurementTest, PerformanceMetricsRejectsForeignNativeReportUnderFSRFG) {
    PerformanceMetrics metrics;
    metrics.SetFGMetrics(90.0f, 45.0f, 2, /*fgType=*/2);
    NativeReport report{};
    report.count = 4;
    for (size_t i = 0; i < report.count; ++i)
        report.frames[i] = MakeNativeFrame(i + 1, 40'000'000 + 20'000 * i);
    metrics.SubmitNativeLatencyReport(report);
    // Tracker should have received no native report, so no native samples exist
    const auto snapshot = metrics.GetSystemLatency(40'100'000);
    EXPECT_NE(snapshot.source, Source::ReflexMarkers);
}

TEST(SystemLatencyClockTest, LatestFrameBeginPublishesTheNewestBoundaryAndRejectsUnusableOnes) {
    FrameBeginClockReset reset;
    FrameBeginKind kind = FrameBeginKind::LowLatencySleepReturn;

    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'000'000, kind), 0);
    EXPECT_EQ(kind, FrameBeginKind::Modelled);
    ce::system_latency::NoteFrameBegin(1'000'000, FrameBeginKind::LowLatencySleepReturn);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'005'000, kind), 1'000'000);
    EXPECT_EQ(kind, FrameBeginKind::LowLatencySleepReturn);
    ce::system_latency::NoteFrameBegin(1'004'000, FrameBeginKind::LowLatencySleepReturn);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'005'000, kind), 1'004'000);

    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'003'000, kind), 0);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'400'000, kind), 0);
    EXPECT_EQ(kind, FrameBeginKind::Modelled);
    EXPECT_EQ(ce::system_latency::LatestFrameBegin(1'005'000, kind), 1'004'000);
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
    EXPECT_NEAR(snapshot.milliseconds, 18.0f, 0.1f);
    EXPECT_EQ(metrics.GetSystemLatencyDiagnostics(7'120'000).lastFrameBeginKind,
              FrameBeginKind::LowLatencySleepReturn);
}
