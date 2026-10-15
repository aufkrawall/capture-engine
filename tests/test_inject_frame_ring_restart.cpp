#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "common/capture/capture_base.h"
#include "common/capture/inject_source_read_release.h"
#include "source_fragment_reader.h"

namespace {

struct ReadToken {
    std::shared_ptr<bool> completed;
    bool Completed() const { return *completed; }
    bool ArmEvent(HANDLE) const { return true; }
};

void Publish(FrameRingBuffer& ring, uint32_t index, int32_t textureIndex) {
    auto& slot = ring.slots[index % FRAME_RING_SIZE];
    slot.textureIndex = textureIndex;
    slot.valid.store(1, std::memory_order_release);
    ring.writeIndex.store(index + 1, std::memory_order_release);
}

}  // namespace

TEST(InjectFrameRingRestartTest, TimedOutReadSurvivesRestartAndDiscardOfTheNextRecordingsFrame) {
    for (const uint32_t start : {7u, UINT32_MAX - 1}) {
        SCOPED_TRACE(start);
        auto shared = std::make_unique<SharedMemoryLayout>();
        auto& ring = shared->frameRing;
        ring.readIndex.store(start, std::memory_order_release);
        Publish(ring, start, 5);
        Publish(ring, start + 1, 6);
        auto oldLeases = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
        auto completed = std::make_shared<bool>(false);
        ce::SourceReadReleaseQueue<ReadToken> reads;
        reads.Defer(oldLeases->Acquire(start), ReadToken{completed}, 1);
        ring.ingestIndex.store(start + 1, std::memory_order_release);
        ASSERT_FALSE(reads.Drain(0));

        // Stop and then start discard the unread suffix, preserving the old read.
        uint32_t discarded = 0;
        ASSERT_TRUE(ce::DiscardUnreadInjectFrameRingSlots(ring, discarded));
        EXPECT_EQ(discarded, 1u);
        ASSERT_TRUE(ce::DiscardUnreadInjectFrameRingSlots(ring, discarded));
        EXPECT_EQ(discarded, 0u);
        EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), start);
        EXPECT_TRUE(IsCaptureTextureSlotOutstanding(shared.get(), 5));
        EXPECT_FALSE(IsCaptureTextureSlotOutstanding(shared.get(), 6));

        const uint32_t resume = ring.ingestIndex.load(std::memory_order_acquire);
        ASSERT_EQ(resume, start + 2);
        Publish(ring, resume, 6);
        auto newLeases = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
        auto newFrame = newLeases->Acquire(resume);
        ring.ingestIndex.store(resume + 1, std::memory_order_release);
        newFrame.Reset();
        EXPECT_EQ(reads.Pending(), 1u);
        EXPECT_TRUE(IsCaptureTextureSlotOutstanding(shared.get(), 5));
        EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), start);

        *completed = true;
        EXPECT_EQ(reads.ReleaseCompleted(2), 1u);
        EXPECT_FALSE(IsCaptureTextureSlotOutstanding(shared.get(), 5));
        EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), resume + 1);
    }
}

TEST(InjectFrameRingRestartTest, DiscardAcknowledgesAnEntireUnreadRingAndKeepsTheNextPublication) {
    FrameRingBuffer ring;
    for (uint32_t index = 0; index < static_cast<uint32_t>(FRAME_RING_SIZE); ++index)
        Publish(ring, index, 0);
    uint32_t discarded = 0;
    ASSERT_TRUE(ce::DiscardUnreadInjectFrameRingSlots(ring, discarded));
    EXPECT_EQ(discarded, static_cast<uint32_t>(FRAME_RING_SIZE));
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), static_cast<uint32_t>(FRAME_RING_SIZE));
    EXPECT_EQ(ring.ingestIndex.load(std::memory_order_acquire), static_cast<uint32_t>(FRAME_RING_SIZE));
    Publish(ring, FRAME_RING_SIZE, 0);
    EXPECT_EQ(ring.slots[0].valid.load(std::memory_order_acquire), 1u);
}

