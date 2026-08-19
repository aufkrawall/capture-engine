#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/overlay_submit_queue_policy.h"
#include "source_fragment_reader.h"

// Regression coverage for the DOOM Eternal overlay disappearing after ~240
// frames, session installed/captureengine/logs/20260819_030710.
//
// idTech 7 presents from queue family 2 - compute + transfer, no graphics -
// from five different threads once the real render loop starts. The overlay is
// a render pass, so the layer bailed with "Overlay skipped on non-graphics
// present queue family 2" and never drew again. `perf_metrics_8696.csv` shows
// overlay_us dropping to 0-1 us from frame 240 onward: the early-out, every
// frame, for the rest of the run.

namespace {

using ce::overlay_submit_queue_policy::CanReserveOverlayQueue;
using ce::overlay_submit_queue_policy::CanWidenQueueCreateEntry;
using ce::overlay_submit_queue_policy::ChooseOverlaySubmitQueue;
using ce::overlay_submit_queue_policy::OverlaySubmitQueue;
using ce::overlay_submit_queue_policy::ReservedOverlayQueueIndex;
using ce::overlay_submit_queue_policy::ReservedOverlayQueuePriority;
using ce::overlay_submit_queue_policy::ShouldSerializeSubmissionsOnQueue;

std::string ReadProjectSource(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / relativePath;
    if (!fs::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

}  // namespace

TEST(OverlaySubmitQueuePolicyTest, GraphicsPresentQueueKeepsTheExistingPath) {
    // Every title that works today must keep submitting exactly where it did.
    EXPECT_EQ(ChooseOverlaySubmitQueue(/*presentQueueSupportsGraphics=*/true, /*reservedQueueAvailable=*/true,
                                       /*borrowedQueueAvailable=*/true),
              OverlaySubmitQueue::kPresentQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(/*presentQueueSupportsGraphics=*/true, /*reservedQueueAvailable=*/false,
                                       /*borrowedQueueAvailable=*/false),
              OverlaySubmitQueue::kPresentQueue);
}

TEST(OverlaySubmitQueuePolicyTest, ComputeOnlyPresentQueueUsesTheReservedQueue) {
    // The DOOM Eternal case: present family has no graphics bit, CE reserved a
    // queue of its own at device creation.
    EXPECT_EQ(ChooseOverlaySubmitQueue(/*presentQueueSupportsGraphics=*/false, /*reservedQueueAvailable=*/true,
                                       /*borrowedQueueAvailable=*/true),
              OverlaySubmitQueue::kReservedQueue)
        << "a queue CE owns outright must win over borrowing the game's";
}

TEST(OverlaySubmitQueuePolicyTest, SingleGraphicsQueueHardwareBorrows) {
    // AMD exposes one graphics queue, so a game that creates it leaves nothing
    // to reserve. Borrowing under CE's submission lock is what keeps the
    // overlay alive there instead of silently disappearing.
    EXPECT_EQ(ChooseOverlaySubmitQueue(/*presentQueueSupportsGraphics=*/false, /*reservedQueueAvailable=*/false,
                                       /*borrowedQueueAvailable=*/true),
              OverlaySubmitQueue::kBorrowedQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(/*presentQueueSupportsGraphics=*/false, /*reservedQueueAvailable=*/false,
                                       /*borrowedQueueAvailable=*/false),
              OverlaySubmitQueue::kNone);
}

TEST(OverlaySubmitQueuePolicyTest, ReservationNeverExceedsWhatTheFamilyExposes) {
    // Asking for more queues than a family has fails vkCreateDevice, which would
    // take the game down with it.
    EXPECT_TRUE(CanReserveOverlayQueue(/*familyQueueCount=*/16, /*gameRequestedQueueCount=*/1));
    EXPECT_TRUE(CanReserveOverlayQueue(/*familyQueueCount=*/2, /*gameRequestedQueueCount=*/1));
    EXPECT_FALSE(CanReserveOverlayQueue(/*familyQueueCount=*/1, /*gameRequestedQueueCount=*/1))
        << "AMD's single graphics queue leaves nothing to reserve";
    EXPECT_FALSE(CanReserveOverlayQueue(/*familyQueueCount=*/4, /*gameRequestedQueueCount=*/4));
}

TEST(OverlaySubmitQueuePolicyTest, ReservedIndexIsPastEveryQueueTheGameCreated) {
    EXPECT_EQ(ReservedOverlayQueueIndex(1), 1u);
    EXPECT_EQ(ReservedOverlayQueueIndex(4), 4u);
}

TEST(OverlaySubmitQueuePolicyTest, ProtectedEntriesAreNotWidened) {
    // Protection is a property of every queue in a VkDeviceQueueCreateInfo, and
    // the overlay is not protected content.
    EXPECT_TRUE(CanWidenQueueCreateEntry(/*entryIsProtected=*/false));
    EXPECT_FALSE(CanWidenQueueCreateEntry(/*entryIsProtected=*/true));
}

TEST(OverlaySubmitQueuePolicyTest, ReservedQueueNeverOutranksTheGame) {
    const float gamePriorities[] = {0.25f, 0.5f, 0.5f};
    EXPECT_FLOAT_EQ(ReservedOverlayQueuePriority(gamePriorities, 3), 0.5f);
    const float singleLow[] = {0.0f};
    EXPECT_FLOAT_EQ(ReservedOverlayQueuePriority(singleLow, 1), 0.0f);
    EXPECT_FLOAT_EQ(ReservedOverlayQueuePriority(nullptr, 0), 1.0f);
    EXPECT_FLOAT_EQ(ReservedOverlayQueuePriority(gamePriorities, 0), 1.0f);
}

TEST(OverlaySubmitQueuePolicyTest, OnlyTheBorrowedQueuePaysForSerialization) {
    // The lock is on the game's hot submit path, so it must be inert unless CE
    // is really sharing that exact queue.
    EXPECT_FALSE(ShouldSerializeSubmissionsOnQueue(/*borrowActive=*/false, /*queueIsBorrowedQueue=*/false));
    EXPECT_FALSE(ShouldSerializeSubmissionsOnQueue(/*borrowActive=*/false, /*queueIsBorrowedQueue=*/true));
    EXPECT_FALSE(ShouldSerializeSubmissionsOnQueue(/*borrowActive=*/true, /*queueIsBorrowedQueue=*/false));
    EXPECT_TRUE(ShouldSerializeSubmissionsOnQueue(/*borrowActive=*/true, /*queueIsBorrowedQueue=*/true));
}

TEST(OverlaySubmitQueuePolicySourceTest, OverlaySubmitsOnTheResolvedQueueNotThePresentQueue) {
    const std::string overlay = ReadProjectSource("hook/vulkan_layer/layer_overlay_render.cpp");
    ASSERT_FALSE(overlay.empty());
    EXPECT_NE(overlay.find("ResolveOverlaySubmitTarget"), std::string::npos);
    EXPECT_NE(overlay.find("fp_vkQueueSubmit(submitQueue"), std::string::npos)
        << "the overlay submit must go to the resolved graphics queue";
    EXPECT_EQ(overlay.find("fp_vkQueueSubmit(queue"), std::string::npos)
        << "submitting to the present queue is what broke on a compute-only present family";
}

TEST(OverlaySubmitQueuePolicySourceTest, BorrowedQueueSubmissionsAreSerialized) {
    const std::string hooks = ReadProjectSource("hook/vulkan_layer/layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    // Every whitelisted down-call in all three submit wrappers must be covered:
    // VkQueue is externally synchronized, and a partially guarded wrapper is a
    // race that only shows up under load.
    size_t guardCount = 0;
    for (size_t pos = hooks.find("ScopedBorrowedQueueSubmission submissionGuard");
         pos != std::string::npos;
         pos = hooks.find("ScopedBorrowedQueueSubmission submissionGuard", pos + 1)) {
        ++guardCount;
    }
    EXPECT_EQ(guardCount, 6u) << "two paths (TLS-cached and cold) in each of vkQueueSubmit, vkQueueSubmit2 and "
                                 "vkQueueSubmit2KHR";

    const std::string queueOwner = ReadProjectSource("hook/vulkan_layer/layer_overlay_queue.cpp");
    ASSERT_FALSE(queueOwner.empty());
    EXPECT_NE(queueOwner.find("SetBorrowedOverlaySubmitQueue(borrowed)"), std::string::npos)
        << "the borrow must be published before the first borrowed submit, not after";
}

TEST(OverlaySubmitQueuePolicySourceTest, DeviceCreateFallsBackWhenTheReservationIsRejected) {
    const std::string hooks = ReadProjectSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    const size_t reservation = hooks.find("BuildOverlayQueueReservation(");
    ASSERT_NE(reservation, std::string::npos);
    EXPECT_NE(hooks.find("retrying with", reservation), std::string::npos)
        << "CE's extra queue must never be the reason a game fails to create its device";
}
