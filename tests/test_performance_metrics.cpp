#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../hook/common/overlay_metrics_publisher.h"
#include "../hook/common/perf_logger.h"
#include "../hook/common/performance_metrics.h"

// Test fixture
class PerformanceMetricsTest : public ::testing::Test {
protected:
    PerformanceMetrics metrics;
};

TEST_F(PerformanceMetricsTest, InitialState) {
    EXPECT_EQ(metrics.GetHistoryIndex(), 0);
    EXPECT_FLOAT_EQ(metrics.GetHistoryArray()[0], 0.0f);
    EXPECT_EQ(metrics.GetWindowStdDev(), 0.0);
    EXPECT_FALSE(metrics.IsStutterDetected());
}

TEST_F(PerformanceMetricsTest, UpdateHistory) {
    // Simulate 16.6ms frames (60 FPS) in microseconds
    // Start at 0, next frame at 16666
    metrics.Update(1000000);  // Initialize lastFrameTime
    metrics.Update(1016666);  // Delta 16666

    int idx = metrics.GetHistoryIndex();
    // Index should increment to 1
    EXPECT_EQ(idx, 1);

    // The value stored is in ms: 16666us = 16.666ms
    float val = metrics.GetHistoryArray()[0];
    EXPECT_NEAR(val, 16.666f, 0.01f);
}

TEST_F(PerformanceMetricsTest, VarianceCalculation) {
    // Input stable 10ms frames
    int64_t t = 1000000;
    metrics.Update(t);

    // Fill window with jitter to measure Pure Variance without dilution
    // 5ms, 15ms repeats (Mean 10ms, Dev +/- 5ms => StdDev 5000us)
    for (int i = 0; i < 80; i++) {  // 160 frames > 120 window
        t += 5000;
        metrics.Update(t);
        t += 15000;
        metrics.Update(t);
    }

    double stdDev = metrics.GetWindowStdDev();
    EXPECT_NEAR(stdDev, 5000.0, 100.0);
}

TEST_F(PerformanceMetricsTest, StutterDetection) {
    // 1. Establish Baseline (Low but non-zero Variance)
    int64_t t = 1000000;
    metrics.Update(t);

    // 200 frames of mostly stable 10ms with tiny jitter (+/- 10us)
    for (int i = 0; i < 200; i++) {
        t += 10000 + ((i % 2 == 0) ? 10 : -10);
        metrics.Update(t);
    }

    metrics.SetRecording(true);  // Locks baseline

    // 2. Introduce High Variance during Recording
    // Jitter +/- 4ms (Variance increase massive vs 1us)
    for (int i = 0; i < 300; i++) {
        int64_t jitter = (i % 2 == 0) ? -4000 : 4000;
        t += (10000 + jitter);
        metrics.Update(t);
    }

    EXPECT_TRUE(metrics.IsStutterDetected());
}

TEST_F(PerformanceMetricsTest, SmartScaling) {
    float min, max;

    // Case 1: Low Frame Times (e.g., 7ms / 144 FPS)
    // Should scale to default floor (33ms)
    metrics.Update(1000000);  // init
    for (int i = 0; i < 300; i++) {
        metrics.Update(1007000 + i * 7000);  // 7ms
    }
    metrics.GetSmartScale(min, max);
    EXPECT_NEAR(min, 0.0f, 0.001f);
    EXPECT_NEAR(max, 33.0f, 0.001f);

    // Case 2: High Latency Spike (100ms) should expand the scale
    // Continue from the last timestamp so time moves forward
    int64_t lastTs = 1007000 + 300 * 7000;
    metrics.Update(lastTs + 100000);  // 100ms spike

    metrics.GetSmartScale(min, max);
    EXPECT_NEAR(min, 0.0f, 0.001f);
    EXPECT_GT(max, 100.0f);  // Should be roughly 110ms
}

