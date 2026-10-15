#include <gtest/gtest.h>

#include <memory>

#include "common/capture/capture_base.h"
#include "common/capture/inject_source_read_release.h"

namespace {

// Stands in for the encoder-device fence: the test decides when the GPU read
// finished and observes the event registration.
struct FakeReadState {
    bool completed = false;
    bool armSucceeds = true;
    HANDLE armedEvent = nullptr;
    int armCalls = 0;
};

class FakeSourceReadToken {
public:
    explicit FakeSourceReadToken(std::shared_ptr<FakeReadState> state) : state_(std::move(state)) {}
    bool Completed() const { return state_->completed; }
    bool ArmEvent(HANDLE event) const {
        ++state_->armCalls;
        state_->armedEvent = event;
        return state_->armSucceeds;
    }

private:
    std::shared_ptr<FakeReadState> state_;
};

using Queue = ce::SourceReadReleaseQueue<FakeSourceReadToken>;

void PublishTestSlot(SharedMemoryLayout& sharedMem, uint32_t index, int32_t textureIndex) {
    FrameSlot& slot = sharedMem.frameRing.slots[index % FRAME_RING_SIZE];
    slot.textureIndex = textureIndex;
    slot.frameIndex = index;
    slot.valid.store(1, std::memory_order_release);
    sharedMem.frameRing.writeIndex.store(index + 1, std::memory_order_release);
}

struct RingFixture {
    std::unique_ptr<SharedMemoryLayout> sharedMem = std::make_unique<SharedMemoryLayout>();
    std::shared_ptr<ce::InjectFrameRingLeaseState> leases =
        std::make_shared<ce::InjectFrameRingLeaseState>(&sharedMem->frameRing);
};

}  // namespace

// The regression: an accepted encode released the slot while its GPU read was
// still queued, so the producer overwrote it and the frame showed newer pixels.
TEST(InjectSourceReadReleaseTest, SlotStaysOutstandingUntilEncoderReadCompletes) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 5);
    auto read = std::make_shared<FakeReadState>();
    Queue queue;

    ce::InjectFrameRingLease lease = ring.leases->Acquire(0);
    queue.Defer(std::move(lease), FakeSourceReadToken(read), 100);

    EXPECT_FALSE(static_cast<bool>(lease));  // NOLINT(bugprone-use-after-move): the queue owns it now.
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 5));
    EXPECT_EQ(queue.ReleaseCompleted(200), 0u);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 5));
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 0u);

    read->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(350), 1u);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 5));
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(queue.Pending(), 0u);

    const ce::SourceReadReleaseWindow window = queue.TakeWindow();
    EXPECT_EQ(window.deferred, 1u);
    EXPECT_EQ(window.released, 1u);
    EXPECT_EQ(window.maxPending, 1u);
    EXPECT_EQ(window.maxHoldQpc, 250);
}

TEST(InjectSourceReadReleaseTest, CompletedReadReleasesImmediatelyWithoutArming) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 2);
    auto read = std::make_shared<FakeReadState>();
    read->completed = true;
    Queue queue;

    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(read), 10);

    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 2));
    EXPECT_EQ(read->armCalls, 0);
    EXPECT_EQ(queue.Pending(), 0u);
    const ce::SourceReadReleaseWindow window = queue.TakeWindow();
    EXPECT_EQ(window.immediate, 1u);
    EXPECT_EQ(window.deferred, 0u);
}

TEST(InjectSourceReadReleaseTest, ArmedEventWakesOwnerThatThenReleases) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 7);
    auto read = std::make_shared<FakeReadState>();
    Queue queue;
    ASSERT_NE(queue.CompletionEvent(), nullptr);

    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(read), 1);
    ASSERT_EQ(read->armCalls, 1);
    EXPECT_EQ(read->armedEvent, queue.CompletionEvent());
    EXPECT_EQ(WaitForSingleObject(queue.CompletionEvent(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

    // What the fence does on completion.
    read->completed = true;
    ASSERT_TRUE(SetEvent(read->armedEvent));
    EXPECT_EQ(WaitForSingleObject(queue.CompletionEvent(), 0), static_cast<DWORD>(WAIT_OBJECT_0));
    EXPECT_EQ(queue.ReleaseCompleted(2), 1u);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 7));
}

TEST(InjectSourceReadReleaseTest, ArmFailureIsCountedAndPollingStillReleases) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 1);
    auto read = std::make_shared<FakeReadState>();
    read->armSucceeds = false;
    Queue queue;

    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(read), 1);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 1));
    read->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(2), 1u);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 1));
    EXPECT_EQ(queue.TakeWindow().armFailures, 1u);
}

