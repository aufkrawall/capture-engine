#include <gtest/gtest.h>
#include <deque>
#include <utility>
#include "../common/frame_queue.h"
#include "../common/frame_timing_utils.h"

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridPrefersNearestTimestamp) {
    std::deque<QueuedFrame> frames;

    QueuedFrame frameA;
    frameA.timestamp = 100;
    frames.push_back(std::move(frameA));

    QueuedFrame frameB;
    frameB.timestamp = 150;
    frames.push_back(std::move(frameB));

    QueuedFrame frameC;
    frameC.timestamp = 220;
    frames.push_back(std::move(frameC));

    const size_t bestIndex = SelectFrameClosestToGrid(frames, frames.size(), 100, 2, 60);
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToTimestampPrefersNearestTimestamp) {
    std::deque<QueuedFrame> frames;

    QueuedFrame older;
    older.timestamp = 100;
    frames.push_back(std::move(older));

    QueuedFrame nearer;
    nearer.timestamp = 158;
    frames.push_back(std::move(nearer));

    QueuedFrame newer;
    newer.timestamp = 190;
    frames.push_back(std::move(newer));

    const size_t bestIndex = SelectFrameClosestToTimestamp(frames, frames.size(), 160);
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToTimestampBreaksTiesTowardNewerFrame) {
    std::deque<QueuedFrame> frames;

    QueuedFrame older;
    older.timestamp = 140;
    frames.push_back(std::move(older));

    QueuedFrame newer;
    newer.timestamp = 160;
    frames.push_back(std::move(newer));

    const size_t bestIndex = SelectFrameClosestToTimestamp(frames, frames.size(), 150);
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToTimestampIfSkipsRejectedFrames) {
    std::deque<QueuedFrame> frames;

    QueuedFrame blocked;
    blocked.timestamp = 150;
    blocked.frameIndex = 1;
    frames.push_back(std::move(blocked));

    QueuedFrame allowed;
    allowed.timestamp = 162;
    allowed.frameIndex = 2;
    frames.push_back(std::move(allowed));

    const size_t bestIndex =
        SelectFrameClosestToTimestampIf(frames, frames.size(), 160,
                                        [](const QueuedFrame& frame) { return frame.frameIndex == 2; });
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToTimestampIfReturnsAvailableCountWhenNoFramesMatch) {
    std::deque<QueuedFrame> frames;

    QueuedFrame frame;
    frame.timestamp = 150;
    frame.frameIndex = 3;
    frames.push_back(std::move(frame));

    const size_t bestIndex =
        SelectFrameClosestToTimestampIf(frames, frames.size(), 160, [](const QueuedFrame&) { return false; });
    EXPECT_EQ(bestIndex, frames.size());
}

TEST(FrameTimingUtilsTest, FindPreviousFrameIndexIfFindsNearestEarlierMatch) {
    std::deque<QueuedFrame> frames;

    QueuedFrame first;
    first.frameIndex = 1;
    frames.push_back(std::move(first));

    QueuedFrame second;
    second.frameIndex = 2;
    frames.push_back(std::move(second));

    QueuedFrame third;
    third.frameIndex = 3;
    frames.push_back(std::move(third));

    const size_t bestIndex =
        FindPreviousFrameIndexIf(frames, 3, [](const QueuedFrame& frame) { return frame.frameIndex != 2; });
    EXPECT_EQ(bestIndex, 2u);
}

TEST(FrameTimingUtilsTest, FindPreviousFrameIndexIfReturnsSizeWhenNoEarlierMatchExists) {
    std::deque<QueuedFrame> frames;

    QueuedFrame only;
    only.frameIndex = 7;
    frames.push_back(std::move(only));

    const size_t bestIndex =
        FindPreviousFrameIndexIf(frames, 1, [](const QueuedFrame& frame) { return frame.frameIndex == 8; });
    EXPECT_EQ(bestIndex, frames.size());
}

TEST(FrameTimingUtilsTest, FindNextFrameIndexIfFindsNearestLaterMatch) {
    std::deque<QueuedFrame> frames;

    QueuedFrame first;
    first.frameIndex = 1;
    frames.push_back(std::move(first));

    QueuedFrame second;
    second.frameIndex = 2;
    frames.push_back(std::move(second));

    QueuedFrame third;
    third.frameIndex = 3;
    frames.push_back(std::move(third));

    const size_t bestIndex =
        FindNextFrameIndexIf(frames, 1, [](const QueuedFrame& frame) { return frame.frameIndex != 2; });
    EXPECT_EQ(bestIndex, 2u);
}

