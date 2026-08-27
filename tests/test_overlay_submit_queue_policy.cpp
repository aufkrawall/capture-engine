#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/overlay_submit_queue_policy.h"
#include "../hook/vulkan_layer/vulkan_prerender_policy.h"
#include "../hook/vulkan_layer/vulkan_reflex_limiter.h"
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
using ce::overlay_submit_queue_policy::CanReuseComputeCompositeCommand;
using ce::overlay_submit_queue_policy::CanWidenQueueCreateEntry;
using ce::overlay_submit_queue_policy::ChooseOverlaySubmitQueue;
using ce::overlay_submit_queue_policy::ChooseIndependentGraphicsQueue;
using ce::overlay_submit_queue_policy::ComputeCompositeBounds;
using ce::overlay_submit_queue_policy::ComputePresentAvailability;
using ce::overlay_submit_queue_policy::OverlaySubmitQueue;
using ce::overlay_submit_queue_policy::ReservedOverlayQueueIndex;
using ce::overlay_submit_queue_policy::ReservedOverlayQueuePriority;
using ce::overlay_submit_queue_policy::ShouldSerializeSubmissionsOnQueue;
using ce::overlay_submit_queue_policy::ShouldUseComputePresent;
using ce::overlay_submit_queue_policy::SubmitQueueAvailability;

SubmitQueueAvailability Availability(bool presentGraphics, bool reserved, bool borrowed, bool concurrent) {
    SubmitQueueAvailability availability;
    availability.presentQueueSupportsGraphics = presentGraphics;
    availability.reservedQueueAvailable = reserved;
    availability.borrowedQueueAvailable = borrowed;
    availability.gameSubmitsConcurrently = concurrent;
    return availability;
}

