#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "mediaengine/video/face_camera_history.h"

// The face camera shows the frame taken at its output slot's content time
// (face_camera_history.h), so it stays in sync with the delayed screen and the
// microphone instead of leading them by the WGC/DXGI smoothness delay.

namespace fc = ce::face_camera;

namespace {

constexpr int64_t kQpcPerSecond = 10000000;
constexpr int64_t kCameraInterval = kQpcPerSecond / 30;

}  // namespace

TEST(FaceCameraHistoryTest, PicksTheNewestFrameTakenAtOrBeforeTheSlot) {
    // Ring order is not capture order once the slot index wraps.
    const std::vector<int64_t> captured = {1000 + 3 * kCameraInterval, 1000, 1000 + kCameraInterval,
                                           1000 + 2 * kCameraInterval};
    auto selection = fc::SelectHistoryFrame(captured, 1000 + kCameraInterval + kCameraInterval / 2, kCameraInterval);
    EXPECT_EQ(selection.index, 2u);
    EXPECT_FALSE(selection.targetOlderThanHistory);

    selection = fc::SelectHistoryFrame(captured, 1000 + 2 * kCameraInterval, kCameraInterval);
    EXPECT_EQ(selection.index, 3u);  // exactly at the slot time
    selection = fc::SelectHistoryFrame(captured, 1000 + 10 * kCameraInterval, kCameraInterval);
    EXPECT_EQ(selection.index, 0u);  // live edge: the newest frame
}

TEST(FaceCameraHistoryTest, UnknownSlotTimeShowsTheNewestFrame) {
    const std::vector<int64_t> captured = {500, 900, 0, 700};
    EXPECT_EQ(fc::SelectHistoryFrame(captured, 0, kCameraInterval).index, 1u);
    EXPECT_EQ(fc::SelectHistoryFrame({0, 0}, 1234, kCameraInterval).index, SIZE_MAX);
}

TEST(FaceCameraHistoryTest, ATooShortHistoryShowsItsOldestFrameAndAsksToGrow) {
    // 300 ms content delay at 30 fps with only four frames retained.
    const int64_t newest = 1000 * kQpcPerSecond;
    const std::vector<int64_t> captured = {newest - 3 * kCameraInterval, newest - 2 * kCameraInterval,
                                           newest - kCameraInterval, newest};
    const int64_t slot = newest - (3 * kQpcPerSecond) / 10;
    const auto selection = fc::SelectHistoryFrame(captured, slot, kCameraInterval);
    EXPECT_EQ(selection.index, 0u);  // never a frame from the slot's future beyond the oldest
    EXPECT_TRUE(selection.targetOlderThanHistory);
    EXPECT_EQ(selection.wantedFrames, 11u);  // 9 intervals of lag + 2

    // A ring that is not yet full is still filling; it does not ask to grow.
    const std::vector<int64_t> filling = {newest, 0, 0, 0};
    EXPECT_EQ(fc::SelectHistoryFrame(filling, slot, kCameraInterval).wantedFrames, 0u);

    // The growth request is bounded.
    const auto capped = fc::SelectHistoryFrame(captured, newest - 100 * kQpcPerSecond, kCameraInterval);
    EXPECT_EQ(capped.wantedFrames, fc::kHistoryMaxFrames);
}

TEST(FaceCameraHistoryTest, CaptureTimeUsesAPlausibleDeviceTimestamp) {
    const int64_t frequency = 10000000;  // 100 ns ticks: device time maps 1:1
    int64_t captureQpc = 0;
    EXPECT_TRUE(fc::ResolveCaptureQpc(5000000, 5400000, frequency, &captureQpc));
    EXPECT_EQ(captureQpc, 5000000);

    // Other QPC frequencies convert without overflow.
    EXPECT_TRUE(fc::ResolveCaptureQpc(int64_t{36000} * 10000000 + 5000000, int64_t{36000} * 3000000 + 1600000, 3000000,
                                      &captureQpc));
    EXPECT_EQ(captureQpc, int64_t{36000} * 3000000 + 1500000);

    // Missing, future or stale (> 1 s) device times fall back to arrival.
    EXPECT_FALSE(fc::ResolveCaptureQpc(0, 5400000, frequency, &captureQpc));
    EXPECT_EQ(captureQpc, 5400000);
    EXPECT_FALSE(fc::ResolveCaptureQpc(5500000, 5400000, frequency, &captureQpc));
    EXPECT_EQ(captureQpc, 5400000);
    EXPECT_FALSE(fc::ResolveCaptureQpc(1000, 5400000 + frequency, frequency, &captureQpc));
    EXPECT_EQ(captureQpc, 5400000 + frequency);
}

TEST(FaceCameraHistoryTest, LagSummaryTracksMinMaxAverage) {
    fc::LagSummary summary;
    summary.Add(10000, false);
    summary.Add(30000, true);
    EXPECT_EQ(summary.minUs, 10000);
    EXPECT_EQ(summary.maxUs, 30000);
    EXPECT_EQ(summary.AverageUs(), 20000);
    EXPECT_EQ(summary.olderThanHistory, 1u);
}