TEST(FrameTimingUtilsTest, FindNextFrameIndexIfReturnsSizeWhenNoLaterMatchExists) {
    std::deque<QueuedFrame> frames;

    QueuedFrame only;
    only.frameIndex = 7;
    frames.push_back(std::move(only));

    const size_t bestIndex =
        FindNextFrameIndexIf(frames, 1, [](const QueuedFrame& frame) { return frame.frameIndex == 8; });
    EXPECT_EQ(bestIndex, frames.size());
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridBreaksTiesTowardNewerFrame) {
    std::deque<QueuedFrame> frames;

    QueuedFrame older;
    older.timestamp = 140;
    frames.push_back(std::move(older));

    QueuedFrame newer;
    newer.timestamp = 160;
    frames.push_back(std::move(newer));

    const size_t bestIndex = SelectFrameClosestToGrid(frames, frames.size(), 100, 2, 50);
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridIfSkipsRejectedFrames) {
    std::deque<QueuedFrame> frames;

    QueuedFrame blocked;
    blocked.timestamp = 150;
    blocked.frameIndex = 1;
    frames.push_back(std::move(blocked));

    QueuedFrame allowed;
    allowed.timestamp = 162;
    allowed.frameIndex = 2;
    frames.push_back(std::move(allowed));

    const size_t bestIndex = SelectFrameClosestToGridIf(frames, frames.size(), 100, 1, 50,
                                                        [](const QueuedFrame& frame) { return frame.frameIndex == 2; });
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridIfReturnsAvailableCountWhenNoFramesMatch) {
    std::deque<QueuedFrame> frames;

    QueuedFrame frame;
    frame.timestamp = 150;
    frame.frameIndex = 3;
    frames.push_back(std::move(frame));

    const size_t bestIndex =
        SelectFrameClosestToGridIf(frames, frames.size(), 100, 1, 50,
                                   [](const QueuedFrame&) { return false; });
    EXPECT_EQ(bestIndex, frames.size());
}

TEST(FrameTimingUtilsTest, ComputeIdealOutputQpcUsesPreviousGridTick) {
    EXPECT_EQ(ComputeIdealOutputQpc(100, 1, 50), 100);
    EXPECT_EQ(ComputeIdealOutputQpc(100, 3, 50), 200);
}

TEST(FrameTimingUtilsTest, ComputeWgcSelectionTargetPrefersScheduledSampleQpc) {
    EXPECT_EQ(ComputeWgcSelectionTargetQpc(250, 100, 3, 50), 250);
    EXPECT_EQ(ComputeWgcSelectionTargetQpc(0, 100, 3, 50), 200);
}

TEST(FrameTimingUtilsTest, ComputeWgcSelectionTargetUsesGridWhenScheduledSampleMissing) {
    EXPECT_EQ(ComputeWgcSelectionTargetQpc(0, 1200, 1, 100), 1200);
    EXPECT_EQ(ComputeWgcSelectionTargetQpc(0, 1200, 4, 100), 1500);
}

TEST(FrameTimingUtilsTest, ComputeCfrFrameIndexUsesElapsedTimeFromRecordingStart) {
    EXPECT_EQ(ComputeCfrFrameIndexForElapsedUs(0, 60, -1), 0);
    EXPECT_EQ(ComputeCfrFrameIndexForElapsedUs(16667, 60, 0), 1);
    EXPECT_EQ(ComputeCfrFrameIndexForElapsedUs(33333, 60, 1), 2);
}

TEST(FrameTimingUtilsTest, ComputeCfrFrameIndexRemainsMonotonicAcrossRepeatedTimestamps) {
    EXPECT_EQ(ComputeCfrFrameIndexForElapsedUs(50000, 60, 3), 4);
    EXPECT_EQ(ComputeCfrFrameIndexForElapsedUs(50000, 60, 4), 5);
}

TEST(FrameTimingUtilsTest, ComputeNextCfrFrameIndexAdvancesSequentially) {
    EXPECT_EQ(ComputeNextCfrFrameIndex(-1), 0);
    EXPECT_EQ(ComputeNextCfrFrameIndex(0), 1);
    EXPECT_EQ(ComputeNextCfrFrameIndex(119), 120);
}

TEST(FrameTimingUtilsTest, ResolveCfrTimelineElapsedUsPrefersExplicitTimelineOverLateWallClock) {
    EXPECT_EQ(ResolveCfrTimelineElapsedUs(120000, 100000, 83333), 100000);
}

