#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>

#include "../common/capture_base.h"

namespace {

constexpr uint32_t SHM_MAGIC = 0xCECAB001;

class MockCapture : public CaptureBase {
public:
    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        width = w;
        height = h;
        format = fmt;
    }
    void Cleanup() override {
        CleanupSharedHandles();
    }
};

static void InitShm(SharedMemoryLayout& shm) {
    shm.SetMagic(SHM_MAGIC);
    shm.SetVersion(31);
    shm.structSize.store(sizeof(SharedMemoryLayout), std::memory_order_release);
}

TEST(CaptureBaseShmTest, PublishToSharedMemoryCopiesMetadata) {
    MockCapture capture;
    capture.width = 1920;
    capture.height = 1080;
    capture.format = 87;
    capture.luidLow = 123;
    capture.luidHigh = 456;

    SharedMemoryLayout shm{};
    InitShm(shm);

    capture.PublishToSharedMemory(&shm);

    EXPECT_EQ(shm.GetWidth(), 1920u);
    EXPECT_EQ(shm.GetHeight(), 1080u);
    EXPECT_EQ(shm.GetFormat(), 87u);
    EXPECT_EQ(shm.GetLuidLowPart(), 123);
    EXPECT_EQ(shm.GetLuidHighPart(), 456);
}

TEST(CaptureBaseShmTest, CleanupSharedHandlesClearsOwnedFence) {
    MockCapture capture;

    HANDLE mockHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x12345678));
    capture.sharedFenceHandle.store(mockHandle, std::memory_order_relaxed);
    capture.sharedFenceHandleOwned.store(true, std::memory_order_relaxed);

    capture.CleanupSharedHandles();

    EXPECT_EQ(capture.sharedFenceHandle.load(std::memory_order_relaxed), (HANDLE)NULL);
    EXPECT_EQ(capture.sharedFenceHandleOwned.load(std::memory_order_relaxed), false);
}

TEST(CaptureBaseShmTest, CleanupSharedHandlesClearsUnownedFence) {
    MockCapture capture;

    capture.sharedFenceHandle.store(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x87654321)),
                                    std::memory_order_relaxed);
    capture.sharedFenceHandleOwned.store(false, std::memory_order_relaxed);

    capture.CleanupSharedHandles();

    EXPECT_EQ(capture.sharedFenceHandle.load(std::memory_order_relaxed), (HANDLE)NULL);
}

TEST(CaptureBaseShmTest, SignalFrameReadyWritesRingWithValidFlag) {
    MockCapture capture;

    SharedMemoryLayout shm{};
    InitShm(shm);

    bool transitioned = false;
    bool result = capture.SignalFrameReady(&shm, 0, 12345, 1, &transitioned);

    EXPECT_TRUE(result);
    EXPECT_TRUE(transitioned);
    EXPECT_EQ(shm.frameRing.slots[0].timestamp, 12345);
    EXPECT_EQ(shm.frameRing.slots[0].fenceValue, 1u);
    EXPECT_EQ(shm.frameRing.slots[0].textureIndex, 0);
    EXPECT_EQ(shm.frameRing.slots[0].valid.load(std::memory_order_acquire), 1);
}

TEST(CaptureBaseShmTest, OutstandingSlotScanDetectsValidSlots) {
    SharedMemoryLayout shm{};
    InitShm(shm);

    shm.frameRing.slots[0].valid.store(1, std::memory_order_release);
    shm.frameRing.slots[0].textureIndex = 0;
    shm.frameRing.writeIndex.store(1, std::memory_order_release);

    bool outstanding = HasOutstandingCaptureFrameLeases(&shm);
    EXPECT_TRUE(outstanding);

    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(&shm, 0));
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(&shm, 1));
}

}  // namespace
