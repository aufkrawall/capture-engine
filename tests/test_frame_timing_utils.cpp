#include <gtest/gtest.h>
#include <deque>
#include <utility>
#include <vector>
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

    const size_t bestIndex = SelectFrameClosestToTimestampIf(
        frames, frames.size(), 160, [](const QueuedFrame& frame) { return frame.frameIndex == 2; });
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

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridIfPrefersReadyFrameNearGrid) {
    std::deque<QueuedFrame> frames;

    QueuedFrame olderReady;
    olderReady.timestamp = 190;
    olderReady.frameIndex = 1;
    frames.push_back(std::move(olderReady));

    QueuedFrame nearestNotReady;
    nearestNotReady.timestamp = 202;
    nearestNotReady.frameIndex = 2;
    frames.push_back(std::move(nearestNotReady));

    QueuedFrame newerReady;
    newerReady.timestamp = 225;
    newerReady.frameIndex = 3;
    frames.push_back(std::move(newerReady));

    const size_t bestIndex = SelectFrameClosestToGridIf(frames, frames.size(), 100, 3, 50,
                                                        [](const QueuedFrame& frame) {
                                                            return frame.frameIndex != 2;
                                                        });
    EXPECT_EQ(bestIndex, 0u);
}

TEST(FrameTimingUtilsTest, SelectFrameClosestToGridIfReturnsAvailableCountWhenNoFramesMatch) {
    std::deque<QueuedFrame> frames;

    QueuedFrame frame;
    frame.timestamp = 150;
    frame.frameIndex = 3;
    frames.push_back(std::move(frame));

    const size_t bestIndex =
        SelectFrameClosestToGridIf(frames, frames.size(), 100, 1, 50, [](const QueuedFrame&) { return false; });
    EXPECT_EQ(bestIndex, frames.size());
}

TEST(FrameTimingUtilsTest, ComputeIdealOutputQpcUsesPreviousGridTick) {
    EXPECT_EQ(ComputeIdealOutputQpc(100, 1, 50), 100);
    EXPECT_EQ(ComputeIdealOutputQpc(100, 3, 50), 200);
}

TEST(FrameTimingUtilsTest, ComputeDelayedContentGridStartShiftsInjectSelectionTarget) {
    const int64_t liveGridStartQpc = 1000;
    const int64_t delayedGridStartQpc = ComputeDelayedContentGridStartQpc(liveGridStartQpc, 150);
    EXPECT_EQ(delayedGridStartQpc, 850);
    EXPECT_EQ(ComputeIdealOutputQpc(delayedGridStartQpc, 3, 50), 950);

    std::deque<QueuedFrame> frames;
    QueuedFrame delayedCandidate;
    delayedCandidate.timestamp = 948;
    frames.push_back(std::move(delayedCandidate));

    QueuedFrame liveCandidate;
    liveCandidate.timestamp = 1098;
    frames.push_back(std::move(liveCandidate));

    EXPECT_EQ(SelectFrameClosestToGrid(frames, frames.size(), liveGridStartQpc, 3, 50), 1u);
    EXPECT_EQ(SelectFrameClosestToGrid(frames, frames.size(), delayedGridStartQpc, 3, 50), 0u);
}