TEST(InjectFrameRingRestartTest, PriorReadCompletionCannotReleaseTheNextRecordingsFrame) {
    auto shared = std::make_unique<SharedMemoryLayout>();
    auto& ring = shared->frameRing;
    Publish(ring, 0, 5);
    ring.ingestIndex.store(1, std::memory_order_release);
    auto oldLeases = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
    auto oldCompleted = std::make_shared<bool>(false);
    ce::SourceReadReleaseQueue<ReadToken> reads;
    reads.Defer(oldLeases->Acquire(0), ReadToken{oldCompleted}, 1);
    ASSERT_FALSE(reads.Drain(0));
    uint32_t discarded = 0;
    ASSERT_TRUE(ce::DiscardUnreadInjectFrameRingSlots(ring, discarded));
    EXPECT_EQ(discarded, 0u);

    const uint32_t resume = ring.ingestIndex.load(std::memory_order_acquire);
    ASSERT_EQ(resume, 1u);
    Publish(ring, resume, 6);
    ring.ingestIndex.store(resume + 1, std::memory_order_release);
    auto newLeases = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
    auto newCompleted = std::make_shared<bool>(false);
    reads.Defer(newLeases->Acquire(resume), ReadToken{newCompleted}, 2);
    *oldCompleted = true;
    EXPECT_EQ(reads.ReleaseCompleted(3), 1u);
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), resume);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(shared.get(), 6));
    *newCompleted = true;
    EXPECT_EQ(reads.ReleaseCompleted(4), 1u);
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), resume + 1);
}

TEST(InjectFrameRingRestartTest, CorruptCursorWindowsDoNotAcknowledgeAnySlot) {
    for (const uint32_t ingest : {6u, 10u, 7u}) {
        FrameRingBuffer ring;
        ring.readIndex.store(7, std::memory_order_release);
        Publish(ring, 7, 5);
        ring.ingestIndex.store(ingest, std::memory_order_release);
        if (ingest == 7)
            ring.writeIndex.store(7 + FRAME_RING_SIZE + 1, std::memory_order_release);
        const uint32_t write = ring.writeIndex.load(std::memory_order_acquire);
        uint32_t discarded = 10;
        EXPECT_FALSE(ce::DiscardUnreadInjectFrameRingSlots(ring, discarded));
        EXPECT_EQ(discarded, 0u);
        EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 7u);
        EXPECT_EQ(ring.ingestIndex.load(std::memory_order_acquire), ingest);
        EXPECT_EQ(ring.writeIndex.load(std::memory_order_acquire), write);
        EXPECT_EQ(ring.slots[7].valid.load(std::memory_order_acquire), 1u);
    }
}

// These coordinator units are not linked into unit_tests. Keep their cursor
// wiring tied to the tested ring ownership behavior.
TEST(InjectFrameRingRestartTest, CoordinatorResumesIngestInsteadOfReacquiringPendingReadSlots) {
    const auto media = std::filesystem::current_path() / "captureengine" / "media";
    const std::string ingest = ce::test_source::ReadLogicalSource(media / "media_main_threads_inject.cpp");
    const std::string reset = ce::test_source::ReadLogicalSource(media / "media_main_wgc.cpp");
    const std::string start = ce::test_source::ReadLogicalSource(media / "media_main_start.cpp");
    ASSERT_FALSE(ingest.empty());
    ASSERT_FALSE(reset.empty());
    EXPECT_NE(ingest.find("uint32_t localReadIndex = media_main_g_pSharedMem->frameRing.ingestIndex.load("),
              std::string::npos);
    EXPECT_EQ(ingest.find("uint32_t localReadIndex = media_main_g_pSharedMem->frameRing.readIndex.load("),
              std::string::npos);
    EXPECT_NE(reset.find("ce::DiscardUnreadInjectFrameRingSlots(ring, discarded)"), std::string::npos);
    EXPECT_EQ(reset.find("ring.readIndex.store(writeIndex"), std::string::npos);
    EXPECT_NE(start.find("static_cast<uint32_t>(ingestIndex - readIndex)"), std::string::npos);
}