TEST_F(PerformanceMetricsTest, HighFpsFramesAreNotDebouncedAway) {
    int64_t t = 1000000;
    metrics.Update(t);

    for (int i = 0; i < 10; ++i) {
        t += 526;  // ~1901 FPS
        metrics.Update(t);
    }

    EXPECT_EQ(metrics.GetHistoryIndex(), 10);
    EXPECT_NEAR(metrics.GetHistoryArray()[9], 0.526f, 0.01f);
    EXPECT_GT(metrics.GetCurrentFPS(), 1500.0f);
}

TEST_F(PerformanceMetricsTest, DisplayChangeTimingIncludesGeneratedOutputCadence) {
    metrics.Update(1000000);
    metrics.Update(1016666);
    metrics.Update(1033332);

    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    timing.Publish(2000000, 3000000);
    timing.Publish(2004167, 3001000);
    timing.Publish(2008334, 3002000);
    timing.Publish(2012501, 3003000);

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3004000);

    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_EQ(metrics.GetHistoryIndex(), 3);
    EXPECT_NEAR(metrics.GetHistoryArray()[0], 4.167f, 0.01f);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 240.0f, 1.0f);
}

TEST_F(PerformanceMetricsTest, DisplayChangeTimingDrivesFrameTimeVariance) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenTimeUs = 2000000;
    timing.Publish(screenTimeUs, 3000000);
    for (int i = 0; i < 80; ++i) {
        screenTimeUs += 5000;
        timing.Publish(screenTimeUs, 3000001 + i * 2);
        screenTimeUs += 15000;
        timing.Publish(screenTimeUs, 3000002 + i * 2);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3001000);

    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_NEAR(metrics.GetWindowStdDev(), 5000.0, 100.0);
}

// The graph draws the refresh-bounded time; a stream whose kernel times
// alternate 3.5/10.4 ms but whose graph times are one refresh apart must read
// as flat.
TEST_F(PerformanceMetricsTest, DisplayGraphUsesRefreshBoundedGraphTime) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t onBlankUs = 2000000;
    timing.Publish(onBlankUs, 3000000);
    for (int i = 0; i < 80; ++i) {
        timing.Publish(onBlankUs + 3530, 3000001 + i * 2, 0, true, onBlankUs + 6945);
        onBlankUs += 2 * 6945;
        timing.Publish(onBlankUs, 3000002 + i * 2);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3001000);

    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_LT(metrics.GetWindowStdDev(), 10.0);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 144.0f, 1.0f);
}

TEST_F(PerformanceMetricsTest, DisplayChangePreferenceFallsBackWhenPublicationBecomesStale) {
    metrics.Update(1000000);
    metrics.Update(1010000);

    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    timing.Publish(2000000, 3000000);
    timing.Publish(2005000, 3001000);

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3002000);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);

    metrics.ConsumeDisplayTiming(timing, 5001001);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 100.0f, 1.0f);
}

// A display stream that publishes flip-latch timestamps is not a screen clock,
// however many samples it delivers at however correct a mean. Measured in Talos
// under FSR frame generation below the refresh cap: the published mean was
// right to 0.3% while the standard deviation was four times what the same
// When display timing is healthy, DisplayChange faithfully reports the active
// display change stream even when unlabelled or alternating, without sugarcoating
// the on-screen variance into a flat presentation line.
TEST_F(PerformanceMetricsTest, UnresolvedDisplayStreamIsFaithfullyUsedWhenPreferred) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    // Presents are even; the reported latch times alternate around them.
    int64_t presentUs = 1'000'000;
    int64_t latchUs = 2'000'000;
    metrics.Update(presentUs);
    for (int i = 0; i < 200; ++i) {
        presentUs += 11'000;
        metrics.Update(presentUs);
        latchUs += (i % 2 == 0) ? 8'000 : 14'000;
        timing.Publish(latchUs, 3'000'000 + i, 0, /*screenTimeResolved=*/false);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3'000'200);

    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 0u);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_GT(metrics.GetWindowStdDev(), 2500.0);

    // When the display timing stream becomes unavailable, it falls back to Presentation.
    timing.SetStatus(DisplayTimingStatus::Unavailable);
    metrics.ConsumeDisplayTiming(timing, 3'000'201);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 90.9f, 1.0f);
    EXPECT_LT(metrics.GetWindowStdDev(), 100.0);
}

