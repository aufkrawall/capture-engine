#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

#include "../mediaengine/audio_latency_probe.h"

using namespace ce::audio;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Build a synthetic loopback capture: `preFrames` of (optional) noise, then a Hann-windowed
// marker burst at markerFreqHz, then trailing noise. Returns the true onset frame.
std::vector<float> MakeSyntheticCapture(int sampleRate, double markerFreqHz, int preFrames, int markerFrames,
                                        int postFrames, double markerAmp, double noiseAmp, unsigned seed) {
    std::vector<float> buf(static_cast<size_t>(preFrames + markerFrames + postFrames), 0.0f);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (auto& s : buf) {
        s = static_cast<float>(noiseAmp) * noise(rng);
    }
    const double w = 2.0 * kPi * markerFreqHz / sampleRate;
    for (int i = 0; i < markerFrames; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / std::max(1, markerFrames - 1));
        buf[static_cast<size_t>(preFrames + i)] += static_cast<float>(markerAmp * hann * std::sin(w * i));
    }
    return buf;
}
}  // namespace

TEST(AudioLatencyProbeTest, ResolveMarkerSpecIsInaudibleAndBelowNyquist) {
    for (int rate : {44100, 48000, 96000, 192000}) {
        const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(rate);
        ASSERT_TRUE(spec.valid) << "rate=" << rate;
        EXPECT_GE(spec.markerFreqHz, 19000.0) << "marker should be inaudible, rate=" << rate;
        EXPECT_LT(spec.markerFreqHz, rate * 0.5) << "marker must be below Nyquist, rate=" << rate;
        EXPECT_LE(spec.amplitude, 0.02);
        EXPECT_GT(spec.markerFrames, 0);
        EXPECT_GT(spec.leadInFrames, 0);
    }
}

TEST(AudioLatencyProbeTest, ResolveMarkerSpecRejectsTooLowRate) {
    EXPECT_FALSE(ResolveProbeMarkerSpec(8000).valid);
    EXPECT_FALSE(ResolveProbeMarkerSpec(32000).valid);
}

TEST(AudioLatencyProbeTest, GenerateMarkerHasSilentLeadInAndConcentratedEnergy) {
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(48000);
    std::vector<float> wave;
    GenerateProbeMarkerMono(spec, wave);
    ASSERT_EQ(wave.size(), static_cast<size_t>(spec.totalFrames()));

    // Lead-in is exact silence.
    for (int i = 0; i < spec.leadInFrames; ++i) {
        EXPECT_FLOAT_EQ(wave[static_cast<size_t>(i)], 0.0f);
    }
    // Marker band carries far more energy than a wrong (audible) band.
    const double atMarker =
        GoertzelPower(wave.data() + spec.leadInFrames, spec.markerFrames, spec.sampleRate, spec.markerFreqHz);
    const double at1k = GoertzelPower(wave.data() + spec.leadInFrames, spec.markerFrames, spec.sampleRate, 1000.0);
    EXPECT_GT(atMarker, at1k * 50.0);
}

TEST(AudioLatencyProbeTest, DetectCenterMatchesKnownPeakCleanSignal) {
    const int rate = 48000;
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(rate);
    const int preFrames = 5000;  // ~104 ms
    std::vector<float> cap =
        MakeSyntheticCapture(rate, spec.markerFreqHz, preFrames, spec.markerFrames, 4000, spec.amplitude, 0.0, 1);

    const int center = DetectMarkerCenterFrame(cap.data(), cap.size(), rate, spec.markerFreqHz);
    ASSERT_GE(center, 0);
    // The Hann burst peaks at its midpoint; detection should land within ~1 window of it.
    const int trueCenter = preFrames + spec.markerFrames / 2;
    EXPECT_NEAR(center, trueCenter, rate / 200);  // ~1 detection window (5 ms)
}

TEST(AudioLatencyProbeTest, DetectCenterSurvivesUnrelatedAudioAndNoise) {
    const int rate = 48000;
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(rate);
    const int preFrames = 3000;
    std::vector<float> cap =
        MakeSyntheticCapture(rate, spec.markerFreqHz, preFrames, spec.markerFrames, 4000, spec.amplitude, 0.01, 7);
    // Add a loud unrelated 440 Hz tone across the whole buffer (simulates music playing). The
    // narrowband Goertzel at the ultrasonic marker frequency must ignore it.
    const double w = 2.0 * kPi * 440.0 / rate;
    for (size_t i = 0; i < cap.size(); ++i) {
        cap[i] += static_cast<float>(0.5 * std::sin(w * static_cast<double>(i)));
    }
    const int center = DetectMarkerCenterFrame(cap.data(), cap.size(), rate, spec.markerFreqHz);
    ASSERT_GE(center, 0);
    const int trueCenter = preFrames + spec.markerFrames / 2;
    EXPECT_NEAR(center, trueCenter, rate / 100);  // ~2 detection windows
}

TEST(AudioLatencyProbeTest, DetectCenterReturnsNegativeWhenMarkerAbsent) {
    const int rate = 48000;
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(rate);
    // Only broadband noise, no marker tone.
    std::vector<float> cap = MakeSyntheticCapture(rate, spec.markerFreqHz, 8000, 0, 0, 0.0, 0.02, 3);
    const int center = DetectMarkerCenterFrame(cap.data(), cap.size(), rate, spec.markerFreqHz);
    EXPECT_LT(center, 0);
}