ComputePresentAvailability ComputeAvailability(bool graphics, bool compute, bool storage, bool readWithoutFormat,
                                               bool writeWithoutFormat) {
    ComputePresentAvailability availability;
    availability.presentQueueSupportsGraphics = graphics;
    availability.presentQueueSupportsCompute = compute;
    availability.swapchainSupportsStorage = storage;
    availability.storageReadWithoutFormatAvailable = readWithoutFormat;
    availability.storageWriteWithoutFormatAvailable = writeWithoutFormat;
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

TEST(OverlaySubmitQueuePolicyTest, StorageCapableComputePresentStaysOnItsOriginalQueue) {
    EXPECT_TRUE(ShouldUseComputePresent(ComputeAvailability(false, true, true, true, true)));
    EXPECT_FALSE(ShouldUseComputePresent(ComputeAvailability(true, true, true, true, true)))
        << "a graphics present already has the zero-cross-queue direct render-pass path";
    EXPECT_FALSE(ShouldUseComputePresent(ComputeAvailability(false, false, true, true, true)));
    EXPECT_FALSE(ShouldUseComputePresent(ComputeAvailability(false, true, false, true, true)));
    EXPECT_FALSE(ShouldUseComputePresent(ComputeAvailability(false, true, true, false, true)));
    EXPECT_FALSE(ShouldUseComputePresent(ComputeAvailability(false, true, true, true, false)));
}

TEST(OverlaySubmitQueuePolicyTest, IndependentOffscreenWorkPrefersCesQueue) {
    EXPECT_EQ(ChooseIndependentGraphicsQueue(true, true), OverlaySubmitQueue::kReservedQueue);
    EXPECT_EQ(ChooseIndependentGraphicsQueue(false, true), OverlaySubmitQueue::kBorrowedQueue);
    EXPECT_EQ(ChooseIndependentGraphicsQueue(false, false), OverlaySubmitQueue::kNone);
}

TEST(OverlaySubmitQueuePolicyTest, ComputeCommandReuseRequiresIdenticalRecordedBounds) {
    const ComputeCompositeBounds bounds = {100, 50, 640, 240};
    EXPECT_FALSE(CanReuseComputeCompositeCommand(false, bounds, bounds));
    EXPECT_TRUE(CanReuseComputeCompositeCommand(true, bounds, bounds));
    EXPECT_FALSE(CanReuseComputeCompositeCommand(true, bounds, {101, 50, 640, 240}));
    EXPECT_FALSE(CanReuseComputeCompositeCommand(true, bounds, {100, 51, 640, 240}));
    EXPECT_FALSE(CanReuseComputeCompositeCommand(true, bounds, {100, 50, 641, 240}));
    EXPECT_FALSE(CanReuseComputeCompositeCommand(true, bounds, {100, 50, 640, 241}));
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

// DOOM Eternal session `20260819_143521`: with the overlay on the game's own
// graphics queue, "present from compute" still cost ~385 us per frame (median
// frame time 8666 us on the two-image swapchain versus 8281 us on the
// three-image one) while CE's CPU time inside the hook was identical, 230 us
// versus 226 us. A swapchain with a spare image absorbs whatever CE adds to the
// present path; one without pays for it in frame time. So everything CE does
// before the present down-call has to earn its place there.
TEST(OverlaySubmitQueuePolicySourceTest, DiagnosticsOnlyWorkRunsAfterThePresentDownCall) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    ASSERT_FALSE(present.empty());

    const size_t downCall = present.find("fp_vkQueuePresentKHR(queue, (pPresentInfo && modified)");
    ASSERT_NE(downCall, std::string::npos);
    // Each of these scans up to five seconds of frame history and feeds nothing
    // but the perf CSV row.
    for (const char* sample : {"Get1PercentLowFPS()", "Get01PercentLowFPS()", "GetWindowStdDev()"}) {
        const size_t at = present.find(sample);
        ASSERT_NE(at, std::string::npos) << sample;
        EXPECT_GT(at, downCall) << sample << " must be sampled after the present, not in front of it";
    }
    // The split that makes a future regression measurable instead of arguable.
    EXPECT_NE(present.find("perfMetrics.prePresentUs"), std::string::npos);
    EXPECT_NE(present.find("perfMetrics.presentCallUs"), std::string::npos);
    EXPECT_NE(present.find("perfMetrics.postPresentUs"), std::string::npos);
}

// The overlay render pass declares PRESENT_SRC_KHR as both its initial and its
// final layout, so it performs exactly one transition each way. The explicit
// barriers that used to bracket it added a third, and a fourth that was invalid
// outright: it named COLOR_ATTACHMENT_OPTIMAL as the old layout for an image the
// render pass had already handed back in PRESENT_SRC_KHR.
TEST(OverlaySubmitQueuePolicySourceTest, TheRenderPassOwnsBothLayoutTransitions) {
    const std::string init = ReadProjectSource("hook/vulkan_layer/layer_overlay.cpp");
    ASSERT_FALSE(init.empty());
    EXPECT_NE(init.find("attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"), std::string::npos);
    EXPECT_NE(init.find("attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"), std::string::npos);
    EXPECT_NE(init.find("dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL"), std::string::npos)
        << "dropping the trailing barrier requires the outgoing subpass dependency that replaced it";

    const std::string render = ReadProjectSource("hook/vulkan_layer/layer_overlay_render.cpp");
    ASSERT_FALSE(render.empty());
    EXPECT_EQ(render.find("fp_vkCmdPipelineBarrier"), std::string::npos)
        << "the render pass performs the layout transitions; an explicit barrier here duplicates or contradicts them";
}

// Which queue signals what a present waits on decides whether CE's overlay - a
// render pass, so always on a graphics queue - inserts a cross-engine round trip
// the game never had. It is not recoverable from a frame-time graph, so the
// layer states it once per swapchain generation.
TEST(OverlaySubmitQueuePolicySourceTest, PresentTopologyIsIdentifiedOncePerSwapchainGeneration) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string topology = ReadProjectSource("hook/vulkan_layer/vulkan_layer_state.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(topology.empty());
    EXPECT_NE(topology.find("Present topology - present queue family"), std::string::npos);
    EXPECT_NE(present.find("ArmPresentTopologyLearning()"), std::string::npos)
        << "a swapchain recreate is exactly when a game's present topology can change";
    EXPECT_NE(topology.find("FinishPresentTopologyLearning()"), std::string::npos)
        << "learning must stop once the answer is known, so the steady state pays only an atomic load";

    const std::string hooks = ReadProjectSource("hook/vulkan_layer/layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    size_t noteCount = 0;
    for (size_t pos = hooks.find("NotePresentTopologyDependencies"); pos != std::string::npos;
         pos = hooks.find("NotePresentTopologyDependencies", pos + 1)) {
        ++noteCount;
    }
    // Two definitions plus both paths of each of the three submit wrappers.
    EXPECT_EQ(noteCount, 8u);
}

TEST(OverlaySubmitQueuePolicySourceTest, PrerenderLimitUsesSemaphoreDerivedGraphicsProducer) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string state = ReadProjectSource("hook/vulkan_layer/vulkan_layer.h");
    const std::string topology = ReadProjectSource("hook/vulkan_layer/vulkan_layer_state.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(state.empty());
    ASSERT_FALSE(topology.empty());

    EXPECT_NE(state.find("prerenderProducerQueue"), std::string::npos);
    EXPECT_NE(topology.find("GetSemaphoreGraphicsProducerQueue"), std::string::npos);
    EXPECT_NE(topology.find("prerenderProducerQueue.store(graphicsQueue"), std::string::npos);
    EXPECT_NE(present.find("prerenderOnProducerSubmit"), std::string::npos)
        << "a queue owned by another game thread must be paced from that thread's submit wrapper";
    EXPECT_NE(present.find("ApplyPrerenderLimitVulkan(queueDevice, prerenderQueue"), std::string::npos)
        << "a compute-only present queue must not silently disable a configured render queue depth";
    EXPECT_EQ(present.find("queue-depth marker withheld"), std::string::npos)
        << "cross-thread ownership must move the marker instead of disabling the configured override";

    const std::string hooks = ReadProjectSource("hook/vulkan_layer/layer_hooks.cpp");
    ASSERT_FALSE(hooks.empty());
    EXPECT_NE(hooks.find("pWaitSemaphores"), std::string::npos);
    EXPECT_NE(hooks.find("pWaitSemaphoreInfos"), std::string::npos)
        << "submit dependencies must propagate the upstream graphics producer through compute queues";
    EXPECT_NE(hooks.find("IsPrerenderProducerSubmit"), std::string::npos);
    EXPECT_NE(hooks.find("ApplyPrerenderLimitVulkan"), std::string::npos);
    EXPECT_NE(hooks.find("queue, prerenderLimit, true"), std::string::npos)
        << "the marker must remain inside the app submit wrapper's queue-serialization scope";
}

TEST(VulkanPrerenderPolicyTest, MovesCrossThreadQueuePacingToTheProducerSubmit) {
    using ce::vulkan_prerender_policy::ShouldPaceOnProducerSubmit;

    EXPECT_TRUE(ShouldPaceOnProducerSubmit(8184, 27228));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(8184, 8184));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(0, 8184));
    EXPECT_FALSE(ShouldPaceOnProducerSubmit(8184, 0));
}

