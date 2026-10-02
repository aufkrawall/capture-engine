#include <gtest/gtest.h>

#include "common/capture/capture_policy/inject_lineage.h"

namespace {

using ce::capture_policy::InjectLineageObservation;
using ce::capture_policy::InjectLineageTracker;
using ce::capture_policy::ShouldLogInjectLineageGenerationReset;

constexpr size_t kSlots = 16;

bool AnyAnomaly(const InjectLineageObservation& observation) {
    return observation.lineageRegression || observation.textureReuse;
}

}  // namespace

TEST(InjectLineagePolicyTest, MonotonicFramesAcrossRotatingSlotsAreClean) {
    InjectLineageTracker<kSlots> tracker;
    for (uint32_t frame = 1; frame <= 64; ++frame) {
        const InjectLineageObservation observation =
            tracker.Observe(1, frame, static_cast<int32_t>((frame - 1) % kSlots));
        EXPECT_FALSE(AnyAnomaly(observation)) << "frame " << frame;
        EXPECT_FALSE(observation.generationReset);
    }
    EXPECT_EQ(tracker.LastFrameIndex(), 64u);
}

TEST(InjectLineagePolicyTest, RegressionWithinOneGenerationIsReported) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(3, 10, 0);
    const InjectLineageObservation observation = tracker.Observe(3, 7, 1);
    EXPECT_TRUE(observation.lineageRegression);
    EXPECT_EQ(observation.previousFrameIndex, 10u);
    EXPECT_FALSE(observation.generationReset);
}

TEST(InjectLineagePolicyTest, SlotCarryingAnOlderFrameWithinOneGenerationIsReported) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(3, 20, 4);
    tracker.Observe(3, 21, 5);
    const InjectLineageObservation observation = tracker.Observe(3, 20, 4);
    EXPECT_TRUE(observation.textureReuse);
    EXPECT_EQ(observation.previousTextureFrame, 20u);
    EXPECT_TRUE(observation.lineageRegression);
}

// Reproduces a recording where a swapchain re-creation (FG toggle plus a
// 2560x1440 -> 3840x2160 source change) republished the transport as
// generation 2 with the frame counter restarting at 1. The old checks logged
// one lineage regression and a slot "reuse" per texture slot.
TEST(InjectLineagePolicyTest, TransportGenerationChangeRestartsLineageWithoutAnomalies) {
    InjectLineageTracker<kSlots> tracker;
    for (uint32_t frame = 1; frame <= 313; ++frame) {
        ASSERT_FALSE(AnyAnomaly(tracker.Observe(1, frame, static_cast<int32_t>((frame - 1) % kSlots))));
    }

    const InjectLineageObservation first = tracker.Observe(2, 1, 0);
    EXPECT_TRUE(first.generationReset);
    EXPECT_EQ(first.previousGeneration, 1u);
    EXPECT_EQ(first.previousGenerationLastFrame, 313u);
    EXPECT_EQ(first.generationResetCount, 1u);
    EXPECT_FALSE(AnyAnomaly(first));

    for (uint32_t frame = 2; frame <= 40; ++frame) {
        const InjectLineageObservation observation =
            tracker.Observe(2, frame, static_cast<int32_t>((frame - 1) % kSlots));
        EXPECT_FALSE(observation.generationReset) << "frame " << frame;
        EXPECT_FALSE(AnyAnomaly(observation)) << "frame " << frame;
    }
}

TEST(InjectLineagePolicyTest, RegressionInsideTheNewGenerationIsStillReported) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(1, 500, 3);
    tracker.Observe(2, 1, 0);
    tracker.Observe(2, 9, 1);
    const InjectLineageObservation observation = tracker.Observe(2, 5, 2);
    EXPECT_TRUE(observation.lineageRegression);
    EXPECT_EQ(observation.previousFrameIndex, 9u);
}

TEST(InjectLineagePolicyTest, NonLineageFramesNeitherCheckNorAdoptAGeneration) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(4, 100, 0);
    // A WGC/framegrab frame: no inject frame index and no shared texture slot.
    const InjectLineageObservation wgc = tracker.Observe(0, 0, -1);
    EXPECT_FALSE(wgc.generationReset);
    EXPECT_FALSE(AnyAnomaly(wgc));

    const InjectLineageObservation next = tracker.Observe(4, 101, 1);
    EXPECT_FALSE(next.generationReset);
    EXPECT_FALSE(AnyAnomaly(next));
}

TEST(InjectLineagePolicyTest, OutOfRangeTextureIndexSkipsOnlyTheSlotCheck) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(1, 10, static_cast<int32_t>(kSlots));
    const InjectLineageObservation observation = tracker.Observe(1, 8, static_cast<int32_t>(kSlots) + 5);
    EXPECT_TRUE(observation.lineageRegression);
    EXPECT_FALSE(observation.textureReuse);
}

TEST(InjectLineagePolicyTest, ResetClearsLineageAndGeneration) {
    InjectLineageTracker<kSlots> tracker;
    tracker.Observe(1, 50, 2);
    tracker.Reset();
    const InjectLineageObservation observation = tracker.Observe(7, 1, 2);
    EXPECT_FALSE(observation.generationReset);
    EXPECT_FALSE(AnyAnomaly(observation));
    EXPECT_EQ(tracker.LastFrameIndex(), 1u);
}

TEST(InjectLineagePolicyTest, GenerationResetLoggingIsSampledAfterTheFirstResets) {
    EXPECT_FALSE(ShouldLogInjectLineageGenerationReset(0));
    for (uint32_t count = 1; count <= 16; ++count) {
        EXPECT_TRUE(ShouldLogInjectLineageGenerationReset(count)) << count;
    }
    EXPECT_FALSE(ShouldLogInjectLineageGenerationReset(17));
    EXPECT_FALSE(ShouldLogInjectLineageGenerationReset(255));
    EXPECT_TRUE(ShouldLogInjectLineageGenerationReset(256));
    EXPECT_TRUE(ShouldLogInjectLineageGenerationReset(512));
}
