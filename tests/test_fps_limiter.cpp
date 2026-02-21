#include "../hook/common/fps_limiter.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

class FpsLimiterTest : public ::testing::Test {
protected:
  std::unique_ptr<SharedMemoryLayout> mockShm;
  FpsLimiter limiter;
  LARGE_INTEGER freq;

  void SetUp() override {
    mockShm = std::make_unique<SharedMemoryLayout>();
    limiter.SetSharedMemory(mockShm.get());
    limiter.ResetMissedFrames();
    QueryPerformanceFrequency(&freq);
  }
};

// Test the high-precision wait logic
TEST_F(FpsLimiterTest, SmartWait_Accuracy) {
  // Target 16ms from now (approx 60 FPS)
  LARGE_INTEGER start, end;
  QueryPerformanceCounter(&start);

  int64_t targetUs = 16666; // 16.666 ms
  int64_t targetTicks = start.QuadPart + (targetUs * freq.QuadPart / 1000000);

  // This should block until targetTicks
  bool waited = limiter.SmartWait(targetTicks);

  QueryPerformanceCounter(&end);

  EXPECT_TRUE(waited);

  // Check error margin
  int64_t elapsedTicks = end.QuadPart - start.QuadPart;
  double elapsedMs = (double)elapsedTicks * 1000.0 / freq.QuadPart;

  // Should be at least 16.66ms
  EXPECT_GE(elapsedMs, 16.0);
  // Should not be excessively late (allow 1.5ms scheduling jitter)
  EXPECT_LT(elapsedMs, 18.2);
}

// Test what happens if we are already late
TEST_F(FpsLimiterTest, SmartWait_Late) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  // Target was 1ms ago
  int64_t targetTicks = now.QuadPart - (freq.QuadPart / 1000);

  // Should return false immediately
  bool waited = limiter.SmartWait(targetTicks);

  EXPECT_FALSE(waited);
}

// Test the SmartWait function directly
TEST_F(FpsLimiterTest, SmartWait_WithTarget) {
  // This directly tests the SmartWait mechanism
  LARGE_INTEGER start, end;
  QueryPerformanceCounter(&start);
  
  // Target 50ms in the future
  int64_t targetTicks = start.QuadPart + (50 * freq.QuadPart / 1000);
  bool waited = limiter.SmartWait(targetTicks);
  
  QueryPerformanceCounter(&end);
  
  EXPECT_TRUE(waited);
  
  double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
  EXPECT_GE(elapsedMs, 45.0);  // Should wait at least 45ms
  EXPECT_LT(elapsedMs, 80.0);  // But not too much more
}

// Test Apply with targetTimeTicks set (uses SmartWait path)
TEST_F(FpsLimiterTest, Apply_WithTargetTimeTicks) {
  // Setup for general FPS limit
  mockShm->runtimeState.isRecording = false;
  mockShm->fpsLimiter.SetGeneralEnabled(true);
  mockShm->fpsLimiter.SetGeneralFps(60);
  
  // Set a target time 50ms in the future - this triggers SmartWait
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  mockShm->fpsLimiter.targetTimeTicks.store(
      now.QuadPart + (50 * freq.QuadPart / 1000), std::memory_order_release);

  LARGE_INTEGER start, end;
  QueryPerformanceCounter(&start);

  limiter.Apply();

  QueryPerformanceCounter(&end);

  double elapsedMs =
      (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

  // Should wait approximately 50ms via SmartWait
  EXPECT_GE(elapsedMs, 40.0);
  EXPECT_LT(elapsedMs, 100.0);
}

// Test Apply timeout behavior when target is 0
TEST_F(FpsLimiterTest, Apply_NoTarget_ReturnsImmediately) {
  // Setup for general FPS limit
  mockShm->runtimeState.isRecording = false;
  mockShm->fpsLimiter.SetGeneralEnabled(true);
  mockShm->fpsLimiter.SetGeneralFps(60);
  
  // NOTE: When dbgShm is set but targetTimeTicks is 0, the Apply() function
  // returns immediately without waiting. This is the expected behavior.
  // The fallback spin-wait path is only entered when dbgShm is NULL.

  LARGE_INTEGER start, end;
  QueryPerformanceCounter(&start);

  limiter.Apply();

  QueryPerformanceCounter(&end);

  double elapsedMs =
      (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

  // Should return almost immediately (< 5ms)
  EXPECT_LT(elapsedMs, 5.0);
}
