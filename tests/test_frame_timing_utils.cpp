#include <gtest/gtest.h>
#include <deque>
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

    QueuedFrame moved(std::move(input));
    EXPECT_EQ(moved.captureLeft, 321);
    EXPECT_EQ(moved.captureTop, 654);
    EXPECT_EQ(moved.frameIndex, 77u);
    EXPECT_EQ(moved.textureIndex, 4);
    EXPECT_EQ(moved.enqueueQpc, 12345);
    EXPECT_EQ(moved.deferCount, 2u);
    EXPECT_EQ(input.captureLeft, 0);
    EXPECT_EQ(input.captureTop, 0);
    EXPECT_EQ(input.frameIndex, 0u);
    EXPECT_EQ(input.textureIndex, -1);
    EXPECT_EQ(input.enqueueQpc, 0);
    EXPECT_EQ(input.deferCount, 0u);
}
