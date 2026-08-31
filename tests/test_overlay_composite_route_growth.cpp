#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../hook/vulkan_layer/overlay_submit_queue_policy.h"
#include "source_fragment_reader.h"

// Regression coverage for Portal RTX session
// `installed/captureengine/logs/20260831_054801`, which showed two independent
// overlay defects under 4x DLSS multi-frame generation:
//
//  1. The overlay's composite route alternated per present. Six swapchain
//     images produced ten submission slots and 60 cached composites; the ring
//     then extended to twelve to absorb the generated-frame burst, and the two
//     appended slots indexed past every compute-route array. They failed that
//     route's bounds check and fell back to the direct render-pass route -
//     exactly the compute -> graphics -> compute round trip the compositor
//     exists to remove on a compute-only present queue. `Compute-present CPU
//     summary` counted 2048 composites per 15.9-17.1 s window while
//     `perf_metrics_28608.csv` recorded presents at 143 Hz, i.e. 83-90% of
//     presents on one route and the remainder on the other.
//
//  2. The overlay's DLSS FG factor froze on the value the game started with.
//     The layer mirrors the hook DLL's DLSS-G runtime state out of shared
//     memory, and that mirror sat inside the FPS limiter's
//     `if (!asyncPresentDetected)` block. `asyncPresentDetected` latched 7.5 s
//     into the run, after which `hook_debug.log` recorded the in-game 4x -> 3x
//     change while `vulkan_layer.log` never left `multiplier 0 -> 4`.

namespace {

using ce::overlay_submit_queue_policy::ComputeCompositeResourceIndex;
using ce::overlay_submit_queue_policy::RequiredComputeCompositeResourceCount;
using ce::overlay_submit_queue_policy::ResolveSubmissionSlotCount;
using ce::overlay_submit_queue_policy::ShouldSkipRepeatPresentComposite;
using ce::overlay_submit_queue_policy::kMaxSubmissionSlots;

std::string LogicalOverlaySource() {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" /
                                              "layer_overlay.cpp");
}

std::string LogicalLayerSource() {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "hook" / "vulkan_layer" /
                                              "vulkan_layer.cpp");
}

}  // namespace

TEST(OverlayCompositeRouteGrowthTest, PortalRtxRingGrowthOutrunsTheInitialCompositeResources) {
    constexpr uint32_t kImageCount = 6;
    const uint32_t initialSlots = ResolveSubmissionSlotCount(kImageCount);
    ASSERT_EQ(initialSlots, 10u);
    EXPECT_EQ(RequiredComputeCompositeResourceCount(initialSlots, kImageCount), 60u);

    // The measured session extended the ring by two. Every pair index of those
    // appended slots is outside the initial allocation, which is what silently
    // moved those presents onto the direct render-pass route.
    constexpr uint32_t kGrownSlots = 12;
    for (uint32_t slot = initialSlots; slot < kGrownSlots; ++slot) {
        for (uint32_t image = 0; image < kImageCount; ++image) {
            EXPECT_GE(ComputeCompositeResourceIndex(slot, image, kImageCount),
                      RequiredComputeCompositeResourceCount(initialSlots, kImageCount));
        }
    }

    // Sizing on the current ring depth instead is what keeps a single route.
    const uint32_t grownRequirement = RequiredComputeCompositeResourceCount(kGrownSlots, kImageCount);
    EXPECT_EQ(grownRequirement, 72u);
    for (uint32_t slot = 0; slot < kGrownSlots; ++slot) {
        for (uint32_t image = 0; image < kImageCount; ++image) {
            EXPECT_LT(ComputeCompositeResourceIndex(slot, image, kImageCount), grownRequirement);
        }
    }
}

