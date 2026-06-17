#include <gtest/gtest.h>

#include <vector>

#include "../mediaengine/av_sync_calibration.h"

using namespace ce::audio;

namespace {
CalibrationVideoFrame Frame(uint64_t qpc, double luma) {
    CalibrationVideoFrame f;
    f.qpc100ns = qpc;
    f.luma = luma;
    return f;
}
}  // namespace

TEST(AvSyncCalibrationTest, DetectsRisingEdgesWithHysteresis) {
    // dark, dark, BRIGHT(edge), bright, dark, dark, BRIGHT(edge), dark
    std::vector<CalibrationVideoFrame> frames = {
        Frame(100, 0.02), Frame(200, 0.05), Frame(300, 0.95), Frame(400, 0.90),
        Frame(500, 0.04), Frame(600, 0.03), Frame(700, 0.97), Frame(800, 0.01),
    };
    std::vector<uint64_t> edges = DetectFlashRisingEdges(frames);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0], 300u);
    EXPECT_EQ(edges[1], 700u);
}

TEST(AvSyncCalibrationTest, IgnoresMidLevelNoiseBetweenThresholds) {
    // Luma wobbles in the hysteresis band (0.3..0.6) but never crosses -> no edges.
    std::vector<CalibrationVideoFrame> frames = {
        Frame(100, 0.35),
        Frame(200, 0.55),
        Frame(300, 0.40),
        Frame(400, 0.50),
    };
    EXPECT_TRUE(DetectFlashRisingEdges(frames).empty());
}

TEST(AvSyncCalibrationTest, RequiresDarkBeforeNextEdge) {
    // Stays bright across several frames -> only ONE rising edge (no re-trigger without going dark).
    std::vector<CalibrationVideoFrame> frames = {
        Frame(100, 0.02),
        Frame(200, 0.95),
        Frame(300, 0.96),
        Frame(400, 0.94),
    };
    std::vector<uint64_t> edges = DetectFlashRisingEdges(frames);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0], 200u);
}

TEST(AvSyncCalibrationTest, DetectHighRunCentersFindsRunMidpoints) {
    // Two high runs: frames 2..4 (qpc 300..500, center 400) and 7..8 (qpc 800..900, center ~850).
    std::vector<double> v = {0.05, 0.10, 0.90, 0.95, 0.92, 0.08, 0.06, 0.93, 0.91, 0.04};
    std::vector<uint64_t> q = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
    std::vector<uint64_t> centers = DetectHighRunCenters(v, q);
    ASSERT_EQ(centers.size(), 2u);
    EXPECT_EQ(centers[0], 400u);  // midpoint of frames 2..4
    EXPECT_EQ(centers[1], 800u);  // midpoint of frames 7..8 -> index (7+8)/2=7 -> qpc 800
}

TEST(AvSyncCalibrationTest, DetectHighRunCentersClosesTrailingRun) {
    std::vector<double> v = {0.05, 0.9, 0.95};  // ends while still high
    std::vector<uint64_t> q = {100, 200, 300};
    std::vector<uint64_t> centers = DetectHighRunCenters(v, q);
    ASSERT_EQ(centers.size(), 1u);
    EXPECT_EQ(centers[0], 200u);  // midpoint of frames 1..2 -> index 1 -> qpc 200
}

TEST(AvSyncCalibrationTest, NormalizeByMaxScalesAndDetectsNoSignal) {
    std::vector<double> v = {0.0, 0.5, 1.5, 0.25};
    ASSERT_TRUE(NormalizeByMax(v));
    EXPECT_NEAR(v[2], 1.0, 1e-9);
    EXPECT_NEAR(v[1], 1.0 / 3.0, 1e-9);
    std::vector<double> zero = {0.0, 0.0};
    EXPECT_FALSE(NormalizeByMax(zero));
}

TEST(AvSyncCalibrationTest, PairOffsetsComputeAudioMinusVideoAndRejectOutliers) {
    // audio bursts land ~46 ms after their flash (460000 ticks). One pair is a mis-detection.
    std::vector<uint64_t> video = {1'000'000, 2'000'000, 3'000'000};
    std::vector<uint64_t> audio = {1'460'000, 2'460'000, 9'000'000};  // 46ms, 46ms, garbage
    std::vector<double> offs = PairAvOffsetsMs(audio, video, 200.0);
    ASSERT_EQ(offs.size(), 2u);  // the 3rd pair (6000 ms) exceeds the 200 ms guard
    EXPECT_NEAR(offs[0], 46.0, 1e-6);
    EXPECT_NEAR(offs[1], 46.0, 1e-6);
}

TEST(AvSyncCalibrationTest, PairOffsetsHandleUnequalLengths) {
    std::vector<uint64_t> video = {1'000'000, 2'000'000};
    std::vector<uint64_t> audio = {1'460'000};  // fewer audio than video
    std::vector<double> offs = PairAvOffsetsMs(audio, video, 200.0);
    ASSERT_EQ(offs.size(), 1u);
    EXPECT_NEAR(offs[0], 46.0, 1e-6);
}

TEST(AvSyncCalibrationTest, EndToEndMedianRecoversOffset) {
    // 5 flashes, audio 46 ms later (+/- a little jitter); median+consistency -> ~46 ms.
    std::vector<uint64_t> video, audio;
    const uint64_t step = 8'000'000;                       // 800 ms apart
    const double jitter[5] = {0.0, 0.4, -0.3, 0.2, -0.1};  // ms
    for (int i = 0; i < 5; ++i) {
        const uint64_t v = 10'000'000 + i * step;
        video.push_back(v);
        audio.push_back(v + 460'000 + static_cast<uint64_t>(jitter[i] * 10000.0));
    }
    std::vector<double> offs = PairAvOffsetsMs(audio, video, 200.0);
    MedianLatencyResult r = MedianWithConsistency(offs, 3, 8.0);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.latencyMs, 46.0, 1.0);
    EXPECT_EQ(r.agreeingCount, 5);
}

TEST(AvSyncCalibrationTest, CacheKeyFoldsInDisplayGeometry) {
    const std::string a = MakeAvCalibrationCacheKey("{dev}", 192000, 2, 3840, 2160);
    const std::string b = MakeAvCalibrationCacheKey("{dev}", 192000, 2, 2560, 1440);
    EXPECT_NE(a, b);  // different display -> different cache entry
    EXPECT_NE(a.find("av|"), std::string::npos);
    EXPECT_NE(a.find("3840x2160"), std::string::npos);
}
