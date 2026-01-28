#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "../hook/common/fps_limiter.h"

class FpsLimiterTest : public ::testing::Test {
protected:
    std::unique_ptr<SharedMemoryLayout> mockShm;
    FpsLimiter limiter;
    LARGE_INTEGER freq;

    void SetUp() override
    {
        mockShm = std::make_unique<SharedMemoryLayout>();
        limiter.SetSharedMemory(mockShm.get());
        limiter.ResetMissedFrames();
        QueryPerformanceFrequency(&freq);
    }
};

// Test the high-precision wait logic
TEST_F(FpsLimiterTest, SmartWait_Accuracy)
{
    // Target 16ms from now (approx 60 FPS)
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    int64_t targetUs = 16666;  // 16.666 ms
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
TEST_F(FpsLimiterTest, SmartWait_Late)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Target was 1ms ago
    int64_t targetTicks = now.QuadPart - (freq.QuadPart / 1000);

    // Should return false immediately
    bool waited = limiter.SmartWait(targetTicks);

    EXPECT_FALSE(waited);
}

// Test Apply logic (Limited integration test without external process)
TEST_F(FpsLimiterTest, Apply_Fallback_Spin)
{
    // Setup for general FPS limit
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.generalEnabled = true;
    mockShm->fpsLimiter.generalFps = 60;

    // We are not initializing events, so it should fall back to spin wait
    // But since we aren't updating releaseCount, it might timeout or wait 100ms
    // The fallback logic waits until releaseCount < requestCount
    // shm->fpsLimiter.releaseCount is 0.
    // requestCount will be incremented to 1.
    // So loop condition: 0 < 1 is TRUE. It will wait.

    // To test this without hanging, we can spawn a thread to update releaseCount
    std::atomic<bool> threadRunning = true;
    std::thread releaser([&]() {
        Sleep(50);  // Wait 50ms
        mockShm->fpsLimiter.releaseCount.store(1, std::memory_order_release);
    });

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);
    threadRunning = false;
    releaser.join();

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Should be approx 50ms
    EXPECT_GE(elapsedMs, 45.0);
    EXPECT_LT(elapsedMs, 70.0);  // Allow some slop

    // No missed frames expected if we responded in 100ms limit
    EXPECT_EQ(limiter.GetMissedFrames(), 0);
}

TEST_F(FpsLimiterTest, Apply_Timeout)
{
    mockShm->runtimeState.isRecording = false;
    mockShm->fpsLimiter.generalEnabled = true;
    mockShm->fpsLimiter.generalFps = 60;

    // Don't release

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    limiter.Apply();

    QueryPerformanceCounter(&end);

    double elapsedMs = (double)(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;

    // Should timeout after 100ms (hardcoded in fallback path)
    EXPECT_GE(elapsedMs, 100.0);

    // Should increment missed frames
    EXPECT_EQ(limiter.GetMissedFrames(), 1);
}