// End-to-end (pure-logic) sanity: synthesize a render+loopback pair offset by a known latency and
// confirm aligning marker CENTERS recovers that latency, regardless of the Hann envelope.
TEST(AudioLatencyProbeTest, CenterAlignmentRecoversKnownLatencyEndToEnd) {
    const int rate = 48000;
    const ProbeMarkerSpec spec = ResolveProbeMarkerSpec(rate);
    const int latencyFrames = rate * 46 / 1000;  // 46 ms render->loopback path
    // Loopback capture = the rendered marker shifted later by latencyFrames (plus capture lead-in).
    const int capPre = 2000 + latencyFrames;
    std::vector<float> cap =
        MakeSyntheticCapture(rate, spec.markerFreqHz, capPre, spec.markerFrames, 6000, spec.amplitude, 0.005, 11);

    const int center = DetectMarkerCenterFrame(cap.data(), cap.size(), rate, spec.markerFreqHz);
    ASSERT_GE(center, 0);

    // Render center is at capPre - latencyFrames + markerFrames/2 (the marker would have been
    // "rendered" latencyFrames earlier). Convert the frame delta to a latency and check it.
    const int renderCenterFrame = (capPre - latencyFrames) + spec.markerFrames / 2;
    const uint64_t loopbackCenterQpc = AudioFramesToHundredNanoseconds(static_cast<uint64_t>(center), rate);
    const uint64_t renderCenterQpc = AudioFramesToHundredNanoseconds(static_cast<uint64_t>(renderCenterFrame), rate);
    const double measured = ComputeRenderLatencyMs(loopbackCenterQpc, renderCenterQpc);
    EXPECT_NEAR(measured, 46.0, 6.0);  // within ~1 detection window (5 ms)
}

TEST(AudioLatencyProbeTest, MedianWithConsistencyAcceptsClusterAndRejectsOutliers) {
    // 4 tight readings around 46 ms + 1 wild outlier -> consensus 46, outlier excluded.
    std::vector<double> shots = {45.0, 46.0, 47.0, 46.5, 220.0};
    MedianLatencyResult r = MedianWithConsistency(shots, 3, 8.0);
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.latencyMs, 46.0, 1.5);
    EXPECT_EQ(r.agreeingCount, 4);
}

TEST(AudioLatencyProbeTest, MedianWithConsistencyRejectsTooFewShots) {
    std::vector<double> shots = {46.0, 46.5};  // fewer than minCount
    EXPECT_FALSE(MedianWithConsistency(shots, 3, 8.0).ok);
    EXPECT_FALSE(MedianWithConsistency({}, 3, 8.0).ok);
}

TEST(AudioLatencyProbeTest, MedianWithConsistencyRejectsScatter) {
    // 5 readings but no 3 agree within tolerance -> no consensus (auto-detect unavailable).
    std::vector<double> shots = {10.0, 40.0, 80.0, 120.0, 200.0};
    EXPECT_FALSE(MedianWithConsistency(shots, 3, 8.0).ok);
}

TEST(AudioLatencyProbeTest, MedianWithConsistencyExactlyMinCountConsensus) {
    std::vector<double> shots = {46.0, 47.0, 46.5, 5.0, 300.0};  // exactly 3 cluster
    MedianLatencyResult r = MedianWithConsistency(shots, 3, 8.0);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.agreeingCount, 3);
    EXPECT_NEAR(r.latencyMs, 46.5, 1.0);
}

TEST(AudioLatencyProbeTest, ComputeRenderLatencyAndPlausibility) {
    // 46 ms => 460000 hundred-ns ticks.
    EXPECT_NEAR(ComputeRenderLatencyMs(1'460'000ULL, 1'000'000ULL), 46.0, 1e-9);
    EXPECT_TRUE(IsPlausibleLatencyMs(46.0));
    EXPECT_TRUE(IsPlausibleLatencyMs(0.0));
    EXPECT_FALSE(IsPlausibleLatencyMs(-1.0));
    EXPECT_FALSE(IsPlausibleLatencyMs(5000.0));
}

TEST(AudioLatencyProbeTest, MemoryCacheLookupAndUpsert) {
    std::vector<LatencyCacheEntry> entries;
    const std::string keyA =
        MakeRenderEndpointCacheKey("{0.0.0.00000000}.{guid-a}", 192000, 2, 32, 8, 3, 100000, 30000);
    const std::string keyB =
        MakeRenderEndpointCacheKey("{0.0.0.00000000}.{guid-b}", 48000, 8, 32, 32, 0x63f, 100000, 30000);
    UpsertLatencyCache(entries, keyA, 46.0);
    UpsertLatencyCache(entries, keyB, 12.5);
    UpsertLatencyCache(entries, keyA, 47.0);  // overwrite
    ASSERT_EQ(entries.size(), 2u);

    double v = 0.0;
    ASSERT_TRUE(LookupLatencyCache(entries, keyA, &v));
    EXPECT_NEAR(v, 47.0, 1e-3);
    ASSERT_TRUE(LookupLatencyCache(entries, keyB, &v));
    EXPECT_NEAR(v, 12.5, 1e-3);
    EXPECT_FALSE(LookupLatencyCache(entries, "missing", &v));
}

TEST(AudioLatencyProbeTest, CacheKeySanitizesDelimiters) {
    const std::string key = MakeRenderEndpointCacheKey("dev=with\nbad\rchars", 48000, 2, 32, 8, 3, 100000, 30000);
    // The line format is key=value, so '=' / CR / LF must not survive in the key.
    EXPECT_EQ(key.find('='), std::string::npos);
    EXPECT_EQ(key.find('\n'), std::string::npos);
    EXPECT_EQ(key.find('\r'), std::string::npos);
    EXPECT_EQ(key, "dev_with_bad_chars|sr48000|ch2|bits32|align8|mask3|period100000|min30000");
}
