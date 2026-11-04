#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <random>
#include <set>

#include "hook/d3d12/dx12_overlay_policy/upload_slot_guard.h"

// Which slot the D3D12 overlay's next draw takes. The one thing that must never happen is a slot read as finished
// while the GPU still executes its last submission: the simulation at the end checks that against a model of the queue.
namespace {

using ce::dx12_overlay_policy::kAllocatorCoupledUploadSlotCount;
using ce::dx12_overlay_policy::OverlaySlotPicker;
using ce::dx12_overlay_policy::ShouldReuseFinishedOverlaySlot;

constexpr int kSlots = kAllocatorCoupledUploadSlotCount;

// The queue: a slot's work, then the Signal if the route issues one.
class GpuModel {
public:
    void Submit(int slot, uint64_t signalValue) {
        busy_[static_cast<size_t>(slot)] = true;
        queue_.push_back({slot, signalValue});
    }
    void Execute(int operations) {
        for (int i = 0; i < operations && !queue_.empty(); ++i) {
            const Operation done = queue_.front();
            queue_.pop_front();
            busy_[static_cast<size_t>(done.slot)] = false;
            for (const Operation& later : queue_) {
                if (later.slot == done.slot) {
                    busy_[static_cast<size_t>(done.slot)] = true;
                }
            }
            if (done.signalValue != 0) {
                completed_ = done.signalValue;
            }
        }
    }
    void Drain() { Execute(static_cast<int>(queue_.size())); }
    bool Busy(int slot) const { return busy_[static_cast<size_t>(slot)]; }
    uint64_t Completed() const { return completed_; }

private:
    struct Operation {
        int slot;
        uint64_t signalValue;
    };
    std::deque<Operation> queue_;
    std::array<bool, kSlots> busy_{};
    uint64_t completed_ = 0;
};

}  // namespace

TEST(OverlaySlotPickerTest, AFreshPoolStartsAtTheLowestSlot) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 0), 0);
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 5, 0), 5);
}

TEST(OverlaySlotPickerTest, AFinishedSlotIsTakenAgainInsteadOfRotating) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    picker.NoteSubmitted(0, 0);
    values[0] = 1;
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 0), 1) << "still in flight";
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 1), 0) << "finished: the same slot again";
}

TEST(OverlaySlotPickerTest, ASubmissionWithoutASignalIsNotProvedByAnOlderValue) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    picker.NoteSubmitted(0, 0);
    values[0] = 1;
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 1), 0);

    // Resubmitted with no Signal afterwards: the old value 1 says nothing about this work.
    picker.NoteSubmitted(0, 1);
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 1), 1);
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 1000), 1);

    // A later Signal covers it again.
    values[0] = 2;
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 1), 1) << "signaled, not finished";
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 2), 0);
}

TEST(OverlaySlotPickerTest, ARouteWithoutSignalsEndsInPlainRotation) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    int ring = 0;
    for (int frame = 0; frame < kSlots; ++frame) {
        const int picked = picker.Pick(values.data(), kSlots, ring, 0);
        ASSERT_EQ(picked, ring) << "unused slots first, in ring order";
        picker.NoteSubmitted(picked, 0);
        ring = (picked + 1) % kSlots;
    }
    EXPECT_EQ(picker.Pick(values.data(), kSlots, ring, 0), -1) << "nothing proved: the caller rotates, as it always did";
}

TEST(OverlaySlotPickerTest, ASmallerPoolWrapsAtItsOwnSize) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    picker.NoteSubmitted(1, 0);
    values[1] = 1;
    picker.NoteSubmitted(2, 0);
    values[2] = 1;
    EXPECT_EQ(picker.Pick(values.data(), 3, 1, 0), 0);
    EXPECT_EQ(picker.Pick(values.data(), 3, 1, 1), 1);
}

TEST(OverlaySlotPickerTest, ResetForgetsAReplacedFenceAndItsValues) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    picker.NoteSubmitted(0, 500);
    values[0] = 501;
    picker.Reset();
    values = {};
    EXPECT_EQ(picker.Pick(values.data(), kSlots, 0, 0), 0);
}