// The gate is about provenance, not about jitter: a resolved screen-time stream
// keeps driving the metric even when the screen really is uneven, because that
// unevenness is then a fact about the display rather than about the reporting.
TEST_F(PerformanceMetricsTest, ResolvedScreenTimeStreamKeepsDrivingTheMetric) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    metrics.Update(1'000'000);
    metrics.Update(1'011'000);

    int64_t screenUs = 2'000'000;
    for (int i = 0; i < 200; ++i) {
        screenUs += (i % 2 == 0) ? 8'000 : 14'000;
        timing.Publish(screenUs, 3'000'000 + i, 0, /*screenTimeResolved=*/true);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3'000'200);

    EXPECT_TRUE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 1000u);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_GT(metrics.GetWindowStdDev(), 2500.0);
}

// Even a short unlabelled stream remains selected. Diagnostic provenance must
// not invent resolved samples just because the stream is new.
TEST_F(PerformanceMetricsTest, ShortUnresolvedStreamStillDrivesMetricWithoutInventedProvenance) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenUs = 2'000'000;
    for (int i = 0; i < 8; ++i) {
        screenUs += 11'000;
        timing.Publish(screenUs, 3'000'000 + i, 0, /*screenTimeResolved=*/false);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3'000'010);

    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
}

// Changes in producer provenance never switch a healthy requested display stream.
TEST_F(PerformanceMetricsTest, ProvenanceDropDoesNotChangeHealthySource) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    const auto publishBatch = [&](int count, int unresolvedEvery) {
        for (int i = 0; i < count; ++i) {
            screenUs += 11'000;
            timing.Publish(screenUs, ++publishUs, 0,
                           /*screenTimeResolved=*/unresolvedEvery == 0 || (i % unresolvedEvery) != 0);
        }
    };

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    // Fully resolved first, so the stream is selected.
    publishBatch(128, 0);
    metrics.ConsumeDisplayTiming(timing, publishUs);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);

    // Now around 80% resolved: the diagnostic changes, the source does not.
    publishBatch(200, 5);
    metrics.ConsumeDisplayTiming(timing, publishUs);
    EXPECT_LT(metrics.GetDisplayScreenTimePermille(), 900u);
    EXPECT_GT(metrics.GetDisplayScreenTimePermille(), 500u);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
}

// GetLastDisplayFrameTimeMs reports the display change interval when a display
// stream is active, and falls back to presentation when no display stream exists.
TEST_F(PerformanceMetricsTest, LastDisplayFrameTimeReportsDisplayIntervalOrFallsBack) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    metrics.Update(1'000'000);
    metrics.Update(1'011'000);

    // Before any display publications arrive, display frame time falls back to presentation.
    EXPECT_NEAR(metrics.GetLastDisplayFrameTimeMs(), metrics.GetLastPresentationFrameTimeMs(), 0.001f);

    int64_t latchUs = 2'000'000;
    for (int i = 0; i < 200; ++i) {
        latchUs += (i % 2 == 0) ? 8'000 : 14'000;
        timing.Publish(latchUs, 3'000'000 + i, 0, /*screenTimeResolved=*/false);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, 3'000'200);

    ASSERT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_NEAR(metrics.GetLastDisplayFrameTimeMs(), 14.0f, 0.1f);
}

// The diagnostic must stay current even where the preference alone already
// decides the source, or a log line that exists to explain the choice would
// report a value it stopped updating.
TEST_F(PerformanceMetricsTest, ScreenTimeDiagnosticStaysCurrentUnderAPresentationPreference) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t latchUs = 2'000'000;
    for (int i = 0; i < 200; ++i) {
        latchUs += 11'000;
        timing.Publish(latchUs, 3'000'000 + i, 0, /*screenTimeResolved=*/false);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::Presentation);
    metrics.ConsumeDisplayTiming(timing, 3'000'200);

    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 0u);
}