TEST(FrameTimingUtilsTest, ResolveCfrTimelineElapsedUsFallsBackToSteadyClockWhenNoOverrideExists) {
    EXPECT_EQ(ResolveCfrTimelineElapsedUs(120000, -1, 83333), 120000);
}

TEST(FrameTimingUtilsTest, ResolveCfrTimelineElapsedUsStaysMonotonicWhenOverrideRegresses) {
    EXPECT_EQ(ResolveCfrTimelineElapsedUs(120000, 90000, 100000), 100000);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridUsesPreviousGridTickPhase) {
    std::deque<QueuedFrame> frames;

    QueuedFrame older;
    older.timestamp = 100;
    frames.push_back(std::move(older));

    QueuedFrame newer;
    newer.timestamp = 150;
    frames.push_back(std::move(newer));

    const size_t bestIndex = SelectFrameClosestToGrid(frames, frames.size(), 100, 1, 60);
    EXPECT_EQ(bestIndex, 0u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridBreaksTiesTowardNewerWithPreviousPhase) {
    std::deque<QueuedFrame> frames;

    QueuedFrame older;
    older.timestamp = 140;
    frames.push_back(std::move(older));

    QueuedFrame newer;
    newer.timestamp = 160;
    frames.push_back(std::move(newer));

    const size_t bestIndex = SelectFrameClosestToGrid(frames, frames.size(), 100, 2, 50);
    EXPECT_EQ(bestIndex, 1u);
}

TEST(FrameTimingUtilsTest, ShouldHoldFrameForNextTickWhenFrameIsMuchCloserToNextGrid) {
    const int64_t idealQpc = ComputeIdealOutputQpc(100, 3, 50);  // 200
    EXPECT_TRUE(ShouldHoldFrameForNextTick(248, idealQpc, 50, 2));
    EXPECT_FALSE(ShouldHoldFrameForNextTick(224, idealQpc, 50, 2));
}

TEST(FrameTimingUtilsTest, ComputeSourceDrivenElapsedUsUsesSourceClockWhenMonotonic) {
    SourceTimelineState state;

    EXPECT_EQ(ComputeSourceDrivenElapsedUs(1000, 5000, 0, state), 0);
    EXPECT_EQ(state.startSourceQpc, 5000);
    EXPECT_EQ(ComputeSourceDrivenElapsedUs(1000, 5042, 1300, state), 42000);
    EXPECT_EQ(state.lastElapsedUs, 42000);
}

TEST(FrameTimingUtilsTest, ComputeSourceDrivenElapsedUsFallsBackToSteadyClockForReplays) {
    SourceTimelineState state;

    EXPECT_EQ(ComputeSourceDrivenElapsedUs(1000, 7000, 0, state), 0);
    EXPECT_EQ(ComputeSourceDrivenElapsedUs(1000, 7010, 1500, state), 10000);
    EXPECT_EQ(ComputeSourceDrivenElapsedUs(1000, 7010, 26000, state), 26000);
    EXPECT_EQ(state.lastElapsedUs, 26000);
}

TEST(FrameTimingUtilsTest, QueuedFrameMovePreservesCaptureOrigin) {
    QueuedFrame input;
    input.captureLeft = 321;
    input.captureTop = 654;
    input.frameIndex = 77;
    input.textureIndex = 4;
    input.enqueueQpc = 12345;
    input.deferCount = 2;
    input.rawTimestamp = 54321;
    input.duplicateSourceTimestamp = true;

    QueuedFrame moved(std::move(input));
    EXPECT_EQ(moved.captureLeft, 321);
    EXPECT_EQ(moved.captureTop, 654);
    EXPECT_EQ(moved.frameIndex, 77u);
    EXPECT_EQ(moved.textureIndex, 4);
    EXPECT_EQ(moved.enqueueQpc, 12345);
    EXPECT_EQ(moved.deferCount, 2u);
    EXPECT_EQ(moved.rawTimestamp, 54321);
    EXPECT_TRUE(moved.duplicateSourceTimestamp);
    EXPECT_EQ(input.captureLeft, 0);
    EXPECT_EQ(input.captureTop, 0);
    EXPECT_EQ(input.frameIndex, 0u);
    EXPECT_EQ(input.textureIndex, -1);
    EXPECT_EQ(input.enqueueQpc, 0);
    EXPECT_EQ(input.deferCount, 0u);
    EXPECT_EQ(input.rawTimestamp, 0);
    EXPECT_FALSE(input.duplicateSourceTimestamp);
}