TEST(OverlayCompositeRouteGrowthTest, CompositeResourceCountIsSlotMajorAndOverflowSafe) {
    EXPECT_EQ(RequiredComputeCompositeResourceCount(0, 6), 0u);
    EXPECT_EQ(RequiredComputeCompositeResourceCount(10, 0), 0u);
    EXPECT_EQ(RequiredComputeCompositeResourceCount(1, 1), 1u);
    EXPECT_EQ(RequiredComputeCompositeResourceCount(kMaxSubmissionSlots, 6), kMaxSubmissionSlots * 6u);
    EXPECT_EQ(RequiredComputeCompositeResourceCount(UINT32_MAX, 2), UINT32_MAX);

    // Appending exactly one slot's row must leave every existing pair index
    // valid, which is the property that makes growth possible at all.
    constexpr uint32_t kImageCount = 6;
    for (uint32_t slot = 0; slot < 12; ++slot) {
        EXPECT_EQ(ComputeCompositeResourceIndex(slot, 0, kImageCount),
                  RequiredComputeCompositeResourceCount(slot, kImageCount));
    }
}

TEST(OverlayCompositeRouteGrowthTest, RepeatPresentOfOneImageIsNotCompositedTwice) {
    // Same image, same acquire generation: the runtime presented it again
    // without the application re-acquiring it, so CE's overlay is still in it
    // and a second blend would show a more opaque panel on that present.
    EXPECT_TRUE(ShouldSkipRepeatPresentComposite(7, 7));

    // Re-acquired: new content, so the overlay has to be composited again.
    EXPECT_FALSE(ShouldSkipRepeatPresentComposite(8, 7));
    EXPECT_FALSE(ShouldSkipRepeatPresentComposite(1, 0));

    // No acquire observed for this image at all is no evidence either way. The
    // guard must fail open rather than suppress the overlay for the rest of the
    // session on an application whose acquires CE cannot see.
    EXPECT_FALSE(ShouldSkipRepeatPresentComposite(0, 0));
}

TEST(OverlayCompositeRouteGrowthSourceTest, RingGrowthExtendsTheCompositeRouteOrDoesNotHappen) {
    const std::string overlay = LogicalOverlaySource();
    ASSERT_FALSE(overlay.empty());

    const size_t grow = overlay.find("bool GrowSubmissionRing(OverlayState& state, DeviceDispatch* disp) {");
    ASSERT_NE(grow, std::string::npos);
    const size_t appendRoute = overlay.find("AppendComputePresentSlot(state, disp)", grow);
    const size_t popSlot = overlay.find("PopSubmissionRingSlot(state, disp)", grow);
    const size_t growthCounter = overlay.find("++state.submissionRingGrowths", grow);
    ASSERT_NE(appendRoute, std::string::npos);
    ASSERT_NE(popSlot, std::string::npos);
    ASSERT_NE(growthCounter, std::string::npos);
    // The route is extended before the growth is counted, and the appended slot
    // is released again when it cannot be.
    EXPECT_LT(appendRoute, growthCounter);
    EXPECT_LT(popSlot, growthCounter);

    // The compute route's per-slot resources are appended one slot at a time,
    // so the initial creation and the growth path share the same code.
    EXPECT_NE(overlay.find("bool AppendComputePresentSlotLocked(OverlayState& state, DeviceDispatch* disp)"),
              std::string::npos);
    EXPECT_NE(overlay.find("void PopOffscreenTarget(OverlayState& state, DeviceDispatch* disp)"), std::string::npos);
    // One descriptor pool per slot: growing the ring adds a pool instead of
    // having to reallocate every existing descriptor set.
    EXPECT_NE(overlay.find("std::vector<VkDescriptorPool> computeDescriptorPools;"), std::string::npos);

    // The timestamp query pool covers only the slots that existed at
    // initialization, so an appended slot must not write query indices past it.
    const size_t computeRoute =
        overlay.find("bool RenderComputePresentOverlay(OverlayState& state, DeviceDispatch* disp");
    ASSERT_NE(computeRoute, std::string::npos);
    const size_t computeTimestamps = overlay.find("const bool writeTimestamps", computeRoute);
    ASSERT_NE(computeTimestamps, std::string::npos);
    EXPECT_NE(overlay.find("submissionSlot < state.timestampSlotCapacity", computeTimestamps), std::string::npos);
}