TEST(OverlaySubmitQueuePolicySourceTest, ForcedAnisotropyPublishesBoundedApplicationProof) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    ASSERT_FALSE(present.empty());
    EXPECT_NE(present.find("Vulkan sampler: forced AF applied"), std::string::npos);
    EXPECT_NE(present.find("s_anisotropyAppliedLogCount"), std::string::npos);
}

TEST(OverlaySubmitQueuePolicySourceTest, VulkanReflexUsesDriverSignaledPostPresentWait) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string reflex = ReadProjectSource("hook/vulkan_layer/vulkan_reflex_limiter.h");
    const std::string entry = ReadProjectSource("hook/vulkan_layer/layer_main.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(reflex.empty());
    ASSERT_FALSE(entry.empty());

    EXPECT_NE(reflex.find("kSetSleepModeId = 0x2acfd162"), std::string::npos);
    EXPECT_NE(reflex.find("kSleepId = 0x36732b1e"), std::string::npos);
    EXPECT_NE(reflex.find("fp_vkWaitSemaphores"), std::string::npos);
    EXPECT_NE(present.find("g_SharedFpsLimiter.Apply(true, nativeVulkanPresent)"), std::string::npos);
    EXPECT_NE(present.find("g_SharedFpsLimiter.ApplyPostPresent()"), std::string::npos);
    EXPECT_NE(entry.find("Capture_vkSetLatencySleepModeNV"), std::string::npos);
    EXPECT_NE(entry.find("Capture_vkLatencySleepNV"), std::string::npos);
}

TEST(VulkanReflexLimiterTest, ExposesCompleteNativePacingCallbacks) {
    const NativeFpsPacingBackend backend = GetVulkanNativeFpsPacingBackend();
    EXPECT_NE(backend.context, nullptr);
    EXPECT_NE(backend.isAvailable, nullptr);
    EXPECT_NE(backend.isGameActive, nullptr);
    EXPECT_NE(backend.setTargetFps, nullptr);
    EXPECT_NE(backend.sleep, nullptr);
    EXPECT_NE(backend.clear, nullptr);
    ASSERT_NE(backend.name, nullptr);
    EXPECT_STREQ(backend.name, "Vulkan Reflex");
}

