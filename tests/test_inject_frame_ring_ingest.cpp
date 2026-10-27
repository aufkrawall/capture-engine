#include <gtest/gtest.h>

#include <memory>

#include "common/capture/inject_frame_ring_ingest.h"

// The media ingest thread's cursor over the inject frame ring (common/capture/inject_frame_ring_ingest.h). A
// recording start rejects a ring whose readIndex passed its ingestIndex as corrupt; HotS session 20261027_235200 left
// one behind (write=14343 ingest=14342 read=14343) and every later recording of that game failed.

namespace {

void Publish(FrameRingBuffer& ring, uint32_t index) {
    auto& slot = ring.slots[index % FRAME_RING_SIZE];
    slot.textureIndex = static_cast<int32_t>(index % SHARED_TEXTURE_SLOT_COUNT);
    slot.valid.store(1, std::memory_order_release);
    ring.writeIndex.store(index + 1, std::memory_order_release);
}

// The check StartRecording applies before it accepts the ring (captureengine/media/media_main_start.cpp).
bool PassesRecordingStartRingCheck(const FrameRingBuffer& ring) {
    const uint32_t write = ring.writeIndex.load(std::memory_order_acquire);
    const uint32_t ingest = ring.ingestIndex.load(std::memory_order_acquire);
    const uint32_t read = ring.readIndex.load(std::memory_order_acquire);
    return IsFrameRingWindowValid(write, read) &&
           static_cast<uint32_t>(ingest - read) <= static_cast<uint32_t>(write - read);
}

struct Consumer {
    explicit Consumer(FrameRingBuffer& ring)
        : leases(std::make_shared<ce::InjectFrameRingLeaseState>(&ring)),
          cursor(ring, leases) {}

    std::shared_ptr<ce::InjectFrameRingLeaseState> leases;
    ce::InjectFrameRingIngestCursor cursor;
};

}  // namespace

TEST(InjectFrameRingIngestTest, ResumesAtIngestionNotAtTheAcknowledgement) {
    auto ring = std::make_unique<FrameRingBuffer>();
    for (uint32_t index = 0; index < 4; ++index)
        Publish(*ring, index);
    ring->ingestIndex.store(3, std::memory_order_release);  // 0..2 ingested, their reads still in flight

    Consumer consumer(*ring);
    EXPECT_EQ(consumer.cursor.Next(), 3u);
    EXPECT_EQ(&consumer.cursor.NextSlot(), &ring->slots[3]);
}

// A frame the consumer takes can be released by the encoder before the ingest thread runs again. The old thread
// pushed the frame first and stored the advanced ingestIndex afterwards; a consumer that died between the two left
// readIndex past ingestIndex for good.
TEST(InjectFrameRingIngestTest, ATakenFrameReleasedAtOnceStillLeavesAnAcceptableRing) {
    for (const uint32_t start : {0u, UINT32_MAX - 1}) {
        SCOPED_TRACE(start);
        auto ring = std::make_unique<FrameRingBuffer>();
        ring->readIndex.store(start, std::memory_order_release);
        ring->ingestIndex.store(start, std::memory_order_release);
        Publish(*ring, start);
        Publish(*ring, start + 1);
        Consumer consumer(*ring);

        ce::InjectFrameRingLease frame = consumer.cursor.Take();
        EXPECT_EQ(ring->ingestIndex.load(std::memory_order_acquire), start + 1)
            << "ingestion moves past the slot before its lease can be released";
        EXPECT_EQ(ring->slots[start % FRAME_RING_SIZE].valid.load(std::memory_order_acquire), 1u)
            << "the slot stays leased until the frame's owner releases it";
        frame.Reset();
        EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), start + 1);
        EXPECT_TRUE(PassesRecordingStartRingCheck(*ring));

        consumer.cursor.Skip();
        EXPECT_EQ(ring->ingestIndex.load(std::memory_order_acquire), start + 2);
        EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), start + 2);
        EXPECT_TRUE(PassesRecordingStartRingCheck(*ring));
    }
}

TEST(InjectFrameRingIngestTest, ASkipBehindAnOutstandingLeaseKeepsTheAcknowledgementBehind) {
    auto ring = std::make_unique<FrameRingBuffer>();
    Publish(*ring, 0);
    Publish(*ring, 1);
    Consumer consumer(*ring);
    ce::InjectFrameRingLease encoding = consumer.cursor.Take();
    consumer.cursor.Skip();
    EXPECT_EQ(ring->ingestIndex.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), 0u) << "slot 0 is still read by the encoder";
    EXPECT_TRUE(PassesRecordingStartRingCheck(*ring));
    encoding.Reset();
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), 2u);
}

TEST(InjectFrameRingIngestTest, AnOrdinaryBacklogIsNoCursorFault) {
    auto ring = std::make_unique<FrameRingBuffer>();
    for (uint32_t index = 0; index < static_cast<uint32_t>(FRAME_RING_SIZE); ++index)
        Publish(*ring, index);
    Consumer consumer(*ring);
    const ce::InjectFrameRingResync resync =
        consumer.cursor.ResyncIfOverrun(ring->writeIndex.load(std::memory_order_acquire));
    EXPECT_FALSE(resync.faulted);
    EXPECT_EQ(consumer.cursor.Next(), 0u);
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), 0u);
}

// The HotS cursor: 14343 against a ring whose writeIndex reads 0. The old catch-up walked every position up to
// 0xFFFFFFFF; the resync touches the ring window only.
TEST(InjectFrameRingIngestTest, ACursorFaultResyncsWithoutWalkingTheCursorDistance) {
    auto ring = std::make_unique<FrameRingBuffer>();
    ring->ingestIndex.store(14343, std::memory_order_release);
    Consumer consumer(*ring);

    const ce::InjectFrameRingResync resync = consumer.cursor.ResyncIfOverrun(0);
    ASSERT_TRUE(resync.faulted);
    EXPECT_EQ(resync.cursor, 14343u);
    EXPECT_EQ(resync.skipped, static_cast<uint32_t>(FRAME_RING_SIZE) - 1);
    EXPECT_EQ(consumer.cursor.Next(), UINT32_MAX);
    EXPECT_EQ(ring->ingestIndex.load(std::memory_order_acquire), UINT32_MAX);
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), 0u) << "nothing of the empty ring was acknowledged";
}

TEST(InjectFrameRingIngestTest, ACursorFaultReleasesTheInWindowSlotsButTheNewest) {
    auto ring = std::make_unique<FrameRingBuffer>();
    const uint32_t write = 100;
    const uint32_t read = write - static_cast<uint32_t>(FRAME_RING_SIZE);
    ring->readIndex.store(read, std::memory_order_release);
    for (uint32_t index = read; index != write; ++index)
        Publish(*ring, index);
    ring->ingestIndex.store(10, std::memory_order_release);  // far behind the producer's window
    Consumer consumer(*ring);

    const ce::InjectFrameRingResync resync = consumer.cursor.ResyncIfOverrun(write);
    ASSERT_TRUE(resync.faulted);
    EXPECT_EQ(consumer.cursor.Next(), write - 1);
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), write - 1);
    EXPECT_EQ(consumer.cursor.NextSlot().valid.load(std::memory_order_acquire), 1u);
    EXPECT_TRUE(PassesRecordingStartRingCheck(*ring));

    ce::InjectFrameRingLease newest = consumer.cursor.Take();
    newest.Reset();
    EXPECT_EQ(ring->readIndex.load(std::memory_order_acquire), write);
}
