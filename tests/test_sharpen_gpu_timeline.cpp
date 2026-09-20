#include <gtest/gtest.h>

#include <cstdint>

#include "../hook/common/sharpen_gpu_timeline.h"

// Regressions for the GPU-completion rules the sharpen passes run on. Each case
// here failed before the fix and describes a symptom that was observed or is
// directly reachable from a shipped configuration; none of them needs a device.

using ce::sharpen::CompletedPast;
using ce::sharpen::kCompletedValueDeviceRemoved;
using ce::sharpen::kSlotValueFree;
using ce::sharpen::kSlotValueUnprovable;
using ce::sharpen::QueueChangeNeedsOrdering;
using ce::sharpen::SelectFreeSlot;
using ce::sharpen::SlotIsBusyAfter;
using ce::sharpen::SlotValueAfter;
using ce::sharpen::SubmissionOutcome;
using ce::sharpen::TimelineIsIdle;

namespace {

// Two distinct addresses standing in for two command queues. Only identity
// matters; nothing dereferences them.
int g_GameQueue = 0;
int g_StreamlineQueue = 0;
const void* const kGameQueue = &g_GameQueue;
const void* const kStreamlineQueue = &g_StreamlineQueue;

}  // namespace

TEST(SharpenGpuTimelineTest, CompletedPastTreatsAFreeSlotAsFinished) {
    EXPECT_TRUE(CompletedPast(0, kSlotValueFree));
    EXPECT_TRUE(CompletedPast(5, 5));
    EXPECT_TRUE(CompletedPast(6, 5));
    EXPECT_FALSE(CompletedPast(4, 5));
}

TEST(SharpenGpuTimelineTest, ARemovedDeviceHasFinishedEverything) {
    // GetCompletedValue() reports UINT64_MAX for a removed device. Nothing is
    // executing on it, so every deferred release must be allowed to happen
    // rather than being held forever.
    EXPECT_TRUE(CompletedPast(kCompletedValueDeviceRemoved, 1));
    EXPECT_TRUE(CompletedPast(kCompletedValueDeviceRemoved, 1'000'000));
    EXPECT_TRUE(TimelineIsIdle(kCompletedValueDeviceRemoved, 42));
}

TEST(SharpenGpuTimelineTest, AnUnprovableSlotIsNeverRecycled) {
    // Signalling the fence failed, so the work is queued with no way to observe
    // that it finished. Recycling that allocator would Reset() it while the GPU
    // may still be reading it.
    EXPECT_EQ(SlotValueAfter(SubmissionOutcome::kUnprovable, 7), kSlotValueUnprovable);
    EXPECT_FALSE(CompletedPast(kSlotValueUnprovable - 1, kSlotValueUnprovable));
}

TEST(SharpenGpuTimelineTest, WorkThatWasNeverQueuedFreesItsSlot) {
    // The Vulkan regression: vkResetFences succeeded and then vkQueueSubmit
    // failed, so the slot's fence was left unsignalled with nothing in flight to
    // ever signal it. Marking the slot busy there retired it permanently, and
    // kSharpenSlotCount such failures retired the whole ring - sharpening then
    // stopped for the session with only the "every command buffer is still in
    // flight" log to show for it.
    EXPECT_EQ(SlotValueAfter(SubmissionOutcome::kNotQueued, 7), kSlotValueFree);
    EXPECT_FALSE(SlotIsBusyAfter(SubmissionOutcome::kNotQueued));
    EXPECT_TRUE(SlotIsBusyAfter(SubmissionOutcome::kSubmitted));
    EXPECT_TRUE(SlotIsBusyAfter(SubmissionOutcome::kUnprovable));
}

TEST(SharpenGpuTimelineTest, ARingOfFailedSubmitsStaysUsable) {
    constexpr uint32_t kSlots = 3;
    uint64_t slots[kSlots] = {kSlotValueFree, kSlotValueFree, kSlotValueFree};
    uint64_t completed = 0;
    uint64_t nextValue = 0;

    // Every submission fails to reach the queue. The ring must still hand out a
    // slot on the very next frame, indefinitely.
    for (int frame = 0; frame < 64; ++frame) {
        const int slot = SelectFreeSlot(slots, kSlots, completed);
        ASSERT_GE(slot, 0) << "ring drained after " << frame << " failed submissions";
        ++nextValue;
        slots[static_cast<uint32_t>(slot)] = SlotValueAfter(SubmissionOutcome::kNotQueued, nextValue);
    }
}

