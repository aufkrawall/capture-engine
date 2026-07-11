// Tests for the shared CaptureBase class
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <thread>
#include "../hook/common/capture_base.h"

// Concrete implementation for testing
class TestCapture : public CaptureBase {
public:
    int createCallCount = 0;
    int cleanupCallCount = 0;

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        width = w;
        height = h;
        format = fmt;
        initialized = true;
        createCallCount++;
    }

    void Cleanup() override {
        initialized = false;
        cleanupCallCount++;
    }
};

TEST(CaptureBaseTest, InitialState) {
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
    for (const auto& pending : capture.pendingRing) {
        EXPECT_EQ(pending.timestampQPC, 0);
        EXPECT_EQ(pending.fenceValue, 0u);
        EXPECT_EQ(pending.completionFenceValue, 0u);
        EXPECT_EQ(pending.apiData, nullptr);
        EXPECT_EQ(pending.syncObject, 0u);
    }
}

TEST(CaptureBaseTest, CreateSharedResources) {
    TestCapture capture;

    capture.CreateSharedResources(1920, 1080, 87);  // DXGI_FORMAT_B8G8R8A8_UNORM

    EXPECT_TRUE(capture.initialized);
    EXPECT_EQ(capture.width, 1920);
    EXPECT_EQ(capture.height, 1080);
    EXPECT_EQ(capture.format, 87);
    EXPECT_EQ(capture.createCallCount, 1);
}

TEST(CaptureBaseTest, Cleanup) {
    TestCapture capture;

    capture.CreateSharedResources(1920, 1080, 87);
    EXPECT_TRUE(capture.initialized);

    capture.Cleanup();
    EXPECT_FALSE(capture.initialized);
    EXPECT_EQ(capture.cleanupCallCount, 1);
}

