// Audit 4, item 4: a failed non-device-lost overlay submit on the direct render-pass
// route left the slot's fence reset and unsignalled while the slot was marked used.
// Later presents could exhaust the ring and then block the game's present thread in
// vkWaitForFences(UINT64_MAX) on a fence nothing would ever signal. The compute route
// re-armed its fence, but when that re-arm failed too it only logged.
//
// The ring is driven here through a mock dispatch: a fence model with the same
// signalled/reset/submit semantics the layer relies on, and the exact slot policy the
// layer uses (ChooseSubmissionSlotAvoidingStranded + ResolveFailedSubmitSlotFate +
// ClassifyBackpressureWait).

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "../hook/vulkan_layer/overlay_submit_queue_policy.h"
#include "source_fragment_reader.h"

namespace {

namespace policy = ce::overlay_submit_queue_policy;

// Minimal fence/queue model: a fence is signalled, or reset with or without work
// queued that will signal it. Waits never block; a wait on a fence that nothing
// will signal reports the bounded-wait timeout, which is exactly what the unbounded
// wait used to turn into a hang.
struct MockDispatch {
    enum class Fence { kSignalled, kPending, kReset };
    std::vector<Fence> fences;
    bool failNextSubmit = false;
    bool failNextRearm = false;
    int unboundedWaits = 0;

    int WaitForFence(uint32_t slot, uint64_t timeoutNs) {
        if (timeoutNs == UINT64_MAX && fences[slot] == Fence::kReset) {
            ++unboundedWaits;  // the old behaviour: this never returns on hardware
        }
        switch (fences[slot]) {
            case Fence::kSignalled:
                return 0;
            case Fence::kPending:
                fences[slot] = Fence::kSignalled;  // the GPU retires queued work
                return 0;
            case Fence::kReset:
                return 2;  // VK_TIMEOUT
        }
        return -1;
    }
    bool Submit(uint32_t slot) {
        if (failNextSubmit) {
            failNextSubmit = false;
            return false;  // the fence stays reset
        }
        fences[slot] = Fence::kPending;
        return true;
    }
    bool RearmWithEmptySubmit(uint32_t slot) {
        if (failNextRearm) {
            failNextRearm = false;
            return false;
        }
        fences[slot] = Fence::kPending;
        return true;
    }
};

struct MockRing {
    MockDispatch disp;
    std::vector<uint8_t> stranded;
    uint32_t next = 0;
    int skippedPresents = 0;

    explicit MockRing(uint32_t slots) {
        disp.fences.assign(slots, MockDispatch::Fence::kSignalled);
        stranded.assign(slots, 0);
    }

    // One present, following layer_overlay_render.cpp: choose, (bounded) wait, reset, submit,
    // and on a failed submit re-arm or strand.
    bool Present() {
        const uint32_t count = static_cast<uint32_t>(disp.fences.size());
        auto choice = policy::ChooseSubmissionSlotAvoidingStranded(
            count, next,
            [&](uint32_t slot) { return disp.fences[slot] == MockDispatch::Fence::kSignalled; },
            [&](uint32_t slot) { return stranded[slot] != 0; }, false);
        if (!choice.valid) {
            ++skippedPresents;
            return false;
        }
        if (choice.waitForCompletion) {
            const int result = disp.WaitForFence(choice.index, policy::kSubmissionSlotBackpressureWaitBoundNs);
            if (policy::ClassifyBackpressureWait(result) != policy::BackpressureWaitOutcome::kSlotRetired) {
                ++skippedPresents;
                return false;
            }
        }
        next = (choice.index + 1) % count;
        disp.fences[choice.index] = MockDispatch::Fence::kReset;
        if (!disp.Submit(choice.index)) {
            const bool reArmed = disp.RearmWithEmptySubmit(choice.index);
            if (policy::ResolveFailedSubmitSlotFate(reArmed) == policy::FailedSubmitSlotFate::kStranded) {
                stranded[choice.index] = 1;
            }
            return false;
        }
        return true;
    }
};

}  // namespace

TEST(OverlaySubmitFenceRecoveryTest, FailedSubmitIsReArmedAndTheSlotStaysUsable) {
    MockRing ring(3);
    ring.disp.failNextSubmit = true;
    EXPECT_FALSE(ring.Present());
    EXPECT_EQ(ring.stranded[0], 0);
    EXPECT_EQ(ring.disp.fences[0], MockDispatch::Fence::kPending);
    // Many more presents: every slot keeps cycling, nothing is ever skipped.
    for (int i = 0; i < 30; ++i) {
        EXPECT_TRUE(ring.Present());
    }
    EXPECT_EQ(ring.skippedPresents, 0);
    EXPECT_EQ(ring.disp.unboundedWaits, 0);
}

TEST(OverlaySubmitFenceRecoveryTest, UnrecoverableSlotIsStrandedAndNeverWaitedOn) {
    MockRing ring(2);
    ring.disp.failNextSubmit = true;
    ring.disp.failNextRearm = true;
    EXPECT_FALSE(ring.Present());
    EXPECT_EQ(ring.stranded[0], 1);
    EXPECT_EQ(ring.disp.fences[0], MockDispatch::Fence::kReset);
    // The ring is one slot shallower but keeps working on the healthy slot.
    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(ring.Present());
    }
    EXPECT_EQ(ring.skippedPresents, 0);
    EXPECT_EQ(ring.disp.unboundedWaits, 0);
}

