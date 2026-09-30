#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>

#include "../common/capture_base.h"
#include "../common/inject_transport_snapshot.h"

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
    shm.SetVersion(SHARED_MEMORY_VERSION);
    shm.structSize.store(sizeof(SharedMemoryLayout), std::memory_order_release);
    shm.abiSignature.store(SHARED_MEMORY_ABI_SIGNATURE, std::memory_order_release);
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
    shm.frameRing.slots[0].displayTimingSequence = 77;
    shm.frameRing.slots[0].captureFlags = SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT |
                                          SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
    shm.frameRing.slots[0].displayTimingGeneration = 12;

    bool transitioned = false;
    bool result = capture.SignalFrameReady(&shm, 0, 12345, 1, &transitioned);

    EXPECT_TRUE(result);
    EXPECT_TRUE(transitioned);
    EXPECT_EQ(shm.frameRing.slots[0].timestamp, 12345);
    EXPECT_EQ(shm.frameRing.slots[0].fenceValue, 1u);
    EXPECT_EQ(shm.frameRing.slots[0].textureIndex, 0);
    EXPECT_EQ(shm.frameRing.slots[0].displayTimingSequence, 0u);
    EXPECT_EQ(shm.frameRing.slots[0].captureFlags, SHARED_FRAME_CAPTURE_NONE);
    EXPECT_EQ(shm.frameRing.slots[0].displayTimingGeneration, 0u);
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

// A producer that re-creates its transport (DX12 swapchain recreation closes the
// old shared handles right before creating new ones) can be handed the very same
// numeric handle values. Frames published before the re-creation must not be read
// with the new handles, and the handles alone cannot tell the generations apart.
TEST(CaptureBaseShmTest, RepublishWithReusedHandleValuesStartsANewTransportGeneration) {
    MockCapture capture;
    capture.width = 1920;
    capture.height = 1080;
    capture.sharedTextureHandles[0].store(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1A4)),
                                          std::memory_order_relaxed);
    capture.sharedFenceHandle.store(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1B8)), std::memory_order_relaxed);

    SharedMemoryLayout shm{};
    InitShm(shm);
    capture.PublishToSharedMemory(&shm);
    ASSERT_TRUE(capture.SignalFrameReady(&shm, 0, 1000, 1));
    const uint32_t oldFrameGeneration = shm.frameRing.slots[0].transportGeneration;
    EXPECT_NE(oldFrameGeneration, 0u);

    ce::InjectTransportSnapshot snapshot = ce::ReadInjectTransportSnapshot(shm, 0, false, oldFrameGeneration);
    EXPECT_TRUE(snapshot.consistent);
    EXPECT_EQ(snapshot.sharedHandle, 0x1A4u);
    EXPECT_EQ(snapshot.fenceHandle, 0x1B8u);

    // Re-created transport, identical handle values.
    capture.PublishToSharedMemory(&shm);
    snapshot = ce::ReadInjectTransportSnapshot(shm, 0, false, oldFrameGeneration);
    EXPECT_FALSE(snapshot.consistent);
    EXPECT_EQ(snapshot.sharedHandle, 0x1A4u);

    ASSERT_TRUE(capture.SignalFrameReady(&shm, 0, 2000, 1));
    const uint32_t newFrameGeneration = shm.frameRing.slots[1].transportGeneration;
    EXPECT_NE(newFrameGeneration, oldFrameGeneration);
    EXPECT_TRUE(ce::ReadInjectTransportSnapshot(shm, 0, false, newFrameGeneration).consistent);
}

TEST(CaptureBaseShmTest, TransportSnapshotRejectsAGenerationChangeBetweenItsReads) {
    EXPECT_TRUE(ce::IsInjectTransportSnapshotConsistent(7, 7, 7));
    EXPECT_FALSE(ce::IsInjectTransportSnapshotConsistent(7, 8, 7));
    EXPECT_FALSE(ce::IsInjectTransportSnapshotConsistent(8, 8, 7));
    // The stamp carries the low 32 bits of the generation.
    EXPECT_TRUE(ce::IsInjectTransportSnapshotConsistent(0x100000005ull, 0x100000005ull, 5u));
}

TEST(CaptureBaseShmTest, TransportSnapshotReadsTheEncoderTextureFenceWhenAdopted) {
    SharedMemoryLayout shm{};
    InitShm(shm);
    const uint32_t generation = static_cast<uint32_t>(shm.BeginTransportGeneration());
    shm.SetSharedHandle(2, 0x300);
    shm.SetFenceShareHandle(0x400);
    shm.encoderTextures.SetFenceHandle(0x500);

    const ce::InjectTransportSnapshot layerFence = ce::ReadInjectTransportSnapshot(shm, 2, false, generation);
    const ce::InjectTransportSnapshot encoderFence = ce::ReadInjectTransportSnapshot(shm, 2, true, generation);
    EXPECT_TRUE(layerFence.consistent);
    EXPECT_EQ(layerFence.sharedHandle, 0x300u);
    EXPECT_EQ(layerFence.fenceHandle, 0x400u);
    EXPECT_EQ(encoderFence.fenceHandle, 0x500u);
}