// A device change starts a new fence, so a later read can finish first; only
// the frames whose reads finished may go back, and readIndex only advances
// over the contiguous completed prefix.
TEST(InjectSourceReadReleaseTest, OutOfOrderCompletionReleasesOnlyFinishedReads) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 3);
    PublishTestSlot(*ring.sharedMem, 1, 4);
    auto firstRead = std::make_shared<FakeReadState>();
    auto secondRead = std::make_shared<FakeReadState>();
    Queue queue;

    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(firstRead), 1);
    queue.Defer(ring.leases->Acquire(1), FakeSourceReadToken(secondRead), 1);
    EXPECT_EQ(queue.TakeWindow().maxPending, 2u);

    secondRead->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(2), 1u);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 3));
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 4));
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 0u);

    firstRead->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(3), 1u);
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 2u);
}

TEST(InjectSourceReadReleaseTest, TeardownTimeoutPreservesOutstandingReads) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 8);
    PublishTestSlot(*ring.sharedMem, 1, 9);
    Queue queue;

    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(std::make_shared<FakeReadState>()), 1);
    queue.Defer(ring.leases->Acquire(1), FakeSourceReadToken(std::make_shared<FakeReadState>()), 1);
    EXPECT_FALSE(queue.Drain(0));
    EXPECT_EQ(queue.Pending(), 2u);
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(queue.Totals().released, 0u);
}

TEST(InjectSourceReadReleaseTest, TeardownDrainsCompletedReadsWithoutWaiting) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 8);
    Queue queue;
    auto read = std::make_shared<FakeReadState>();
    queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(read), 1);
    read->completed = true;
    EXPECT_TRUE(queue.Drain(0));
    EXPECT_EQ(queue.Pending(), 0u);
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 1u);
}

TEST(InjectSourceReadReleaseTest, DestructorDoesNotPublishIncompleteRead) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 8);
    {
        Queue queue;
        queue.Defer(ring.leases->Acquire(0), FakeSourceReadToken(std::make_shared<FakeReadState>()), 1);
    }
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 8));
    EXPECT_EQ(ring.sharedMem->frameRing.readIndex.load(std::memory_order_acquire), 0u);
}

TEST(InjectSourceReadReleaseTest, CancelledRetryStillOwnsSlotUntilItsReadCompletes) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 6);
    auto read = std::make_shared<FakeReadState>();
    Queue queue;
    auto retryLease = ring.leases->Acquire(0);
    queue.Defer(retryLease.Share(), FakeSourceReadToken(read), 1);
    retryLease.Reset();
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
    read->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(2), 1u);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
}

TEST(InjectSourceReadReleaseTest, CompletedReadDoesNotReleaseSlotStillOwnedByRetry) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 6);
    auto read = std::make_shared<FakeReadState>();
    Queue queue;
    auto retryLease = ring.leases->Acquire(0);
    queue.Defer(retryLease.Share(), FakeSourceReadToken(read), 1);
    read->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(2), 1u);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
    retryLease.Reset();
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
}

TEST(InjectSourceReadReleaseTest, MultipleRetryReadsRequireEveryOwnerToComplete) {
    RingFixture ring;
    PublishTestSlot(*ring.sharedMem, 0, 5);
    PublishTestSlot(*ring.sharedMem, 1, 6);
    auto first = std::make_shared<FakeReadState>();
    auto second = std::make_shared<FakeReadState>();
    Queue queue;
    auto retryLease = ring.leases->Acquire(1);
    queue.Defer(retryLease.Share(), FakeSourceReadToken(first), 1);
    queue.Defer(retryLease.Share(), FakeSourceReadToken(second), 2);
    EXPECT_EQ(retryLease.RingIndex(), 1u);
    retryLease.Reset();
    first->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(3), 1u);
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
    second->completed = true;
    EXPECT_EQ(queue.ReleaseCompleted(4), 1u);
    EXPECT_FALSE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 6));
    EXPECT_TRUE(IsCaptureTextureSlotOutstanding(ring.sharedMem.get(), 5));
}

TEST(InjectSourceReadReleaseTest, EmptyLeaseIsIgnored) {
    Queue queue;
    auto read = std::make_shared<FakeReadState>();
    queue.Defer(ce::InjectFrameRingLease{}, FakeSourceReadToken(read), 1);
    EXPECT_EQ(queue.Pending(), 0u);
    EXPECT_EQ(read->armCalls, 0);
    const ce::SourceReadReleaseWindow window = queue.TakeWindow();
    EXPECT_EQ(window.deferred + window.immediate, 0u);
}