TEST(SharpenGpuTimelineTest, SelectFreeSlotRefusesWhenEverySlotIsInFlight) {
    constexpr uint32_t kSlots = 3;
    uint64_t slots[kSlots] = {10, 11, 12};
    EXPECT_EQ(SelectFreeSlot(slots, kSlots, 9), -1);
    EXPECT_EQ(SelectFreeSlot(slots, kSlots, 10), 0);
    EXPECT_EQ(SelectFreeSlot(slots, kSlots, 11), 0);
    EXPECT_EQ(SelectFreeSlot(nullptr, kSlots, 99), -1);
}

TEST(SharpenGpuTimelineTest, TimelineIsIdleOnlyOnceEverySubmissionHasCompleted) {
    // This gates rebuilding the one shader-visible SRV descriptor, which - unlike
    // a resource - cannot be kept alive until the GPU catches up.
    EXPECT_TRUE(TimelineIsIdle(0, 0)) << "nothing submitted yet";
    EXPECT_FALSE(TimelineIsIdle(4, 5));
    EXPECT_TRUE(TimelineIsIdle(5, 5));
    EXPECT_TRUE(TimelineIsIdle(6, 5));
}

TEST(SharpenGpuTimelineTest, AQueueChangeMustBeOrderedBeforeTheTimelineIsTrusted) {
    // The D3D12 regression. The pass is driven from two call sites that submit on
    // different queues, and a DLSS-G activation switches between them within one
    // swapchain generation. One fence signalled from two queues yields completion
    // values that are not ordered against each other, so the allocator recorded
    // for the older queue's value looks retired and gets Reset() under the GPU.
    EXPECT_TRUE(QueueChangeNeedsOrdering(kGameQueue, kStreamlineQueue, 5));
    EXPECT_TRUE(QueueChangeNeedsOrdering(kStreamlineQueue, kGameQueue, 5));
}

TEST(SharpenGpuTimelineTest, TheSameQueueNeedsNoOrdering) {
    // The ordering wait must be issued once per switch, never once per frame.
    EXPECT_FALSE(QueueChangeNeedsOrdering(kGameQueue, kGameQueue, 5));
    EXPECT_FALSE(QueueChangeNeedsOrdering(nullptr, nullptr, 5));
}

TEST(SharpenGpuTimelineTest, TheFirstSubmissionHasNothingToOrderAgainst) {
    EXPECT_FALSE(QueueChangeNeedsOrdering(nullptr, kGameQueue, 0));
    EXPECT_FALSE(QueueChangeNeedsOrdering(kGameQueue, kStreamlineQueue, 0));
    EXPECT_FALSE(QueueChangeNeedsOrdering(kGameQueue, nullptr, 5));
}

TEST(SharpenGpuTimelineTest, AnOrderedSwitchKeepsTheRingCorrectAcrossQueues) {
    // With the switch ordered, the new queue cannot execute before the old
    // queue's last signal, so completion values stay a single sorted timeline and
    // a slot is only ever handed back after the work really finished.
    constexpr uint32_t kSlots = 2;
    uint64_t slots[kSlots] = {kSlotValueFree, kSlotValueFree};

    // Frame 1 on the game's queue.
    int slot = SelectFreeSlot(slots, kSlots, 0);
    ASSERT_EQ(slot, 0);
    slots[0] = SlotValueAfter(SubmissionOutcome::kSubmitted, 1);

    // Frame 2 switches to Streamline's queue. It is chained behind value 1, so
    // its own value 2 cannot complete before value 1 does.
    ASSERT_TRUE(QueueChangeNeedsOrdering(kGameQueue, kStreamlineQueue, 1));
    slot = SelectFreeSlot(slots, kSlots, 0);
    ASSERT_EQ(slot, 1) << "slot 0 is still in flight and must not be reused";
    slots[1] = SlotValueAfter(SubmissionOutcome::kSubmitted, 2);

    // Nothing has completed: both slots are busy, so the frame is skipped rather
    // than recorded into an allocator the GPU still owns.
    EXPECT_EQ(SelectFreeSlot(slots, kSlots, 0), -1);

    // Value 2 completing implies value 1 completed, because the chain ordered
    // them. Both slots come back.
    EXPECT_EQ(SelectFreeSlot(slots, kSlots, 2), 0);
}
