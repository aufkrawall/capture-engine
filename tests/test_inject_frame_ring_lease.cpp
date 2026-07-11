#include <gtest/gtest.h>

#include <memory>

#include "../common/inject_frame_ring_lease.h"

namespace {

void PublishTestSlot(FrameRingBuffer& ring, uint32_t index, int32_t textureIndex) {
    FrameSlot& slot = ring.slots[index % FRAME_RING_SIZE];
    slot.textureIndex = textureIndex;
    slot.frameIndex = index;
    slot.valid.store(1, std::memory_order_release);
    ring.writeIndex.store(index + 1, std::memory_order_release);
}

}  // namespace

TEST(InjectFrameRingLeaseTest, OutOfOrderCompletionPublishesOnlyContiguousPrefix) {
    FrameRingBuffer ring;
    PublishTestSlot(ring, 0, 0);
    PublishTestSlot(ring, 1, 1);
    PublishTestSlot(ring, 2, 2);
    auto state = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);

    auto first = state->Acquire(0);
    auto second = state->Acquire(1);
    auto third = state->Acquire(2);

    second.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(ring.slots[1].valid.load(std::memory_order_acquire), 0u);

    first.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 2u);

    third.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 3u);
}

TEST(InjectFrameRingLeaseTest, MovedLeaseCompletesExactlyOnce) {
    FrameRingBuffer ring;
    PublishTestSlot(ring, 0, 3);
    auto state = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);

    auto original = state->Acquire(0);
    ce::InjectFrameRingLease moved = std::move(original);
    EXPECT_FALSE(static_cast<bool>(original));
    EXPECT_TRUE(static_cast<bool>(moved));

    moved.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 1u);
    moved.Reset();
    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 1u);
}

TEST(InjectFrameRingLeaseTest, DetachedStateMakesLateDestructionSafe) {
    FrameRingBuffer ring;
    PublishTestSlot(ring, 0, 4);
    auto state = std::make_shared<ce::InjectFrameRingLeaseState>(&ring);
    auto lease = state->Acquire(0);

    state->Detach();
    lease.Reset();

    EXPECT_EQ(ring.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(ring.slots[0].valid.load(std::memory_order_acquire), 1u);
}