// The provenance tracking is about the stream's current regime, so it has to follow
// a regime change rather than average across it. Recovery must complete inside one window.
TEST_F(PerformanceMetricsTest, ScreenTimeProvenanceFollowsARegimeChangeWithinOneWindow) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    const auto publish = [&](bool resolved) {
        screenUs += 11'000;
        timing.Publish(screenUs, ++publishUs, 0, resolved);
    };

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.Update(1'000'000);
    metrics.Update(1'011'000);

    // A long latch-only stretch, far more than one window.
    for (int i = 0; i < 3000; ++i)
        publish(false);
    metrics.ConsumeDisplayTiming(timing, publishUs);
    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 0u);

    // The new regime resolves every sample. One window of them has to be enough,
    // whatever the stream did before it.
    for (uint32_t i = 0; i < 128; ++i)
        publish(true);
    metrics.ConsumeDisplayTiming(timing, publishUs);

    EXPECT_TRUE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 1000u);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
}

// ... and symmetrically, a stream that stops resolving must not keep the metric
// on a screen-time claim it can no longer support.
TEST_F(PerformanceMetricsTest, ScreenTimeProvenanceReleasesWithinOneWindowWhenResolutionStops) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    const auto publish = [&](bool resolved) {
        screenUs += 11'000;
        timing.Publish(screenUs, ++publishUs, 0, resolved);
    };

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.Update(1'000'000);
    metrics.Update(1'011'000);

    for (int i = 0; i < 3000; ++i)
        publish(true);
    metrics.ConsumeDisplayTiming(timing, publishUs);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_TRUE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 1000u);

    for (uint32_t i = 0; i < 128; ++i)
        publish(false);
    metrics.ConsumeDisplayTiming(timing, publishUs);

    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 0u);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
}

// Provenance is a per-sample fact and a stream can be a screen clock while a
// minority of its samples are not. Measured under DLSS FG: four fifths of the
// completions are immediate flips carrying the driver's announced screen time,
// the rest are deferred and unresolved, and the published series is flat at
// 450 us of jaggedness while the presents that produced it - a generated group
// issued as a burst - carry 18244 us. A series that is not adding jitter to the
// frames it measures has to be usable whatever its labels say, or the overlay
// throws away the only measurement that shows what the screen did.
TEST_F(PerformanceMetricsTest, AFlatDisplayStreamIsUsedEvenWhereSomeSamplesAreUnlabelled) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    // Presents arrive as generated groups: two together, then a long wait.
    int64_t presentUs = 1'000'000;
    // Screen transitions are evenly spaced, as the display consumes that group.
    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    for (int i = 0; i < 400; ++i) {
        presentUs += (i % 2 == 0) ? 1'000 : 21'000;
        metrics.Update(presentUs);
        screenUs += 11'000;
        // One sample in five stays a deferred, unresolved completion.
        timing.Publish(screenUs, ++publishUs, 0, /*screenTimeResolved=*/(i % 5) != 0);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, publishUs);

    // Only four fifths carry a screen-time label, below the selection share...
    EXPECT_LT(metrics.GetDisplayScreenTimePermille(), 900u);
    EXPECT_GT(metrics.GetDisplayScreenTimePermille(), 700u);
    // The measured display series remains selected; flatness cannot invent provenance.
    EXPECT_LT(metrics.GetDisplayJaggednessUs(), metrics.GetPresentationJaggednessUs());
    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 90.9f, 1.0f);
}

