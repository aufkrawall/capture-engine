#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/capture/frame_timing_utils.h"

namespace {

constexpr int64_t kQpcFreq = 1000000;
constexpr int64_t kOutputInterval = 8333;

void CheckRateChange(int64_t firstInterval, int64_t nextInterval, size_t rampFrames,
                     int64_t origin = 1000000, int64_t qpcFreq = kQpcFreq) {
    InputFrameRatePredictor predictor;
    const int64_t outputInterval = qpcFreq / 120;
    const size_t changeFrame = 128;
    const size_t settledFrame = changeFrame + rampFrames + 128;
    const size_t totalFrames = settledFrame + 128;
    int64_t raw = origin;
    int64_t previous = 0;
    int64_t settledMaxError = 0;
    for (size_t i = 0; i < totalFrames; ++i) {
        int64_t interval = i < changeFrame ? firstInterval : nextInterval;
        if (i >= changeFrame && i < changeFrame + rampFrames) {
            interval = firstInterval + (nextInterval - firstInterval) *
                                           static_cast<int64_t>(i - changeFrame + 1) /
                                           static_cast<int64_t>(rampFrames);
        }
        raw += interval;
        predictor.Update(raw, qpcFreq);
        const int64_t selected = predictor.SmoothMonotonicTimestamp(raw, outputInterval);
        ASSERT_GT(selected, previous) << "frame " << i;
        EXPECT_LE(AbsoluteTimestampDistance(selected, raw), predictor.GetSmoothingMaxDeviationQpc(outputInterval))
            << "frame " << i;
        previous = selected;
        if (i >= settledFrame) {
            settledMaxError = std::max(settledMaxError, AbsoluteTimestampDistance(selected, raw));
        }
    }
    // A steady source after a rate change has no noise to smooth. Its old interval
    // must not survive as a permanent content-time offset (previously 2.8-4.2 ms).
    EXPECT_LE(settledMaxError, 1) << "interval " << firstInterval << " -> " << nextInterval;
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
}

}  // namespace

TEST(FrameTimestampSmoothingTest, RateChangesDoNotLeavePermanentPhaseOffsets) {
    for (const auto& intervals : {std::pair<int64_t, int64_t>{11200, 13000}, {13000, 11200},
                                  {7143, 8333}, {8333, 7143}}) {
        CheckRateChange(intervals.first, intervals.second, 0);
    }
}

TEST(FrameTimestampSmoothingTest, GradualRateChangesDoNotLeavePermanentPhaseOffsets) {
    CheckRateChange(11200, 13000, 256);
    CheckRateChange(13000, 11200, 256);
}

TEST(FrameTimestampSmoothingTest, PhaseFitPreservesPrecisionAtLargeQpcValues) {
    // Beyond double's exact-integer range: fit relative timestamps, not absolute QPC.
    const int64_t origin = (int64_t{1} << 55) + 123;
    CheckRateChange(112000, 130000, 0, origin, 10000000);
    CheckRateChange(130000, 112000, 256, origin, 10000000);
}

TEST(FrameTimestampSmoothingTest, QuantizedSurplusSourcesKeepEveryGapInsideOneOutputInterval) {
    for (const double fps : {135.0, 140.0, 144.0}) {
        InputFrameRatePredictor predictor;
        int64_t previous = 0;
        int64_t largestGap = 0;
        for (size_t i = 0; i < 1200; ++i) {
            const int64_t ideal = 1000000 + static_cast<int64_t>(static_cast<double>(i) * 1000000.0 / fps);
            const int64_t raw = ((ideal + 4999) / 5000) * 5000;
            predictor.Update(raw, kQpcFreq);
            const int64_t selected = predictor.SmoothMonotonicTimestamp(raw, kOutputInterval);
            ASSERT_GT(selected, previous) << "fps " << fps << " frame " << i;
            EXPECT_LE(AbsoluteTimestampDistance(selected, raw), predictor.GetSmoothingMaxDeviationQpc(kOutputInterval));
            if (i >= 64) {
                largestGap = std::max(largestGap, selected - previous);
            }
            previous = selected;
        }
        EXPECT_LE(largestGap, kOutputInterval) << "fps " << fps;
        EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
    }
}

