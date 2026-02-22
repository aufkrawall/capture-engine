#include "../hook/common/performance_metrics.h"
#include <gtest/gtest.h>

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
  metrics.Update(1000000); // Initialize lastFrameTime
  metrics.Update(1016666); // Delta 16666

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
  for (int i = 0; i < 80; i++) { // 160 frames > 120 window
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

  metrics.SetRecording(true); // Locks baseline

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
  metrics.Update(1000000); // init
  for (int i = 0; i < 300; i++) {
    metrics.Update(1007000 + i * 7000); // 7ms
  }
  metrics.GetSmartScale(min, max);
  EXPECT_NEAR(min, 0.0f, 0.001f);
  EXPECT_NEAR(max, 33.0f, 0.001f);

  // Case 2: High Latency Spike (100ms) should expand the scale
  // Continue from the last timestamp so time moves forward
  int64_t lastTs = 1007000 + 300 * 7000;
  metrics.Update(lastTs + 100000); // 100ms spike

  metrics.GetSmartScale(min, max);
  EXPECT_NEAR(min, 0.0f, 0.001f);
  EXPECT_GT(max, 100.0f); // Should be roughly 110ms
}