// When DisplayChange is preferred, an alternating or jagged display stream (such
// as FSR FG on VRR below the refresh cap) is faithfully admitted to reflect
// real on-screen frame pacing, rather than sugarcoated into presentation timing.
TEST_F(PerformanceMetricsTest, AJaggedDisplayStreamIsFaithfullyAdmittedWithoutSugarcoating) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t presentUs = 1'000'000;
    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    for (int i = 0; i < 400; ++i) {
        // Evenly paced presents, with just enough movement to be a real series.
        presentUs += (i % 2 == 0) ? 10'900 : 11'100;
        metrics.Update(presentUs);
        // Flip-latch timestamps alternating either side of the true screen time.
        screenUs += (i % 2 == 0) ? 8'000 : 14'000;
        timing.Publish(screenUs, ++publishUs, 0, /*screenTimeResolved=*/(i % 5) != 0);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, publishUs);

    EXPECT_GT(metrics.GetDisplayJaggednessUs(), metrics.GetPresentationJaggednessUs());
    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_GT(metrics.GetWindowStdDev(), 2500.0);
}

// Under variable refresh below the cap (or with FG off), completions are unclocked
// and unlabelled (screenTimeResolved=false), but reflect true on-screen frame times
// with minor DPC jitter. A healthy display stream remains selected regardless
// of its jaggedness and without inventing producer provenance.
TEST_F(PerformanceMetricsTest, VRRDisplayStreamWithMinorDpcJitterIsAdmitted) {
    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);

    int64_t presentUs = 1'000'000;
    int64_t screenUs = 2'000'000;
    int64_t publishUs = 3'000'000;
    for (int i = 0; i < 400; ++i) {
        // Presents with modest frame variation: alternating 13.0 ms and 14.0 ms (jaggedness ~1000 us).
        presentUs += (i % 2 == 0) ? 13'000 : 14'000;
        metrics.Update(presentUs);
        // VRR display completions with slight DPC jitter: alternating 12.9 ms and 14.1 ms (jaggedness ~1200 us).
        screenUs += (i % 2 == 0) ? 12'900 : 14'100;
        timing.Publish(screenUs, ++publishUs, 0, /*screenTimeResolved=*/false);
    }

    metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
    metrics.ConsumeDisplayTiming(timing, publishUs);

    EXPECT_EQ(metrics.GetDisplayScreenTimePermille(), 0u);
    EXPECT_GT(metrics.GetDisplayJaggednessUs(), metrics.GetPresentationJaggednessUs());
    EXPECT_LE(metrics.GetDisplayJaggednessUs(), metrics.GetPresentationJaggednessUs() * 1.5);
    EXPECT_FALSE(metrics.IsDisplayStreamScreenTime());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
}

TEST_F(PerformanceMetricsTest, PresentationSelectionIgnoresAHealthyDisplayStream) {
    metrics.Update(1000000);
    metrics.Update(1020000);

    SharedDisplayTiming timing;
    timing.Reset(1234, 0, DisplayTimingStatus::Starting);
    timing.Publish(2000000, 3000000);
    timing.Publish(2004000, 3001000);

    metrics.SetFrameTimeSource(FrameTimeSource::Presentation);
    metrics.ConsumeDisplayTiming(timing, 3002000);

    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 50.0f, 1.0f);
}

// Telemetry must keep reporting observed presentation activity - never clamp
// it to the configured cap. A correct 3x output-group limiter under a 130 fps
// cap produces an even ~7.7 ms callback cadence (the paced group owner waits
// ~23.08 ms, its two generated outputs follow immediately), which converges
// near 130. The pre-fix escape (whole extra callback groups admitted inside
// the 2 ms dedup window, measured at ~146 fps with 167 fps one-second peaks)
// stays honestly visible as the higher rate it really was.
TEST_F(PerformanceMetricsTest, FGGroupedAdmissionTraceConvergesNearConfiguredCap) {
    constexpr int64_t kFrame130Us = 7692;  // 130 fps output cadence (post-fix)
    int64_t t = 1000000;
    metrics.Update(t);
    for (int i = 0; i < 120; ++i) {
        t += kFrame130Us;
        metrics.Update(t);
    }
    EXPECT_NEAR(metrics.GetCurrentFPS(), 130.0f, 1.5f);
    EXPECT_NEAR(metrics.GetAverageFPS(), 130.0f, 1.5f);

    // The pre-fix burst pattern: groups of 3 outputs arrive together every
    // ~20.5 ms because the time-window dedup admitted extra groups unpaced.
    PerformanceMetrics burst;
    int64_t b = 1000000;
    burst.Update(b);
    for (int i = 0; i < 60; ++i) {
        for (int j = 0; j < 3; ++j) {
            b += 200;  // sub-threshold burst callback (not a duplicate sample)
            burst.Update(b);
        }
        b += 19900;  // next paced group boundary
    }
    EXPECT_GT(burst.GetCurrentFPS(), 140.0f) << "telemetry must not be clamped to the configured cap";
}

