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