TEST(FrameTimingUtilsTest, ComputeDelayedContentGridStartIgnoresInactiveDelay) {
    EXPECT_EQ(ComputeDelayedContentGridStartQpc(1000, 0), 1000);
    EXPECT_EQ(ComputeDelayedContentGridStartQpc(1000, -10), 1000);
    EXPECT_EQ(ComputeDelayedContentGridStartQpc(0, 100), 0);
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

TEST(FrameTimingUtilsTest, ResolveAuthoritativeCfrTimelineElapsedUsTrustsExplicitTimelineOverInflatedHistory) {
    EXPECT_EQ(ResolveAuthoritativeCfrTimelineElapsedUs(120000, 100000, 140000), 100000);
}

TEST(FrameTimingUtilsTest, ResolveAuthoritativeCfrTimelineElapsedUsReusesPriorTimelineWhenOverrideMissing) {
    EXPECT_EQ(ResolveAuthoritativeCfrTimelineElapsedUs(140000, -1, 100000), 100000);
}

TEST(FrameTimingUtilsTest, ResolveAuthoritativeCfrTimelineElapsedUsFallsBackToSteadyClockOnFirstFrame) {
    EXPECT_EQ(ResolveAuthoritativeCfrTimelineElapsedUs(120000, -1, 0), 120000);
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

TEST(FrameTimingUtilsTest, InputFrameRatePredictorCalibratesAndTracksStableCadence) {
    InputFrameRatePredictor predictor;
    constexpr int64_t kQpcFreq = 60000;
    constexpr int64_t kFrameInterval = 1000;

    EXPECT_EQ(predictor.Update(10000, kQpcFreq), 0);
    EXPECT_EQ(predictor.Update(11000, kQpcFreq), kFrameInterval);
    EXPECT_EQ(predictor.Update(12000, kQpcFreq), kFrameInterval);
    EXPECT_EQ(predictor.Update(13000, kQpcFreq), kFrameInterval);
    EXPECT_FALSE(predictor.IsCalibrated());
    EXPECT_EQ(predictor.Update(14000, kQpcFreq), kFrameInterval);
    EXPECT_TRUE(predictor.IsCalibrated());
    EXPECT_NEAR(predictor.GetPredictedFps(kQpcFreq), 60.0, 0.1);
    EXPECT_NEAR(predictor.GetJitterUs(kQpcFreq), 0.0, 0.1);
    // Deviation budget: 3/4 of the source interval, tightened by 3/4 of the output interval.
    EXPECT_EQ(predictor.GetSmoothingMaxDeviationQpc(0), 750);
    EXPECT_EQ(predictor.GetSmoothingMaxDeviationQpc(2000), 750);
    EXPECT_EQ(predictor.GetSmoothingMaxDeviationQpc(800), 600);
}

TEST(FrameTimingUtilsTest, InputFrameRatePredictorCountsDuplicateTimestampsTowardCalibration) {
    InputFrameRatePredictor predictor;
    constexpr int64_t kQpcFreq = 60000;

    EXPECT_EQ(predictor.Update(10000, kQpcFreq), 0);
    EXPECT_EQ(predictor.Update(11000, kQpcFreq), 1000);
    EXPECT_EQ(predictor.Update(11000, kQpcFreq), 1000);
    EXPECT_EQ(predictor.Update(11000, kQpcFreq), 1000);
    EXPECT_EQ(predictor.Update(11000, kQpcFreq), 1000);
    EXPECT_TRUE(predictor.IsCalibrated());
    EXPECT_NEAR(predictor.GetPredictedFps(kQpcFreq), 60.0, 0.1);
}

TEST(FrameTimingUtilsTest, InputFrameRatePredictorResetOnTimestampRegressionDropsCalibration) {
    InputFrameRatePredictor predictor;
    constexpr int64_t kQpcFreq = 60000;

    predictor.Update(10000, kQpcFreq);
    predictor.Update(11000, kQpcFreq);
    predictor.Update(12000, kQpcFreq);
    predictor.Update(13000, kQpcFreq);
    predictor.Update(14000, kQpcFreq);
    ASSERT_TRUE(predictor.IsCalibrated());
    const double previousFps = predictor.GetPredictedFps(kQpcFreq);

    EXPECT_EQ(predictor.Update(9000, kQpcFreq), 0);
    EXPECT_FALSE(predictor.IsCalibrated());
    EXPECT_DOUBLE_EQ(predictor.GetPredictedFps(kQpcFreq), previousFps);
}

TEST(FrameTimingUtilsTest, InputFrameRatePredictorTracksJitterAndZeroFrequencySafely) {
    InputFrameRatePredictor predictor;
    constexpr int64_t kQpcFreq = 1000000;

    EXPECT_EQ(predictor.Update(100000, kQpcFreq), 0);
    EXPECT_EQ(predictor.Update(116000, kQpcFreq), 16000);
    EXPECT_EQ(predictor.Update(133000, kQpcFreq), 17000);
    EXPECT_EQ(predictor.Update(149000, kQpcFreq), 16400);

    EXPECT_GT(predictor.GetJitterUs(kQpcFreq), 0.0);
    EXPECT_GT(predictor.SmoothedIntervalQpc(), 15000.0);
    EXPECT_EQ(predictor.Update(200000, 0), 0);
    EXPECT_DOUBLE_EQ(predictor.GetPredictedFps(0), 0.0);
    EXPECT_DOUBLE_EQ(predictor.GetJitterUs(0), 0.0);
}

TEST(FrameTimingUtilsTest, InjectDelayedGridSelectionPinsRealizedDelayUnderAbundantBuffer) {
    // The inject video path sees the full smooth present stream, so its buffer holds many frames
    // spanning a wide timestamp range. Selecting the frame nearest the DELAYED content grid
    // (gridStart - contentDelay) pins the realized content delay near contentDelay regardless of how
    // deep the buffer is. This is what structurally protects inject from the oldest-first realized-
    // delay rubber-band that affected the WGC active-delay path before the nearest-target playout fix:
    // emitting the OLDEST buffered frame here would yield a realized delay of ~2x contentDelay, while
    // nearest-grid selection yields exactly contentDelay.
    const int64_t interval = 100;      // qpc ticks per frame
    const int64_t contentDelay = 400;  // 4 frames
    const int64_t gridStart = 1000000;
    const int64_t injectGridStart = ComputeDelayedContentGridStartQpc(gridStart, contentDelay);
    EXPECT_EQ(injectGridStart, gridStart - contentDelay);

    for (int64_t tick = 1; tick <= 50; ++tick) {
        const int64_t nowQpc = gridStart + (tick - 1) * interval;  // scheduled emit time for this tick
        std::deque<QueuedFrame> frames;                            // abundant buffer up to "now"
        for (int64_t ts = gridStart - 2 * contentDelay; ts <= nowQpc; ts += interval) {
            QueuedFrame f;
            f.timestamp = ts;
            f.selectionTimestamp = ts;
            frames.push_back(std::move(f));
        }
        const size_t best = SelectFrameClosestToGridIf(frames, frames.size(), injectGridStart, tick, interval,
                                                       [](const QueuedFrame&) { return true; });
        ASSERT_LT(best, frames.size());
        const int64_t realizedDelay = nowQpc - frames[best].timestamp;
        // Pinned within half a frame of the configured content delay (never the oldest-frame age,
        // which here would be ~2x contentDelay and grow with buffer depth).
        EXPECT_GE(realizedDelay, contentDelay - interval / 2);
        EXPECT_LE(realizedDelay, contentDelay + interval / 2);
        const int64_t oldestFrameDelay = nowQpc - frames.front().timestamp;
        EXPECT_GT(oldestFrameDelay, realizedDelay);  // the bug behaviour we are guarding against
    }
}

// --- Monotonic bounded-deviation selection-timestamp smoothing (fortistutter fix) ---------------
//
// WGC/DXGI source timestamps are DWM composition times: under VRR / composed presentation a
// perfectly smooth game present stream arrives QUANTIZED to the composition clock. The fortistutter
// session (140 fps source into 120 fps CFR, buffer 31-35 frames deep the whole time) showed 22%
// CFR repeats because the uniform playout was slaved to those raw quantized timestamps: recurring
// artificial >8.3 ms holes at the fixed-latency read boundary each forced a hold while the
// surrounding surplus was dropped. These tests pin the smoother contract that removes that noise.

namespace {

// True 140 fps cadence (7142.86 us) quantized UP to a 5 ms composition clock: raw intervals mix
// 5000/10000 us around the same average. The 10 ms raw gaps are what starved CFR output slots.
std::vector<int64_t> MakeQuantizedCadence(int64_t startUs, size_t frames, double trueIntervalUs,
                                          int64_t bucketUs) {
    std::vector<int64_t> raw;
    raw.reserve(frames);
    for (size_t i = 0; i < frames; ++i) {
        const double trueTs = static_cast<double>(startUs) + trueIntervalUs * static_cast<double>(i);
        const int64_t quantized = ((static_cast<int64_t>(trueTs) + bucketUs - 1) / bucketUs) * bucketUs;
        if (!raw.empty() && quantized <= raw.back()) {
            raw.push_back(raw.back());  // compositor stamp collision (duplicate timestamp)
        } else {
            raw.push_back(quantized);
        }
    }
    return raw;
}

}  // namespace

TEST(FrameTimingUtilsTest, SmoothMonotonicTimestampRemovesCompositorQuantizationNoise) {
    constexpr int64_t kQpcFreq = 1000000;        // 1 tick == 1 us
    constexpr int64_t kOutputIntervalUs = 8333;  // 120 fps CFR
    constexpr double kTrueIntervalUs = 1000000.0 / 140.0;

    InputFrameRatePredictor predictor;
    const auto raw = MakeQuantizedCadence(1000000, 400, kTrueIntervalUs, 5000);

    std::vector<int64_t> smoothed;
    smoothed.reserve(raw.size());
    for (const int64_t ts : raw) {
        predictor.Update(ts, kQpcFreq);
        smoothed.push_back(predictor.SmoothMonotonicTimestamp(ts, kOutputIntervalUs));
    }

    // The raw quantized stream contains >= output-interval gaps (the CFR hold trigger)...
    int64_t rawGapMax = 0;
    for (size_t i = 1; i < raw.size(); ++i) {
        rawGapMax = std::max(rawGapMax, raw[i] - raw[i - 1]);
    }
    EXPECT_GE(rawGapMax, 10000);

    // ...but after calibration the smoothed stream must never starve an output slot: every smoothed
    // gap fits inside one CFR sweep step, so a surplus source can never force a repeat again.
    int64_t smoothedGapMax = 0;
    int64_t deviationMax = 0;
    const size_t warmup = 32;  // interval EMA convergence
    for (size_t i = 1; i < smoothed.size(); ++i) {
        EXPECT_GT(smoothed[i], smoothed[i - 1]) << "strict monotonicity violated at " << i;
        if (i >= warmup) {
            smoothedGapMax = std::max(smoothedGapMax, smoothed[i] - smoothed[i - 1]);
            deviationMax = std::max(deviationMax, AbsoluteTimestampDistance(smoothed[i], raw[i]));
        }
    }
    EXPECT_LE(smoothedGapMax, kOutputIntervalUs);
    // Bounded deviation: the content-time error added by smoothing never exceeds 3/4 of the output
    // interval (and is further bounded by 3/4 of the source interval), so raw-domain sync checks
    // and the active-delay hard cap keep their meaning.
    EXPECT_LE(deviationMax, (kOutputIntervalUs * 3) / 4);
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
}

TEST(FrameTimingUtilsTest, SmoothMonotonicTimestampKeepsRealStallsVisible) {
    constexpr int64_t kQpcFreq = 1000000;
    constexpr int64_t kOutputIntervalUs = 8333;
    constexpr int64_t kIntervalUs = 7143;

    InputFrameRatePredictor predictor;
    int64_t ts = 1000000;
    int64_t lastSmoothed = 0;
    for (int i = 0; i < 64; ++i) {
        predictor.Update(ts, kQpcFreq);
        lastSmoothed = predictor.SmoothMonotonicTimestamp(ts, kOutputIntervalUs);
        ts += kIntervalUs;
    }
    ASSERT_TRUE(predictor.IsCalibrated());
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);

    // A genuine 100 ms delivery stall must NOT be smeared into earlier content time: the smoother
    // relocks to the raw post-stall timestamp so the in-gap freeze stays an honest hold.
    const int64_t stallTs = ts + 100000;
    predictor.Update(stallTs, kQpcFreq);
    const int64_t postStall = predictor.SmoothMonotonicTimestamp(stallTs, kOutputIntervalUs);
    EXPECT_EQ(postStall, stallTs);
    EXPECT_EQ(predictor.SmoothingSnapCount(), 1u);
    EXPECT_GT(postStall, lastSmoothed);
}

TEST(FrameTimingUtilsTest, SmoothMonotonicTimestampStaysStrictlyMonotonicOnDuplicateRawTimestamps) {
    constexpr int64_t kQpcFreq = 1000000;
    constexpr int64_t kOutputIntervalUs = 8333;
    constexpr int64_t kIntervalUs = 7143;

    InputFrameRatePredictor predictor;
    int64_t ts = 500000;
    int64_t prevSmoothed = 0;
    for (int i = 0; i < 32; ++i) {
        predictor.Update(ts, kQpcFreq);
        const int64_t smoothed = predictor.SmoothMonotonicTimestamp(ts, kOutputIntervalUs);
        EXPECT_GT(smoothed, prevSmoothed);
        prevSmoothed = smoothed;
        ts += kIntervalUs;
    }

    // Duplicate raw timestamps (compositor stamp collision) still advance strictly and stay within
    // the deviation budget of the raw time.
    const int64_t dupTs = ts;
    for (int i = 0; i < 3; ++i) {
        predictor.Update(dupTs, kQpcFreq);
        const int64_t smoothed = predictor.SmoothMonotonicTimestamp(dupTs, kOutputIntervalUs);
        EXPECT_GT(smoothed, prevSmoothed);
        EXPECT_LE(AbsoluteTimestampDistance(smoothed, dupTs),
                  predictor.GetSmoothingMaxDeviationQpc(kOutputIntervalUs));
        prevSmoothed = smoothed;
    }
}

TEST(FrameTimingUtilsTest, SmoothMonotonicTimestampPassthroughBeforeCalibrationAndAfterReset) {
    constexpr int64_t kQpcFreq = 1000000;
    constexpr int64_t kOutputIntervalUs = 8333;

    InputFrameRatePredictor predictor;
    predictor.Update(1000, kQpcFreq);
    EXPECT_EQ(predictor.SmoothMonotonicTimestamp(1000, kOutputIntervalUs), 1000);
    predictor.Update(8000, kQpcFreq);
    EXPECT_EQ(predictor.SmoothMonotonicTimestamp(8000, kOutputIntervalUs), 8000);

    predictor.Reset();
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
    predictor.Update(50000, kQpcFreq);
    EXPECT_EQ(predictor.SmoothMonotonicTimestamp(50000, kOutputIntervalUs), 50000);
    EXPECT_EQ(predictor.SmoothMonotonicTimestamp(0, kOutputIntervalUs), 0);
}