TEST_F(PerformanceMetricsTest, LowPercentilesUseWorstFrameTimesWithoutHeapSortDependency) {
    int64_t t = 1000000;
    metrics.Update(t);

    for (int i = 0; i < 199; ++i) {
        t += 10000;
        metrics.Update(t);
    }
    t += 50000;
    metrics.Update(t);

    EXPECT_LT(metrics.Get1PercentLowFPS(), 50.0f);
    EXPECT_GT(metrics.Get1PercentLowFPS(), 0.0f);
    EXPECT_LT(metrics.Get01PercentLowFPS(), 50.0f);
    EXPECT_GT(metrics.Get01PercentLowFPS(), 0.0f);
}

// The percentile scratch buffer is a reused thread-local (it is 32 KB, and
// zero-initializing it per call happened inside a present hook). Reuse is only
// safe while nothing reads past the sample count, so prove that a short history
// cannot see the tail of a longer one computed earlier on the same thread.
TEST_F(PerformanceMetricsTest, PercentileScratchReuseCannotLeakAnEarlierHistory) {
    int64_t t = 1000000;
    PerformanceMetrics slow;
    slow.Update(t);
    for (int i = 0; i < 400; ++i) {
        t += 100000;  // 100 ms frames: a very slow history to leave behind
        slow.Update(t);
    }
    const float slowLow = slow.Get1PercentLowFPS();
    ASSERT_GT(slowLow, 0.0f);
    EXPECT_NEAR(slowLow, 10.0f, 1.0f);

    PerformanceMetrics fast;
    int64_t f = 1000000;
    fast.Update(f);
    for (int i = 0; i < 120; ++i) {
        f += 5000;  // 5 ms frames throughout: nothing here is slower than 200 FPS
        fast.Update(f);
    }
    const float fastLow = fast.Get1PercentLowFPS();
    EXPECT_NEAR(fastLow, 200.0f, 10.0f) << "a leaked 100 ms sample from the previous history would show up here";
    EXPECT_GT(fastLow, 100.0f);
}

TEST_F(PerformanceMetricsTest, FGMetricsResetClearsActiveStateAndLabel) {
    metrics.SetFGMetrics(120.0f, 60.0f, 2, 1);

    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");

    metrics.SetFGMetrics(0.0f, 0.0f, 1, 0);

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
}

TEST_F(PerformanceMetricsTest, OverlayPublisherPublishesDLSSMetricsThroughCanonicalMapping) {
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, {
                                                               .effectiveFGActive = true,
                                                               .runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                                                               .outputFPS = 180.0f,
                                                               .baseFPS = 60.0f,
                                                               .multiplier = 3,
                                                               .publicationSource = "test",
                                                           });

    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 180.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 60.0f);
    EXPECT_EQ(metrics.GetFGMultiplier(), 3);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "DLSS FG");
}

TEST_F(PerformanceMetricsTest, OverlayPublisherPublishesFSRMetricsThroughCanonicalMapping) {
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, {
                                                               .effectiveFGActive = true,
                                                               .runtimeMode = ce::fg_runtime::RuntimeMode::kFSRFG,
                                                               .outputFPS = 144.0f,
                                                               .baseFPS = 72.0f,
                                                               .multiplier = 2,
                                                               .publicationSource = "test",
                                                           });

    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FSR FG");
}