TEST(CaptureBaseTest, HasPendingSpace) {
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

TEST(CaptureBaseTest, EnqueueFrame) {
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

TEST(CaptureBaseTest, EnqueueMultipleFrames) {
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

TEST(CaptureBaseTest, EnqueueWhenFull) {
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

TEST(CaptureBaseTest, AdvanceWriteIndex) {
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

TEST(CaptureBaseTest, CompletionFenceTracking) {
    TestCapture capture;

    EXPECT_EQ(capture.completionFenceValue.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(capture.pendingCaptureWaitValue.load(std::memory_order_relaxed), 0);

    // Enqueue frames and check completion fence advances
    capture.EnqueueFrame(1000, 10, 0, nullptr);
    EXPECT_EQ(capture.completionFenceValue.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(capture.pendingCaptureWaitValue.load(std::memory_order_relaxed), 1);

    capture.EnqueueFrame(2000, 20, 1, nullptr);
    EXPECT_EQ(capture.completionFenceValue.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(capture.pendingCaptureWaitValue.load(std::memory_order_relaxed), 2);
}

TEST(CaptureBaseTest, ResetForNewRecordingClearsPendingState) {
    TestCapture capture;
    capture.recordingSessionID.store(41, std::memory_order_relaxed);
    capture.droppedFrames.store(7, std::memory_order_relaxed);
    capture.writeIndex.store(3, std::memory_order_relaxed);
    capture.EnqueueFrame(1000, 1, 0, nullptr);
    capture.EnqueueFrame(2000, 2, 1, nullptr);
    capture.pendingReadIdx.store(1, std::memory_order_relaxed);

    capture.ResetForNewRecording();

    EXPECT_EQ(capture.recordingSessionID.load(std::memory_order_relaxed), 42);
    EXPECT_EQ(capture.droppedFrames.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(capture.pendingWriteIdx.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(capture.pendingReadIdx.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(capture.writeIndex.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(capture.completionFenceValue.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(capture.pendingCaptureWaitValue.load(std::memory_order_relaxed), 0u);
    for (const auto& pending : capture.pendingRing) {
        EXPECT_EQ(pending.timestampQPC, 0);
        EXPECT_EQ(pending.fenceValue, 0u);
        EXPECT_EQ(pending.completionFenceValue, 0u);
        EXPECT_EQ(pending.apiData, nullptr);
        EXPECT_EQ(pending.syncObject, 0u);
    }
}

TEST(CaptureBaseTest, CleanupSharedHandlesClosesOnlyOwnedNtHandles) {
    TestCapture capture;
    HANDLE resourceOwnedHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ntHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(resourceOwnedHandle, nullptr);
    ASSERT_NE(ntHandle, nullptr);

    capture.sharedTextureHandles[0].store(resourceOwnedHandle, std::memory_order_relaxed);
    capture.sharedTextureHandles[1].store(ntHandle, std::memory_order_relaxed);
    capture.sharedTextureHandleOwned[1].store(true, std::memory_order_relaxed);
    capture.CleanupSharedHandles();

    DWORD flags = 0;
    EXPECT_TRUE(GetHandleInformation(resourceOwnedHandle, &flags));
    EXPECT_FALSE(GetHandleInformation(ntHandle, &flags));
    CloseHandle(resourceOwnedHandle);
}

TEST(CaptureBaseTest, StartCaptureThreadRejectsConcurrentSecondStart) {
    TestCapture capture;
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(started, nullptr);
    ASSERT_NE(release, nullptr);
    std::atomic<int> starts{0};

    capture.StartCaptureThread([&]() {
        starts.fetch_add(1, std::memory_order_relaxed);
        SetEvent(started);
        WaitForSingleObject(release, INFINITE);
    });
    ASSERT_EQ(WaitForSingleObject(started, 5000), WAIT_OBJECT_0);

    capture.StartCaptureThread([&]() { starts.fetch_add(100, std::memory_order_relaxed); });
    EXPECT_EQ(starts.load(std::memory_order_relaxed), 1);

    SetEvent(release);
    capture.StopCaptureThread();
    EXPECT_FALSE(capture.captureThreadRunning.load(std::memory_order_acquire));
    CloseHandle(release);
    CloseHandle(started);
}

TEST(CaptureBaseTest, PublishToSharedMemoryCopiesCaptureMetadata) {
    TestCapture capture;
    SharedMemoryLayout sharedMem;

    capture.width = 2560;
    capture.height = 1440;
    capture.format = 87;
    capture.luidLow = 123;
    capture.luidHigh = 456;

    std::array<uintptr_t, CAPTURE_TEXTURE_COUNT> expectedHandles{};
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
        expectedHandles[i] = static_cast<uintptr_t>(0x1000 + i * 0x40);
        capture.sharedTextureHandles[i].store(reinterpret_cast<HANDLE>(expectedHandles[i]), std::memory_order_relaxed);
    }
    const uintptr_t expectedFence = static_cast<uintptr_t>(0xDEAD0000);
    capture.sharedFenceHandle.store(reinterpret_cast<HANDLE>(expectedFence), std::memory_order_relaxed);

    capture.PublishToSharedMemory(&sharedMem);

    EXPECT_EQ(sharedMem.GetWidth(), 2560u);
    EXPECT_EQ(sharedMem.GetHeight(), 1440u);
    EXPECT_EQ(sharedMem.GetFormat(), 87u);
    EXPECT_EQ(sharedMem.GetLuidLowPart(), 123);
    EXPECT_EQ(sharedMem.GetLuidHighPart(), 456);
    EXPECT_EQ(sharedMem.GetFenceShareHandle(), static_cast<uint64_t>(expectedFence));
    EXPECT_EQ(sharedMem.GetSourcePid(), GetCurrentProcessId());
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
        EXPECT_EQ(sharedMem.GetSharedHandle(i), static_cast<uint64_t>(expectedHandles[i]));
    }
}

TEST(CaptureBaseTest, SharedTextureSlotCountRejectsOutOfRangeIndex) {
    SharedMemoryLayout sharedMem;

    EXPECT_EQ(CAPTURE_TEXTURE_COUNT, SHARED_TEXTURE_SLOT_COUNT);
    EXPECT_TRUE(IsValidTextureIndex(SHARED_TEXTURE_SLOT_COUNT - 1));
    EXPECT_FALSE(IsValidTextureIndex(SHARED_TEXTURE_SLOT_COUNT));

    sharedMem.SetSharedHandle(SHARED_TEXTURE_SLOT_COUNT - 1, 0xABCDEFu);
    sharedMem.SetSharedHandle(SHARED_TEXTURE_SLOT_COUNT, 0x123456u);

    EXPECT_EQ(sharedMem.GetSharedHandle(SHARED_TEXTURE_SLOT_COUNT - 1), 0xABCDEFu);
    EXPECT_EQ(sharedMem.GetSharedHandle(SHARED_TEXTURE_SLOT_COUNT), 0u);
}

TEST(CaptureBaseTest, SignalFrameReadyWritesRingAndDropsWhenFull) {
    TestCapture capture;
    SharedMemoryLayout sharedMem;

    capture.SignalFrameReady(&sharedMem, 3, 123456, 99);

    EXPECT_EQ(sharedMem.frameRing.writeIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(sharedMem.frameRing.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(sharedMem.frameRing.droppedFrames.load(std::memory_order_relaxed), 0u);

    const auto& first = sharedMem.frameRing.slots[0];
    EXPECT_EQ(first.textureIndex, 3);
    EXPECT_EQ(first.timestamp, 123456);
    EXPECT_EQ(first.fenceValue, 99u);
    EXPECT_EQ(first.frameIndex, 0u);
    EXPECT_EQ(first.sourcePid, GetCurrentProcessId());
    EXPECT_EQ(first.valid.load(std::memory_order_acquire), 1u);

    for (uint32_t i = 1; i < FRAME_RING_SIZE; ++i) {
        capture.SignalFrameReady(&sharedMem, static_cast<int>(i % CAPTURE_TEXTURE_COUNT), i * 1000, i);
    }
    EXPECT_EQ(sharedMem.frameRing.writeIndex.load(std::memory_order_acquire), FRAME_RING_SIZE);
    capture.SignalFrameReady(&sharedMem, 0, 999999, 777);
    EXPECT_EQ(sharedMem.frameRing.writeIndex.load(std::memory_order_acquire), FRAME_RING_SIZE);
    EXPECT_EQ(sharedMem.frameRing.droppedFrames.load(std::memory_order_relaxed), 1u);
}

TEST(CaptureBaseTest, OutstandingTextureSlotScanHonorsValidityAndWraparound) {
    SharedMemoryLayout sharedMem;
    auto& ring = sharedMem.frameRing;

    ring.readIndex.store(FRAME_RING_SIZE - 2, std::memory_order_relaxed);
    ring.writeIndex.store(FRAME_RING_SIZE + 1, std::memory_order_relaxed);
    FrameSlot& first = ring.slots[FRAME_RING_SIZE - 2];
    first.textureIndex = 3;
    first.valid.store(1, std::memory_order_release);
    FrameSlot& ignored = ring.slots[FRAME_RING_SIZE - 1];
    ignored.textureIndex = 5;
    ignored.valid.store(0, std::memory_order_release);
    FrameSlot& wrapped = ring.slots[0];
    wrapped.textureIndex = 7;
    wrapped.valid.store(1, std::memory_order_release);

    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(&sharedMem, 3));
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(&sharedMem, 5));
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(&sharedMem, 7));
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(&sharedMem, 1));
    EXPECT_EQ(FindAvailableCaptureTextureSlot(&sharedMem, 3, 8), 4);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(nullptr, 3));
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(&sharedMem, -1));

    for (uint32_t i = 0; i < 8; ++i) {
        FrameSlot& slot = ring.slots[i];
        slot.textureIndex = static_cast<int32_t>(i);
        slot.valid.store(1, std::memory_order_release);
    }
    ring.readIndex.store(0, std::memory_order_relaxed);
    ring.writeIndex.store(8, std::memory_order_release);
    EXPECT_EQ(FindAvailableCaptureTextureSlot(&sharedMem, 0, 8), -1);
}

TEST(CaptureBaseTest, OutstandingFrameLeaseScanProtectsResourceGeneration) {
    SharedMemoryLayout sharedMem;
    auto& ring = sharedMem.frameRing;

    EXPECT_FALSE(HasOutstandingCaptureFrameLeases(nullptr));
    EXPECT_FALSE(HasOutstandingCaptureFrameLeases(&sharedMem));

    ring.readIndex.store(FRAME_RING_SIZE - 1, std::memory_order_relaxed);
    ring.writeIndex.store(FRAME_RING_SIZE + 2, std::memory_order_relaxed);
    ring.slots[FRAME_RING_SIZE - 1].valid.store(0, std::memory_order_release);
    ring.slots[0].valid.store(1, std::memory_order_release);
    ring.slots[1].valid.store(0, std::memory_order_release);
    EXPECT_TRUE(HasOutstandingCaptureFrameLeases(&sharedMem));

    ring.slots[0].valid.store(0, std::memory_order_release);
    EXPECT_FALSE(HasOutstandingCaptureFrameLeases(&sharedMem));

    // Even corrupted/overrun indices remain bounded and conservatively inspect
    // every physical slot that can still contain frame metadata.
    ring.readIndex.store(0, std::memory_order_relaxed);
    ring.writeIndex.store(FRAME_RING_SIZE + 4, std::memory_order_relaxed);
    ring.slots[3].valid.store(1, std::memory_order_release);
    EXPECT_TRUE(HasOutstandingCaptureFrameLeases(&sharedMem));
}