// Doom Eternal (native Vulkan) resized 4K -> 1440p mid-recording. Media had
// created its encoder textures at recording start (encoderTextures.ready), the
// layer never adopted them, and the resize published the new fence into the
// encoder-texture slot. Media kept reading the retired fence from the shared
// slot, and deferred every frame once new fence values passed its last value.
TEST(CaptureBaseShmTest, FenceRepublishedAfterResizeReachesTheSlotMediaReads) {
    SharedMemoryLayout shm{};
    InitShm(shm);
    shm.BeginTransportGeneration();
    ASSERT_FALSE(ce::PublishInjectFenceHandle(shm, 0x2050));

    shm.encoderTextures.SetFenceHandle(0xB0C);
    shm.encoderTextures.ready.store(true, std::memory_order_release);
    shm.useEncoderTextures.store(false, std::memory_order_release);

    const uint32_t resizedGeneration = static_cast<uint32_t>(shm.BeginTransportGeneration());
    EXPECT_FALSE(ce::PublishInjectFenceHandle(shm, 0x2184));

    const ce::InjectTransportSnapshot snapshot =
        ce::ReadInjectTransportSnapshot(shm, 0, ce::InjectFenceUsesEncoderTextureSlot(shm), resizedGeneration);
    EXPECT_TRUE(snapshot.consistent);
    EXPECT_EQ(snapshot.fenceHandle, 0x2184u);
    EXPECT_EQ(shm.encoderTextures.GetFenceHandle(), 0xB0Cu);
}

// DXVK: the swapchain predates the recording, so the layer starts on its IPC
// relay (fence in the shared slot). Media then creates its encoder textures and
// stores its own, never-signaled fence handle in the encoder-texture slot. When
// the layer adopts those textures, media reads that slot; it must find the
// layer's timeline fence, not media's handle, or every frame is deferred.
TEST(CaptureBaseShmTest, LateEncoderTextureAdoptionPublishesTheProducerFence) {
    SharedMemoryLayout shm{};
    InitShm(shm);
    shm.BeginTransportGeneration();
    ASSERT_FALSE(ce::PublishInjectFenceHandle(shm, 0x2200));

    shm.encoderTextures.SetFenceHandle(0xB0C);
    shm.encoderTextures.kmtReady.store(true, std::memory_order_release);
    shm.encoderTextures.ready.store(true, std::memory_order_release);

    const uint32_t adoptedGeneration = static_cast<uint32_t>(shm.BeginTransportGeneration());
    ce::AdoptEncoderTexturesWithFence(shm, 0x2210);

    ASSERT_TRUE(ce::InjectFenceUsesEncoderTextureSlot(shm));
    const ce::InjectTransportSnapshot snapshot =
        ce::ReadInjectTransportSnapshot(shm, 0, ce::InjectFenceUsesEncoderTextureSlot(shm), adoptedGeneration);
    EXPECT_TRUE(snapshot.consistent);
    EXPECT_EQ(snapshot.fenceHandle, 0x2210u);
    EXPECT_EQ(shm.GetFenceShareHandle(), 0x2200u);
}

TEST(CaptureBaseShmTest, FencePublishFollowsEncoderTextureAdoption) {
    SharedMemoryLayout shm{};
    InitShm(shm);
    shm.useEncoderTextures.store(true, std::memory_order_release);
    const uint32_t generation = static_cast<uint32_t>(shm.BeginTransportGeneration());
    shm.SetFenceShareHandle(0x400);
    EXPECT_TRUE(ce::PublishInjectFenceHandle(shm, 0x600));

    const ce::InjectTransportSnapshot snapshot =
        ce::ReadInjectTransportSnapshot(shm, 0, ce::InjectFenceUsesEncoderTextureSlot(shm), generation);
    EXPECT_EQ(snapshot.fenceHandle, 0x600u);
    EXPECT_EQ(shm.GetFenceShareHandle(), 0x400u);
}

// The generation must begin before a capture re-initialization closes the old
// handles, and again whenever the mapping being stamped is not the one (or not at
// the generation) the producer last published.
TEST(CaptureBaseShmTest, ProducerBeginsATransportGenerationBeforeReinitializingItsCapture) {
    EXPECT_TRUE(ce::ShouldBeginInjectTransportGeneration(false, true, 5, 5));
    EXPECT_TRUE(ce::ShouldBeginInjectTransportGeneration(false, false, 0, 5));
    EXPECT_FALSE(ce::ShouldBeginInjectTransportGeneration(true, true, 5, 5));
}

TEST(CaptureBaseShmTest, ProducerBeginsATransportGenerationForAReplacementHostMapping) {
    // Another mapping, possibly at the same address, starting at generation 0.
    EXPECT_TRUE(ce::ShouldBeginInjectTransportGeneration(true, false, 5, 5));
    EXPECT_TRUE(ce::ShouldBeginInjectTransportGeneration(true, true, 0, 5));
    // Only the low 32 bits are stamped on frames.
    EXPECT_FALSE(ce::ShouldBeginInjectTransportGeneration(true, true, 0x100000005ull, 5));
}

TEST(CaptureBaseShmTest, VulkanProducerPoolsCoverTheFullSharedTextureLeaseSpace) {
    EXPECT_EQ(ENCODER_TEXTURE_SLOT_COUNT, SHARED_TEXTURE_SLOT_COUNT);
    EXPECT_EQ(SHARED_TEXTURE_SLOT_COUNT, 16);
}

}  // namespace