TEST_F(PerformanceMetricsTest, OverlayPublisherPublishesSmoothMotionMetricsThroughCanonicalMapping) {
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics,
                                                 {
                                                     .effectiveFGActive = true,
                                                     .runtimeMode = ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion,
                                                     .outputFPS = 240.0f,
                                                     .baseFPS = 120.0f,
                                                     .multiplier = 2,
                                                     .publicationSource = "test",
                                                 });

    EXPECT_TRUE(metrics.IsFGActive());
    EXPECT_EQ(metrics.GetFGMultiplier(), 2);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "NVIDIA SM");
}

TEST_F(PerformanceMetricsTest, OverlayPublisherResetsInactiveStateToBaseline) {
    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, {
                                                               .effectiveFGActive = true,
                                                               .runtimeMode = ce::fg_runtime::RuntimeMode::kDLSSFG,
                                                               .outputFPS = 120.0f,
                                                               .baseFPS = 60.0f,
                                                               .multiplier = 2,
                                                               .publicationSource = "test",
                                                           });

    ce::overlay_metrics::PublishOverlayFGMetrics(&metrics, {
                                                               .effectiveFGActive = false,
                                                               .runtimeMode = ce::fg_runtime::RuntimeMode::kOff,
                                                               .outputFPS = 999.0f,
                                                               .baseFPS = 999.0f,
                                                               .multiplier = 4,
                                                               .publicationSource = "test",
                                                           });

    EXPECT_FALSE(metrics.IsFGActive());
    EXPECT_FLOAT_EQ(metrics.GetFGOutputFPS(), 0.0f);
    EXPECT_FLOAT_EQ(metrics.GetFGBaseFPS(), 0.0f);
    EXPECT_EQ(metrics.GetFGMultiplier(), 1);
    EXPECT_STREQ(metrics.GetFGTypeLabel(), "FG");
}

TEST(PerfLoggerTest, PerfMetricsCsvFlushPolicyKeepsEarlyAndPeriodicFramesDurable) {
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(1));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(8));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(32));
    EXPECT_TRUE(ShouldFlushPerfMetricsCsvAfterFrame(64));

    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(9));
    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(31));
    EXPECT_FALSE(ShouldFlushPerfMetricsCsvAfterFrame(63));
}

TEST(PerfLoggerTest, ForceRebindFinalizesOldCsvAndStartsFreshSequence) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ce_perf_rebind_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path firstCsv = dir / "perf_metrics_first.csv";
    const fs::path secondCsv = dir / "perf_metrics_second.csv";

    PerfLogger& logger = PerfLogger::Get();
    logger.Shutdown();  // Finalize any state a previous test may have left open.

    FrameMetrics metrics{};
    metrics.qpcUs = PerfLogger::GetQpcUs();

    logger.Init(firstCsv.string().c_str());
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);

    // A plain Init while a file is open must keep the original session file.
    logger.Init(secondCsv.string().c_str());
    EXPECT_TRUE(logger.IsEnabled());
    EXPECT_TRUE(fs::exists(firstCsv));
    EXPECT_FALSE(fs::exists(secondCsv));

    // Resident-hook reactivation force-rebinds: finalize the old CSV and open
    // the new session path with a fresh frame sequence.
    logger.Init(secondCsv.string().c_str(), true);
    ASSERT_TRUE(logger.IsEnabled());
    logger.LogFrame(metrics);
    logger.Shutdown();

    EXPECT_TRUE(fs::exists(secondCsv));
    std::ifstream first(firstCsv);
    std::ifstream second(secondCsv);
    std::stringstream firstText;
    std::stringstream secondText;
    firstText << first.rdbuf();
    secondText << second.rdbuf();

    // Both files carry the header and restart their row numbering at 1.
    EXPECT_NE(firstText.str().find("frame,qpc_us,total_us"), std::string::npos);
    EXPECT_NE(firstText.str().find("\n1,"), std::string::npos);
    EXPECT_NE(secondText.str().find("frame,qpc_us,total_us"), std::string::npos);
    EXPECT_NE(secondText.str().find("\n1,"), std::string::npos);

    first.close();
    second.close();
    fs::remove_all(dir);
}