TEST(OverlaySlotPickerTest, BadInputPicksNothing) {
    OverlaySlotPicker picker;
    std::array<uint64_t, kSlots> values{};
    EXPECT_EQ(picker.Pick(nullptr, kSlots, 0, 0), -1);
    EXPECT_EQ(picker.Pick(values.data(), 0, 0, 0), -1);
    EXPECT_EQ(picker.Pick(values.data(), kSlots + 1, 0, 0), -1);
    EXPECT_EQ(picker.Pick(values.data(), kSlots, -1, 0), -1);
}

TEST(OverlaySlotPickerTest, OnlyThePlainRouteTakesTheLowestSlot) {
    EXPECT_TRUE(ShouldReuseFinishedOverlaySlot(true, false, false, false));
    EXPECT_FALSE(ShouldReuseFinishedOverlaySlot(false, false, false, false)) << "no fence, no proof";
    EXPECT_FALSE(ShouldReuseFinishedOverlaySlot(true, true, false, false)) << "frame generation";
    EXPECT_FALSE(ShouldReuseFinishedOverlaySlot(true, false, true, false)) << "runtime-owned swapchain";
    EXPECT_FALSE(ShouldReuseFinishedOverlaySlot(true, false, false, true)) << "a Signal failed";
}

TEST(OverlaySlotPickerTest, AGpuAFrameBehindKeepsOnlyAFewSlotsInUse) {
    OverlaySlotPicker picker;
    GpuModel gpu;
    std::array<uint64_t, kSlots> values{};
    uint64_t current = 0;
    std::set<int> used;
    for (int frame = 0; frame < 2000; ++frame) {
        gpu.Execute(4);  // one frame's work and Signal, plus the one before
        const int slot = picker.Pick(values.data(), kSlots, 0, gpu.Completed());
        ASSERT_GE(slot, 0);
        used.insert(slot);
        picker.NoteSubmitted(slot, current);
        values[static_cast<size_t>(slot)] = ++current;
        gpu.Submit(slot, current);
    }
    EXPECT_LE(used.size(), 3u) << "each slot in use rewrites the text once per text change";
}

// Random routes and random GPU progress: Signal after every draw, no Signal at all, the GPU stalling or catching up.
// Mutating IsFinished to accept any completed value fails this at once: a no-Signal slot reads as finished while busy.
TEST(OverlaySlotPickerTest, ASlotReadAsFinishedIsNeverStillOnTheGpu) {
    std::mt19937 random(12345);
    for (int run = 0; run < 20; ++run) {
        OverlaySlotPicker picker;
        GpuModel gpu;
        std::array<uint64_t, kSlots> values{};
        uint64_t current = 0;
        int ring = 0;
        bool signals = true;
        bool plainRoute = true;
        for (int frame = 0; frame < 5000; ++frame) {
            if (random() % 50 == 0) {
                signals = !signals;
            }
            if (random() % 200 == 0) {
                plainRoute = !plainRoute;
            }
            gpu.Execute(static_cast<int>(random() % 5));
            const int picked = picker.Pick(values.data(), kSlots, plainRoute ? 0 : ring, gpu.Completed());
            if (picked >= 0) {
                ASSERT_FALSE(gpu.Busy(picked)) << "run=" << run << " frame=" << frame << " slot=" << picked;
            } else {
                // The caller rotates here, which is what a slot without a proof always relied on; drain so the
                // model's rotation stays as safe as 16 frames of distance are in a real queue.
                gpu.Drain();
            }
            const int slot = picked >= 0 ? picked : ring;
            ring = (slot + 1) % kSlots;
            picker.NoteSubmitted(slot, current);
            if (signals) {
                values[static_cast<size_t>(slot)] = ++current;
                gpu.Submit(slot, current);
            } else {
                gpu.Submit(slot, 0);
            }
        }
    }
}