TEST(FrameTimestampSmoothingTest, ARealStallClearsTheOldPhaseHistory) {
    InputFrameRatePredictor predictor;
    int64_t raw = 1000000;
    for (int i = 0; i < 96; ++i) {
        raw += i < 48 ? 11200 : 13000;
        predictor.Update(raw, kQpcFreq);
        predictor.SmoothMonotonicTimestamp(raw, kOutputInterval);
    }
    raw += 100000;
    predictor.Update(raw, kQpcFreq);
    EXPECT_EQ(predictor.SmoothMonotonicTimestamp(raw, kOutputInterval), raw);
    EXPECT_EQ(predictor.SmoothingSnapCount(), 1u);
    for (int i = 0; i < 64; ++i) {
        raw += 13000;
        predictor.Update(raw, kQpcFreq);
        EXPECT_LE(AbsoluteTimestampDistance(predictor.SmoothMonotonicTimestamp(raw, kOutputInterval), raw), 1)
            << "post-stall frame " << i;
    }
}

TEST(FrameTimestampSmoothingTest, ResetDiscardsAChangedSourcePhase) {
    InputFrameRatePredictor predictor;
    int64_t raw = 1000000;
    for (int i = 0; i < 96; ++i) {
        raw += i < 48 ? 11200 : 13000;
        predictor.Update(raw, kQpcFreq);
        predictor.SmoothMonotonicTimestamp(raw, kOutputInterval);
    }
    predictor.Reset();
    raw = 500000;
    for (int i = 0; i < 64; ++i) {
        raw += 7143;
        predictor.Update(raw, kQpcFreq);
        EXPECT_LE(AbsoluteTimestampDistance(predictor.SmoothMonotonicTimestamp(raw, kOutputInterval), raw), 1);
    }
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
}

TEST(FrameTimestampSmoothingTest, DuplicateTimestampsDoNotAdvanceTheFittedSourceOrdinal) {
    InputFrameRatePredictor predictor;
    int64_t raw = 1000000;
    int64_t previous = 0;
    for (int i = 0; i < 256; ++i) {
        if (i % 7 != 0) {
            raw += 7143;
        }
        predictor.Update(raw, kQpcFreq);
        const int64_t selected = predictor.SmoothMonotonicTimestamp(raw, kOutputInterval);
        EXPECT_GT(selected, previous);
        EXPECT_LE(AbsoluteTimestampDistance(selected, raw), predictor.GetSmoothingMaxDeviationQpc(kOutputInterval));
        if (i >= 64 && i % 7 != 0) {
            EXPECT_LE(AbsoluteTimestampDistance(selected, raw), 1);
        }
        previous = selected;
    }
}

TEST(FrameTimestampSmoothingTest, LongThinnedSourceRateChangesRemainAnchored) {
    InputFrameRatePredictor predictor;
    int64_t raw = (int64_t{1} << 55) + 123;
    int64_t previous = 0;
    int64_t settledMaxError = 0;
    for (size_t i = 0; i < 300000; ++i) {
        const uint32_t span = 1u + static_cast<uint32_t>(i % 4);
        raw += (i < 150000 ? 112000 : 130000) * static_cast<int64_t>(span);
        predictor.Update(raw, 10000000, span);
        const int64_t selected = predictor.SmoothMonotonicTimestamp(raw, 83333, span);
        ASSERT_GT(selected, previous) << "frame " << i;
        EXPECT_LE(AbsoluteTimestampDistance(selected, raw), predictor.GetSmoothingMaxDeviationQpc(83333));
        if (i >= 150128) {
            settledMaxError = std::max(settledMaxError, AbsoluteTimestampDistance(selected, raw));
        }
        previous = selected;
    }
    EXPECT_LE(settledMaxError, 1);
    EXPECT_EQ(predictor.SmoothingSnapCount(), 0u);
    EXPECT_NEAR(predictor.GetPredictedFps(10000000), 10000000.0 / 130000.0, 0.001);
}