// DOOM Eternal `doomasyncpresentswitch` (2026-08-22) proves the remaining
// penalty is GPU scheduling, not CE's CPU/GPU workload: overlay CPU is 64-66 us
// and overlay GPU is 9 us in both modes, but the compute-signalled present
// forces the direct render pass through compute -> graphics -> compute.
TEST(OverlaySubmitQueuePolicySourceTest, ComputePresentCompositesOnThePresentQueue) {
    const std::string compute = ReadProjectSource("hook/vulkan_layer/layer_overlay_compute.cpp");
    const std::string render = ReadProjectSource("hook/vulkan_layer/layer_overlay_render.cpp");
    const std::string shader = ReadProjectSource("hook/vulkan_layer/shaders/overlay_composite.comp");
    const std::string spirv = ReadProjectSource("hook/common/overlay_shader_spirv.h");
    const std::string generator = ReadProjectSource("tools/compile_vulkan_overlay_shaders.py");
    ASSERT_FALSE(compute.empty());
    ASSERT_FALSE(render.empty());
    ASSERT_FALSE(shader.empty());
    ASSERT_FALSE(spirv.empty());
    ASSERT_FALSE(generator.empty());

    EXPECT_NE(render.find("ShouldUseComputePresent"), std::string::npos);
    EXPECT_NE(compute.find("fp_vkQueueSubmit(presentQueue"), std::string::npos)
        << "the final composite must stay on the compute/present queue";
    EXPECT_NE(compute.find("VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT"), std::string::npos);
    EXPECT_EQ(compute.find("graphicsSubmit.waitSemaphoreCount"), std::string::npos)
        << "offscreen graphics must run concurrently, not wait behind the game's final compute work";
    EXPECT_NE(compute.find("computeSubmit.waitSemaphoreCount"), std::string::npos)
        << "the compute composite joins the game's present waits and the independently rendered overlay";
    EXPECT_NE(compute.find("CanReuseComputeCompositeCommand"), std::string::npos)
        << "stable overlay bounds must not force compute command recording on every present";
    EXPECT_NE(compute.find("state.computeWaitSemaphores"), std::string::npos)
        << "the present hot path must retain its semaphore scratch allocation";
    EXPECT_EQ(compute.find("std::vector<VkSemaphore> waits"), std::string::npos)
        << "a local vector allocates again on every present";
    EXPECT_NE(shader.find("overlay.rgb + base.rgb * (1.0 - overlay.a)"), std::string::npos)
        << "the offscreen render pass produces premultiplied RGB";
    EXPECT_NE(spirv.find("g_ComputeCompositeShaderSpv"), std::string::npos);
    EXPECT_NE(generator.find("overlay_composite.comp"), std::string::npos)
        << "the checked-in payload must remain reproducible and SPIR-V validated";
}

TEST(OverlaySubmitQueuePolicySourceTest, ComputePresentIsCapabilityGatedAndKeepsTheGraphicsFallback) {
    const std::string present = ReadProjectSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    const std::string render = ReadProjectSource("hook/vulkan_layer/layer_overlay_render.cpp");
    const std::string hooks = ReadProjectSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(render.empty());
    ASSERT_FALSE(hooks.empty());

    EXPECT_NE(present.find("sd->imageUsage = pCreateInfo->imageUsage"), std::string::npos);
    EXPECT_NE(render.find("VK_IMAGE_USAGE_STORAGE_BIT"), std::string::npos);
    EXPECT_NE(render.find("storageImageReadWithoutFormatAvailable"), std::string::npos);
    EXPECT_NE(render.find("storageImageWriteWithoutFormatAvailable"), std::string::npos);
    EXPECT_NE(hooks.find("shaderStorageImageReadWithoutFormat"), std::string::npos);
    EXPECT_NE(hooks.find("shaderStorageImageWriteWithoutFormat"), std::string::npos);
    EXPECT_NE(hooks.find("VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME"), std::string::npos)
        << "the SPIR-V capabilities may come from Vulkan 1.3 or the format-feature-flags2 extension";

    const std::string initialization = ReadProjectSource("hook/vulkan_layer/layer_overlay.cpp");
    ASSERT_FALSE(initialization.empty());
    EXPECT_NE(initialization.find("VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT"), std::string::npos);
    EXPECT_NE(initialization.find("VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT"), std::string::npos)
        << "B8G8R8A8 is not core-guaranteed for formatless access, so the exact format needs querying";
    EXPECT_NE(initialization.find("formatFeatureFlags2Available &&"), std::string::npos)
        << "VkFormatProperties3 must not be chained on devices that do not support its API or extension";

    const size_t computeAttempt = render.find("RenderComputePresentOverlay");
    const size_t directRenderPass = render.find("// Record command buffer");
    ASSERT_NE(computeAttempt, std::string::npos);
    ASSERT_NE(directRenderPass, std::string::npos);
    EXPECT_LT(computeAttempt, directRenderPass)
        << "unsupported formats/features must fall through to the proven graphics route";
}