TEST(OverlayCompositeRouteGrowthSourceTest, RenderOverlayGuardsRepeatPresentsBeforeTakingASlot) {
    const std::string overlay = LogicalOverlaySource();
    ASSERT_FALSE(overlay.empty());

    const size_t render = overlay.find("bool RenderOverlay(VkDevice device, VkQueue queue, uint32_t imageIndex");
    ASSERT_NE(render, std::string::npos);
    const size_t publish = overlay.find("PublishDetectedOverlayFGMetrics", render);
    const size_t guard = overlay.find("ShouldSkipRepeatPresentComposite", render);
    const size_t chooseSlot = overlay.find("ChooseSubmissionSlot", render);
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(chooseSlot, std::string::npos);
    // A repeated present is still a presented frame, so metrics are updated
    // first; the guard then runs before any submission resource is taken.
    EXPECT_LT(publish, guard);
    EXPECT_LT(guard, chooseSlot);

    // The guard's evidence is recorded only after a composite actually reached
    // a queue, on both routes.
    EXPECT_GE(overlay.find("RecordOverlayComposite(state, imageIndex, acquireGeneration)", render), render);
    size_t recorded = 0;
    for (size_t at = overlay.find("RecordOverlayComposite(state, imageIndex, acquireGeneration)");
         at != std::string::npos;
         at = overlay.find("RecordOverlayComposite(state, imageIndex, acquireGeneration)", at + 1)) {
        ++recorded;
    }
    EXPECT_EQ(recorded, 2u);
}

TEST(OverlayCompositeRouteGrowthSourceTest, SharedDlssFactorMirrorIsNotGatedOnTheLimiterPath) {
    const std::string layer = LogicalLayerSource();
    ASSERT_FALSE(layer.empty());

    const size_t present = layer.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkQueuePresentKHR");
    ASSERT_NE(present, std::string::npos);
    const size_t mirror = layer.find("g_FGCompat.SetDLSSFGMultiplier(std::clamp(sharedDLSSFGMultiplier", present);
    const size_t limiterBlock = layer.find("if (!asyncPresentDetected) {", present);
    ASSERT_NE(mirror, std::string::npos);
    ASSERT_NE(limiterBlock, std::string::npos);
    // The overlay's FG label is made of this mirror, and `asyncPresentDetected`
    // latches for the rest of the session. It must run on every present.
    EXPECT_LT(mirror, limiterBlock);
}

TEST(OverlayCompositeRouteGrowthSourceTest, BothAcquireEntryPointsMaintainTheAcquireGeneration) {
    const std::string layer = LogicalLayerSource();
    ASSERT_FALSE(layer.empty());

    // Slot reuse and the repeat-present guard both read the acquire generation,
    // so an acquire that bypasses CE would strand the submission ring at its
    // safety bound and blind the guard.
    EXPECT_NE(layer.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImage2KHR"), std::string::npos);
    const size_t acquire2 = layer.find("disp->fp_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex)");
    ASSERT_NE(acquire2, std::string::npos);
    EXPECT_NE(layer.find("EndAcquireBoundary(sd, acquireResult, pImageIndex)", acquire2), std::string::npos);

    const std::string layerMain =
        ce::test_source::ReadFile(std::filesystem::current_path() / "hook" / "vulkan_layer" / "layer_main.cpp");
    ASSERT_FALSE(layerMain.empty());
    size_t registrations = 0;
    for (size_t at = layerMain.find("Capture_vkAcquireNextImage2KHR"); at != std::string::npos;
         at = layerMain.find("Capture_vkAcquireNextImage2KHR", at + 1)) {
        ++registrations;
    }
    // Instance and device vkGetProcAddr both have to hand it out; a game that
    // resolves it from either one must reach CE.
    EXPECT_EQ(registrations, 2u);
}