TEST(OverlaySubmitFenceRecoveryTest, AllSlotsStrandedSkipsTheOverlayInsteadOfHanging) {
    MockRing ring(2);
    for (int i = 0; i < 2; ++i) {
        ring.disp.failNextSubmit = true;
        ring.disp.failNextRearm = true;
        EXPECT_FALSE(ring.Present());
    }
    EXPECT_EQ(ring.stranded[0], 1);
    EXPECT_EQ(ring.stranded[1], 1);
    EXPECT_FALSE(ring.Present());
    EXPECT_EQ(ring.skippedPresents, 1);
    EXPECT_EQ(ring.disp.unboundedWaits, 0);
}

TEST(OverlaySubmitFenceRecoveryTest, SlotChoiceSkipsStrandedSlotsForProbeGrowthAndBackpressure) {
    const auto never = [](uint32_t) { return false; };
    const auto strandedZero = [](uint32_t slot) { return slot == 0; };
    int probed0 = 0;
    const auto probe = [&](uint32_t slot) {
        if (slot == 0)
            ++probed0;
        return false;
    };
    // Backpressure never lands on the stranded slot, even when it is the round-robin cursor.
    auto choice = policy::ChooseSubmissionSlotAvoidingStranded(3, 0, probe, strandedZero, false);
    ASSERT_TRUE(choice.valid);
    EXPECT_TRUE(choice.waitForCompletion);
    EXPECT_EQ(choice.index, 1u);
    EXPECT_EQ(probed0, 0);
    // Growth is still preferred over any wait.
    choice = policy::ChooseSubmissionSlotAvoidingStranded(3, 0, never, strandedZero, true);
    EXPECT_TRUE(choice.growRing);
    EXPECT_EQ(choice.index, 3u);
    // Every slot stranded: no valid choice at all.
    choice = policy::ChooseSubmissionSlotAvoidingStranded(2, 0, never, [](uint32_t) { return true; }, false);
    EXPECT_FALSE(choice.valid);
    // The legacy entry point is unchanged.
    choice = policy::ChooseSubmissionSlot(4, 2, never, false);
    EXPECT_TRUE(choice.valid);
    EXPECT_TRUE(choice.waitForCompletion);
    EXPECT_EQ(choice.index, 2u);
}

TEST(OverlaySubmitFenceRecoveryTest, BackpressureWaitIsBoundedAndClassified) {
    EXPECT_EQ(policy::ClassifyBackpressureWait(0), policy::BackpressureWaitOutcome::kSlotRetired);
    EXPECT_EQ(policy::ClassifyBackpressureWait(2), policy::BackpressureWaitOutcome::kSkipOverlayThisPresent);
    EXPECT_EQ(policy::ClassifyBackpressureWait(-4), policy::BackpressureWaitOutcome::kDeviceLost);
    EXPECT_EQ(policy::ClassifyBackpressureWait(-1), policy::BackpressureWaitOutcome::kFailed);
    EXPECT_LT(policy::kSubmissionSlotBackpressureWaitBoundNs, UINT64_MAX);
    EXPECT_EQ(policy::ResolveFailedSubmitSlotFate(true), policy::FailedSubmitSlotFate::kRearmed);
    EXPECT_EQ(policy::ResolveFailedSubmitSlotFate(false), policy::FailedSubmitSlotFate::kStranded);
}

TEST(OverlaySubmitFenceRecoveryTest, LayerUsesTheRecoveryPolicyOnBothRoutes) {
    const std::filesystem::path root = std::filesystem::current_path() / "hook" / "vulkan_layer";
    const std::string render = ce::test_source::ReadFile(root / "layer_overlay_render.cpp");
    const std::string compute = ce::test_source::ReadFile(root / "layer_overlay_compute.cpp");
    ASSERT_FALSE(render.empty());
    ASSERT_FALSE(compute.empty());
    EXPECT_EQ(render.find("UINT64_MAX"), std::string::npos);
    EXPECT_NE(render.find("kSubmissionSlotBackpressureWaitBoundNs"), std::string::npos);
    EXPECT_NE(render.find("ChooseSubmissionSlotAvoidingStranded("), std::string::npos);
    // Direct route: a failed non-device-lost submit re-arms its fence or strands the slot.
    const size_t submitFailed = render.find("if (submitResult != VK_SUCCESS) {");
    ASSERT_NE(submitFailed, std::string::npos);
    const size_t rearm = render.find("fp_vkQueueSubmit(submitQueue, 0, nullptr, fence)", submitFailed);
    const size_t strand = render.find("MarkSubmissionSlotStranded(state, submissionSlot);", submitFailed);
    EXPECT_NE(rearm, std::string::npos);
    EXPECT_NE(strand, std::string::npos);
    // Compute route: a failed re-arm is recorded, not just logged.
    const size_t computeRearm = compute.find("fp_vkQueueSubmit(presentQueue, 0, nullptr, fence) != VK_SUCCESS");
    ASSERT_NE(computeRearm, std::string::npos);
    EXPECT_NE(compute.find("MarkSubmissionSlotStranded(state, submissionSlot);", computeRearm), std::string::npos);
}
