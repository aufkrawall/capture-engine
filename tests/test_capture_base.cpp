// Tests for the shared CaptureBase class
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "../hook/common/capture_base.h"

// Concrete implementation for testing
class TestCapture : public CaptureBase {
public:
    int createCallCount = 0;
    int cleanupCallCount = 0;

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override
    {
        width = w;
        height = h;
        format = fmt;
        initialized = true;
        createCallCount++;
    }

    void Cleanup() override
    {
        initialized = false;
        cleanupCallCount++;
    }
};

TEST(CaptureBaseTest, InitialState)
{
    TestCapture capture;

    EXPECT_FALSE(capture.initialized);
    EXPECT_EQ(capture.width, 0);
    EXPECT_EQ(capture.height, 0);
    EXPECT_EQ(capture.format, 0);
    EXPECT_EQ(capture.writeIndex, 0);
    EXPECT_EQ(capture.fenceValue, 0);

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        EXPECT_EQ(capture.sharedTextureHandles[i], nullptr);
    }
}

TEST(CaptureBaseTest, CreateSharedResources)
{
    TestCapture capture;

    capture.CreateSharedResources(1920, 1080, 87);  // DXGI_FORMAT_B8G8R8A8_UNORM

    EXPECT_TRUE(capture.initialized);
    EXPECT_EQ(capture.width, 1920);
    EXPECT_EQ(capture.height, 1080);
    EXPECT_EQ(capture.format, 87);
    EXPECT_EQ(capture.createCallCount, 1);
}

TEST(CaptureBaseTest, Cleanup)
{
    TestCapture capture;

    capture.CreateSharedResources(1920, 1080, 87);
    EXPECT_TRUE(capture.initialized);

    capture.Cleanup();
    EXPECT_FALSE(capture.initialized);
    EXPECT_EQ(capture.cleanupCallCount, 1);
}

TEST(CaptureBaseTest, HasPendingSpace)
{
    TestCapture capture;

    // Initially empty - should have space
    EXPECT_TRUE(capture.HasPendingSpace());

    // Fill up the ring buffer
    for (int i = 0; i < CAPTURE_RING_SIZE; i++) {
        EXPECT_TRUE(capture.EnqueueFrame(i * 1000, i, i % 4, nullptr));
    }

    // Should be full now
    EXPECT_FALSE(capture.HasPendingSpace());
}

TEST(CaptureBaseTest, EnqueueFrame)
{
    TestCapture capture;

    // Enqueue a frame
    bool result = capture.EnqueueFrame(12345, 100, 2, (void*)0xDEADBEEF);
    EXPECT_TRUE(result);

    // Check pending ring was updated
    EXPECT_EQ(capture.pendingWriteIdx.load(), 1);
    EXPECT_EQ(capture.pendingReadIdx.load(), 0);

    // Check the frame data
    const auto& frame = capture.pendingRing[0];
    EXPECT_EQ(frame.timestampQPC, 12345);
    EXPECT_EQ(frame.fenceValue, 100);
    EXPECT_EQ(frame.backBufferIndex, 2);
    EXPECT_EQ(frame.apiData, (void*)0xDEADBEEF);
}

TEST(CaptureBaseTest, EnqueueMultipleFrames)
{
    TestCapture capture;

    // Enqueue multiple frames
    for (int i = 0; i < 5; i++) {
        EXPECT_TRUE(capture.EnqueueFrame(i * 1000, i * 10, i % 4, nullptr));
    }

    EXPECT_EQ(capture.pendingWriteIdx.load(), 5);
    EXPECT_EQ(capture.pendingReadIdx.load(), 0);

    // Verify data is in ring order
    for (int i = 0; i < 5; i++) {
        EXPECT_EQ(capture.pendingRing[i].timestampQPC, i * 1000);
        EXPECT_EQ(capture.pendingRing[i].fenceValue, i * 10);
    }
}

TEST(CaptureBaseTest, EnqueueWhenFull)
{
    TestCapture capture;

    // Fill the ring buffer
    for (int i = 0; i < CAPTURE_RING_SIZE; i++) {
        EXPECT_TRUE(capture.EnqueueFrame(i, i, 0, nullptr));
    }

    // Next enqueue should fail
    EXPECT_FALSE(capture.EnqueueFrame(999, 999, 0, nullptr));

    // Write index should not have advanced
    EXPECT_EQ(capture.pendingWriteIdx.load(), CAPTURE_RING_SIZE);
}

TEST(CaptureBaseTest, AdvanceWriteIndex)
{
    TestCapture capture;

    EXPECT_EQ(capture.writeIndex, 0);

    int idx = capture.AdvanceWriteIndex();
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(capture.writeIndex, 1);

    idx = capture.AdvanceWriteIndex();
    EXPECT_EQ(idx, 1);
    EXPECT_EQ(capture.writeIndex, 2);

    // Test wraparound
    capture.writeIndex = CAPTURE_TEXTURE_COUNT - 1;
    idx = capture.AdvanceWriteIndex();
    EXPECT_EQ(idx, CAPTURE_TEXTURE_COUNT - 1);
    EXPECT_EQ(capture.writeIndex, 0);
}

TEST(CaptureBaseTest, CompletionFenceTracking)
{
    TestCapture capture;

    EXPECT_EQ(capture.completionFenceValue, 0);
    EXPECT_EQ(capture.pendingCaptureWaitValue, 0);

    // Enqueue frames and check completion fence advances
    capture.EnqueueFrame(1000, 10, 0, nullptr);
    EXPECT_EQ(capture.completionFenceValue, 1);
    EXPECT_EQ(capture.pendingCaptureWaitValue, 1);

    capture.EnqueueFrame(2000, 20, 1, nullptr);
    EXPECT_EQ(capture.completionFenceValue, 2);
    EXPECT_EQ(capture.pendingCaptureWaitValue, 2);
}
