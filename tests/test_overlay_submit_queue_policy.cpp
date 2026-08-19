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
using ce::overlay_submit_queue_policy::SubmitQueueAvailability;

SubmitQueueAvailability Availability(bool presentGraphics, bool reserved, bool borrowed, bool concurrent) {
    SubmitQueueAvailability availability;
    availability.presentQueueSupportsGraphics = presentGraphics;
    availability.reservedQueueAvailable = reserved;
    availability.borrowedQueueAvailable = borrowed;
    availability.gameSubmitsConcurrently = concurrent;
    return availability;
}

std::string ReadProjectSource(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path source = fs::current_path() / relativePath;
    if (!fs::exists(source))
        return {};
    return ce::test_source::ReadFile(source);
}

}  // namespace

TEST(OverlaySubmitQueuePolicyTest, GraphicsPresentQueueKeepsTheExistingPath) {
    // Every title that works today must keep submitting exactly where it did -
    // and that queue is the game's own, which is why it never cost frame rate.
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(true, true, true, false)), OverlaySubmitQueue::kPresentQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(true, false, false, true)), OverlaySubmitQueue::kPresentQueue);
}

// DOOM Eternal session `20260819_140614`: "present from compute" on costs frame
// rate with CE injected and nothing without it. The overlay is on the present's
// critical path by construction, so a second queue can only add cross-queue
// waits and - on NVIDIA, where the whole graphics family is one engine - two
// channel context switches per frame.
TEST(OverlaySubmitQueuePolicyTest, ComputeOnlyPresentQueueJoinsTheGamesGraphicsQueue) {
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, true, true, false)), OverlaySubmitQueue::kBorrowedQueue)
        << "in-order on the game's own queue beats a queue of CE's own that the present must then wait for";
}

TEST(OverlaySubmitQueuePolicyTest, AsyncSubmittingGamesGetCesOwnQueue) {
    // When the game submits from a thread other than the one it presents on, it
    // can be queueing the next frame while CE is inside this present: appending
    // there could put a whole frame of the game's work ahead of the overlay the
    // present has to wait for. Two context switches beat a frame.
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, true, true, true)), OverlaySubmitQueue::kReservedQueue);
}

TEST(OverlaySubmitQueuePolicyTest, SingleGraphicsQueueHardwareBorrows) {
    // AMD exposes one graphics queue, so a game that creates it leaves nothing
    // to reserve. Borrowing under CE's submission lock is what keeps the
    // overlay alive there instead of silently disappearing - even for a game
    // that submits concurrently, where borrowing is the only option left.
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, false, true, false)), OverlaySubmitQueue::kBorrowedQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, false, true, true)), OverlaySubmitQueue::kBorrowedQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, false, false, false)), OverlaySubmitQueue::kNone);
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, false, false, true)), OverlaySubmitQueue::kNone);
}

TEST(OverlaySubmitQueuePolicyTest, ReservedQueueStillCoversAConcurrentGameWithNoBorrowCandidate) {
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, true, false, true)), OverlaySubmitQueue::kReservedQueue);
    EXPECT_EQ(ChooseOverlaySubmitQueue(Availability(false, true, false, false)), OverlaySubmitQueue::kReservedQueue);
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
    EXPECT_NE(queueOwner.find("FindLastGameGraphicsSubmitQueue(device)"), std::string::npos)
        << "the borrow candidate must be the queue that produced the frame, so the overlay is in-order behind it";
}

TEST(OverlaySubmitQueuePolicySourceTest, TheBorrowedQueuePublicationDiesWithItsDevice) {
    // Nothing else clears the publication, so a device teardown must - otherwise
    // the next device inherits a dangling VkQueue as the handle CE serializes
    // every game submission against.
    const std::string queueOwner = ReadProjectSource("hook/vulkan_layer/layer_overlay_queue.cpp");
    ASSERT_FALSE(queueOwner.empty());
    EXPECT_NE(queueOwner.find("void ForgetBorrowedOverlaySubmitQueue(VkDevice device)"), std::string::npos);

    const std::string hooks = ReadProjectSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    const size_t destroy = hooks.find("Capture_vkDestroyDevice(VkDevice device");
    ASSERT_NE(destroy, std::string::npos);
    const size_t forget = hooks.find("ForgetBorrowedOverlaySubmitQueue(device)", destroy);
    const size_t unregister = hooks.find("UnregisterDevice(device)", destroy);
    ASSERT_NE(forget, std::string::npos);
    ASSERT_NE(unregister, std::string::npos);
    EXPECT_LT(forget, unregister) << "the release check needs the queue-to-device mapping to still be live";
}

TEST(OverlaySubmitQueuePolicySourceTest, DeviceCreateFallsBackWhenTheReservationIsRejected) {
    const std::string hooks = ReadProjectSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    const size_t reservation = hooks.find("BuildOverlayQueueReservation(");
    ASSERT_NE(reservation, std::string::npos);
    EXPECT_NE(hooks.find("retrying with", reservation), std::string::npos)
        << "CE's extra queue must never be the reason a game fails to create its device";
}
