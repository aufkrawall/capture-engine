// Smooth Motion's rates on the overlay's FG row (hook/metrics/performance_metrics.h).

#include <gtest/gtest.h>

#include "hook/metrics/performance_metrics.h"

namespace {

// What the FG row shows comes from the screen. Witcher 3 with driver vsync on a 143 Hz panel: the interposer submitted
// 288 a second and the game ran at 144 (20260919_181510).
class SmoothMotionRatesTest : public ::testing::Test {
protected:
    void SetUp() override {
        timing_.Reset(4321, 0, DisplayTimingStatus::Starting);
        metrics_.ConsumeDisplayTiming(timing_, 49'000'000);
    }

    void PublishScreen(int64_t intervalUs, int samples) {
        for (int sample = 0; sample < samples; ++sample) {
            const int64_t presentUs = 50'000'000 + intervalUs * sample;
            nowUs_ = presentUs + 3'000;
            timing_.Publish(nowUs_, nowUs_ + 100, presentUs);
            metrics_.ConsumeDisplayTiming(timing_, nowUs_ + 200);
        }
    }

    PerformanceMetrics metrics_;
    SharedDisplayTiming timing_;
    int64_t nowUs_ = 0;
};

TEST_F(SmoothMotionRatesTest, TheOutputRateIsWhatTheScreenShows) {
    metrics_.SetFGMetrics(288.0f, 144.0f, 2, /*fgType=*/3);
    PublishScreen(6'944, 80);
    EXPECT_NEAR(metrics_.GetFGOutputFPS(), 144.0f, 0.5f);
    EXPECT_NEAR(metrics_.GetFGBaseFPS(), 144.0f, 0.5f) << "never above what reaches the screen";
    EXPECT_FLOAT_EQ(metrics_.GetReportedFGOutputFPS(), 288.0f);
}

TEST_F(SmoothMotionRatesTest, TheBaseRateStaysTheApplicationsWithoutVsync) {
    metrics_.SetFGMetrics(316.0f, 158.0f, 2, /*fgType=*/3);
    PublishScreen(3'165, 80);
    EXPECT_NEAR(metrics_.GetFGOutputFPS(), 316.0f, 1.0f);
    EXPECT_FLOAT_EQ(metrics_.GetFGBaseFPS(), 158.0f);
}

TEST_F(SmoothMotionRatesTest, TheInterposersFiguresStandInWithoutADisplayStream) {
    metrics_.SetFGMetrics(144.0f, 72.0f, 2, /*fgType=*/3);
    EXPECT_FLOAT_EQ(metrics_.GetFGOutputFPS(), 144.0f);
    EXPECT_FLOAT_EQ(metrics_.GetFGBaseFPS(), 72.0f);

    PublishScreen(6'944, 80);
    metrics_.ConsumeDisplayTiming(timing_, nowUs_ + 5'000'000);
    EXPECT_FLOAT_EQ(metrics_.GetFGOutputFPS(), 144.0f);
    EXPECT_FLOAT_EQ(metrics_.GetFGBaseFPS(), 72.0f);
}

}  // namespace
