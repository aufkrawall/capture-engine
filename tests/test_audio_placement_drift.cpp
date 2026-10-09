#include <gtest/gtest.h>

#include <windows.h>

#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "mediaengine/audio/audio_resampler.h"
#include "mediaengine/audio/audio_sync_utils.h"

namespace {

constexpr int64_t kRate = 48000;
constexpr int64_t kPacket = 480;  // 10 ms
constexpr int64_t kSlop = kRate / 1000;

// Closed-loop model of one device-clock source: the device delivers `kPacket` samples per packet on a clock
// that is `fastPpm` fast against QPC, the intake resampler turns that into `out = in * (1 + delta / horizon)`
// samples (swr_set_compensation semantics), and the placement code compares the packet's QPC start with the
// samples already written exactly like AudioLoopCommitSource does.
struct LoopResult {
    uint64_t legacySeams = 0;  // times the cut/insert path would have run
    uint64_t compensationCalls = 0;
    int64_t maxAbsSeamAfterWarmup = 0;
    int64_t finalSeam = 0;
    int32_t maxAbsDelta = 0;
    int32_t maxStep = 0;
    ce::audio::PlacementDriftLane lane;
};

struct LoopOptions {
    double fastPpm = 28.0;
    double seconds = 600.0;
    int64_t qpcQuantumSamples = 1;  // timestamps floored to a multiple of this
    int64_t jitterSamples = 0;      // deterministic +-jitter on the QPC start
    int64_t glitchPacket = -1;      // one packet whose QPC start is late by glitchSamples
    int64_t glitchSamples = 0;
    int64_t gapPacket = -1;  // device stalls: QPC jumps ahead by gapSamples (real discontinuity)
    int64_t gapSamples = 0;
    bool eligible = true;
    double warmupSeconds = 90.0;
};

LoopResult RunLoop(const LoopOptions& opt) {
    LoopResult r;
    const int64_t packets = static_cast<int64_t>(opt.seconds * 100.0);
    const double qpcPerPacket = static_cast<double>(kPacket) / (1.0 + opt.fastPpm * 1e-6);
    double qpcPos = static_cast<double>(kRate) * 0.5;  // steady state from the first packet
    double written = qpcPos;
    int32_t delta = 0;
    int32_t lastApplied = 0;
    uint32_t lcg = 12345;
    for (int64_t k = 0; k < packets; ++k) {
        int64_t jitter = 0;
        if (opt.jitterSamples > 0) {
            lcg = lcg * 1664525u + 1013904223u;
            jitter = static_cast<int64_t>(lcg >> 16) % (2 * opt.jitterSamples + 1) - opt.jitterSamples;
        }
        int64_t packetStart = static_cast<int64_t>(qpcPos);
        packetStart -= packetStart % std::max<int64_t>(1, opt.qpcQuantumSamples);
        packetStart += jitter;
        if (k == opt.glitchPacket) {
            packetStart += opt.glitchSamples;
        }
        if (k == opt.gapPacket) {
            qpcPos += static_cast<double>(opt.gapSamples);
            packetStart = static_cast<int64_t>(qpcPos);
        }
        const int64_t seam = packetStart - static_cast<int64_t>(written);
        const uint64_t nowMs = 10000 + static_cast<uint64_t>(k) * 10;
        const auto plan = ce::audio::PlanPlacementDrift(r.lane, opt.eligible, true, seam, kPacket, kRate, nowMs);
        if (plan.applyCompensation) {
            delta = plan.compensationDelta;
            ++r.compensationCalls;
            r.maxAbsDelta = std::max(r.maxAbsDelta, std::abs(delta));
            r.maxStep = std::max(r.maxStep, std::abs(delta - lastApplied));
            lastApplied = delta;
        }
        if (!plan.absorbSeam && std::abs(seam) > kSlop) {
            ++r.legacySeams;
            written = static_cast<double>(packetStart);  // the cut/insert path lands on QPC
        }
        if (static_cast<double>(k) / 100.0 >= opt.warmupSeconds) {
            r.maxAbsSeamAfterWarmup =
                std::max(r.maxAbsSeamAfterWarmup, std::abs(packetStart - static_cast<int64_t>(written)));
        }
        written += static_cast<double>(kPacket) * (1.0 + static_cast<double>(delta) / (kRate * 10.0));
        qpcPos += qpcPerPacket;
    }
    r.finalSeam = static_cast<int64_t>(qpcPos) - static_cast<int64_t>(written);
    return r;
}

}  // namespace

TEST(PlacementDriftPolicyTest, WithinSlopStaysOnTheLegacyPath) {
    ce::audio::PlacementDriftLane lane;
    for (int64_t seam : {int64_t{0}, int64_t{12}, int64_t{-47}, kSlop, -kSlop}) {
        const auto plan = ce::audio::PlanPlacementDrift(lane, true, true, seam, kPacket, kRate, 10000);
        EXPECT_FALSE(plan.absorbSeam);
        EXPECT_FALSE(plan.applyCompensation);
    }
    EXPECT_FALSE(lane.engaged);
}

TEST(PlacementDriftPolicyTest, IneligibleOrStartupSourcesNeverAbsorb) {
    ce::audio::PlacementDriftLane lane;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(ce::audio::PlanPlacementDrift(lane, false, true, -60, kPacket, kRate, 10000 + i).absorbSeam);
        EXPECT_FALSE(ce::audio::PlanPlacementDrift(lane, true, false, -60, kPacket, kRate, 10000 + i).absorbSeam);
    }
    EXPECT_FALSE(lane.engaged);
    EXPECT_EQ(lane.updates, 0u);
}

TEST(PlacementDriftPolicyTest, OneJitteryTimestampIsNotDrift) {
    ce::audio::PlacementDriftLane lane;
    auto plan = ce::audio::PlanPlacementDrift(lane, true, true, -96, kPacket, kRate, 10000);
    EXPECT_TRUE(plan.absorbSeam);  // not cut: the error will vanish with the next timestamp
    EXPECT_FALSE(plan.applyCompensation);
    plan = ce::audio::PlanPlacementDrift(lane, true, true, 3, kPacket, kRate, 10010);
    EXPECT_FALSE(plan.absorbSeam);
    EXPECT_FALSE(lane.engaged);
    EXPECT_EQ(lane.appliedDelta, 0);
}

TEST(PlacementDriftPolicyTest, SameSignErrorEngagesAfterConfirmationAndBendsTheRateTowardTheError) {
    ce::audio::PlacementDriftLane lane;
    ce::audio::PlacementDriftPlan plan;
    for (int i = 0; i < ce::audio::kPlacementDriftConfirmPackets; ++i) {
        plan = ce::audio::PlanPlacementDrift(lane, true, true, -49, kPacket, kRate, 10000 + i * 10);
        EXPECT_TRUE(plan.absorbSeam);
    }
    EXPECT_TRUE(plan.justEngaged);
    EXPECT_TRUE(plan.applyCompensation);
    EXPECT_LT(plan.compensationDelta, 0);  // device fast: samples are removed
    EXPECT_GE(plan.compensationDelta, -ce::audio::kPlacementDriftMaxSlewPerUpdate);

    lane = {};
    for (int i = 0; i < ce::audio::kPlacementDriftConfirmPackets; ++i) {
        plan = ce::audio::PlanPlacementDrift(lane, true, true, 60, kPacket, kRate, 10000 + i * 10);
    }
    EXPECT_GT(plan.compensationDelta, 0);  // device slow: samples are added
}

TEST(PlacementDriftPolicyTest, CompensationIsCappedToThePitchBudgetAndSlewLimited) {
    ce::audio::PlacementDriftLane lane;
    ce::audio::PlacementDriftPlan plan;
    uint64_t now = 10000;
    // The window edge (5 ms = 240 samples) is the largest error the lane absorbs; its target equals the cap.
    const int64_t windowEdge = -(kRate * ce::audio::kPlacementDriftAbsorbMaxMs) / 1000;
    for (int i = 0; i < ce::audio::kPlacementDriftConfirmPackets; ++i) {
        plan = ce::audio::PlanPlacementDrift(lane, true, true, windowEdge, kPacket, kRate, now);
        now += 10;
    }
    int32_t previous = plan.compensationDelta;
    for (int i = 0; i < 60; ++i) {
        now += static_cast<uint64_t>(ce::audio::kPlacementDriftUpdateIntervalMs);
        plan = ce::audio::PlanPlacementDrift(lane, true, true, windowEdge, kPacket, kRate, now);
        ASSERT_TRUE(plan.applyCompensation);
        EXPECT_LE(std::abs(plan.compensationDelta - previous), ce::audio::kPlacementDriftMaxSlewPerUpdate);
        EXPECT_GE(plan.compensationDelta, -ce::audio::PlacementDriftMaxDelta(kRate));
        previous = plan.compensationDelta;
    }
    EXPECT_EQ(previous, -ce::audio::PlacementDriftMaxDelta(kRate));
    EXPECT_EQ(ce::audio::PlacementDriftMaxDelta(kRate), 240);  // 0.05% of ten seconds at 48 kHz
}

TEST(PlacementDriftPolicyTest, UpdatesAreRateLimitedBetweenCadenceTicks) {
    ce::audio::PlacementDriftLane lane;
    uint64_t now = 10000;
    for (int i = 0; i < ce::audio::kPlacementDriftConfirmPackets; ++i) {
        ce::audio::PlanPlacementDrift(lane, true, true, -60, kPacket, kRate, now);
        now += 10;
    }
    const uint64_t updates = lane.updates;
    EXPECT_FALSE(ce::audio::PlanPlacementDrift(lane, true, true, -60, kPacket, kRate, now + 50).applyCompensation);
    EXPECT_TRUE(ce::audio::PlanPlacementDrift(lane, true, true, -60, kPacket, kRate, now + 260).applyCompensation);
    EXPECT_EQ(lane.updates, updates + 1);
}

TEST(PlacementDriftPolicyTest, RealDiscontinuitiesStillTakeTheCutInsertPath) {
    ce::audio::PlacementDriftLane lane;
    uint64_t now = 10000;
    for (int i = 0; i < ce::audio::kPlacementDriftConfirmPackets; ++i) {
        ce::audio::PlanPlacementDrift(lane, true, true, -60, kPacket, kRate, now);
        now += 10;
    }
    ASSERT_TRUE(lane.engaged);

    // A 60 ms stall, a 6 ms overlap (> 5 ms window) and a packet fully behind the written timeline.
    for (int64_t seam : {int64_t{2880}, int64_t{-288}, -kPacket}) {
        now += 300;
        const auto plan = ce::audio::PlanPlacementDrift(lane, true, true, seam, kPacket, kRate, now);
        EXPECT_FALSE(plan.absorbSeam) << "seam=" << seam;
        EXPECT_TRUE(plan.applyCompensation);  // the error input restarts from zero
    }
    EXPECT_EQ(lane.hardSeams, 3u);
}

TEST(PlacementDriftLoopTest, FastDeviceClockIsAbsorbedWithoutAnyCut) {
    LoopOptions opt;
    opt.fastPpm = 28.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_EQ(r.legacySeams, 0u);  // the cut path ran ~14 times in ten minutes before the lane
    EXPECT_EQ(r.lane.hardSeams, 0u);
    EXPECT_TRUE(r.lane.engaged);
    EXPECT_LE(r.maxAbsSeamAfterWarmup, kSlop / 2);
    EXPECT_NEAR(ce::audio::ComputePlacementDriftLanePpm(r.lane, kRate), -28.0, 4.0);
    EXPECT_LE(r.maxAbsDelta, ce::audio::PlacementDriftMaxDelta(kRate));
    EXPECT_LE(r.maxStep, ce::audio::kPlacementDriftMaxSlewPerUpdate);
}

TEST(PlacementDriftLoopTest, SlowDeviceClockIsAbsorbedWithoutSilenceInsertion) {
    LoopOptions opt;
    opt.fastPpm = -35.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_EQ(r.legacySeams, 0u);
    EXPECT_EQ(r.lane.hardSeams, 0u);
    EXPECT_LE(r.maxAbsSeamAfterWarmup, kSlop / 2);
    EXPECT_NEAR(ce::audio::ComputePlacementDriftLanePpm(r.lane, kRate), 35.0, 4.0);
}

TEST(PlacementDriftLoopTest, CoarseQuantizedTimestampsDoNotCauseCutsOrRunaway) {
    LoopOptions opt;
    opt.fastPpm = 28.0;
    opt.qpcQuantumSamples = 80;  // loopback timestamps that move in 1.67 ms steps
    const LoopResult r = RunLoop(opt);
    EXPECT_EQ(r.legacySeams, 0u);
    EXPECT_EQ(r.lane.hardSeams, 0u);
    EXPECT_LE(r.maxAbsSeamAfterWarmup, 2 * 80);
    EXPECT_NEAR(ce::audio::ComputePlacementDriftLanePpm(r.lane, kRate), -28.0, 12.0);
    EXPECT_LE(r.maxAbsDelta, ce::audio::PlacementDriftMaxDelta(kRate));
}

TEST(PlacementDriftLoopTest, LargerDriftWithinTheBudgetStillNeedsNoCuts) {
    LoopOptions opt;
    opt.fastPpm = 300.0;
    opt.seconds = 300.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_EQ(r.legacySeams, 0u);
    EXPECT_NEAR(ce::audio::ComputePlacementDriftLanePpm(r.lane, kRate), -300.0, 40.0);
}

TEST(PlacementDriftLoopTest, DriftBeyondTheBudgetFallsBackToCutsWithoutExceedingTheCap) {
    LoopOptions opt;
    opt.fastPpm = 900.0;  // a broken clock: more than the 500 ppm lane can bend
    opt.seconds = 120.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_GT(r.legacySeams, 0u);
    EXPECT_LE(r.maxAbsDelta, ce::audio::PlacementDriftMaxDelta(kRate));
}

TEST(PlacementDriftLoopTest, CleanClockWithTimestampJitterNeverEngagesTheLane) {
    LoopOptions opt;
    opt.fastPpm = 0.0;
    opt.jitterSamples = 20;  // 0.4 ms of timestamp noise stays inside the 1 ms slop
    opt.seconds = 300.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_FALSE(r.lane.engaged);
    EXPECT_EQ(r.compensationCalls, 0u);
    EXPECT_EQ(r.legacySeams, 0u);
}

TEST(PlacementDriftLoopTest, SingleGlitchedTimestampOnACleanClockIsIgnored) {
    LoopOptions opt;
    opt.fastPpm = 0.0;
    opt.seconds = 60.0;
    opt.glitchPacket = 1000;
    opt.glitchSamples = 96;  // one packet 2 ms late
    const LoopResult r = RunLoop(opt);
    EXPECT_FALSE(r.lane.engaged);
    EXPECT_EQ(r.compensationCalls, 0u);
    EXPECT_EQ(r.legacySeams, 0u);
}

TEST(PlacementDriftLoopTest, RealStallMidRecordingIsFilledOnceAndTheLaneRecovers) {
    LoopOptions opt;
    opt.fastPpm = 28.0;
    opt.seconds = 300.0;
    opt.gapPacket = 12000;  // 120 s in
    opt.gapSamples = 2880;  // 60 ms
    opt.warmupSeconds = 150.0;
    const LoopResult r = RunLoop(opt);
    EXPECT_EQ(r.legacySeams, 1u);  // exactly the stall
    EXPECT_EQ(r.lane.hardSeams, 1u);
    EXPECT_LE(r.maxAbsSeamAfterWarmup, kSlop / 2);
}

TEST(PlacementDriftLoopTest, IneligibleSourcesKeepTheLegacyCutsUnchanged) {
    LoopOptions opt;
    opt.fastPpm = 28.0;
    opt.seconds = 300.0;
    opt.eligible = false;
    const LoopResult r = RunLoop(opt);
    EXPECT_GE(r.legacySeams, 5u);
    EXPECT_EQ(r.compensationCalls, 0u);
}

// The model above assumes swr_set_compensation(delta, horizon) scales the output count by (1 + delta/horizon)
// with positive deltas adding samples. Check that against the real intake resampler for both the same-rate
// (microphone) and the downsampling (192 kHz loopback) configuration.
namespace {

struct ResampleRun {
    int64_t inSamples = 0;
    int64_t outSamples = 0;
    std::vector<float> left;
};

ResampleRun RunIntakeResampler(int inRate, int32_t delta, double seconds, bool engageAfterOneSecond,
                               double sineHz = 1000.0) {
    AudioResampler resampler;
    AudioResampler::InputFormat inFmt{};
    inFmt.channels = 2;
    inFmt.sampleRate = inRate;
    inFmt.bitsPerSample = 32;
    inFmt.validBitsPerSample = 32;
    inFmt.isFloat = true;
    inFmt.blockAlign = 8;
    inFmt.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    AudioResampler::OutputFormat outFmt{};
    outFmt.channels = 2;
    outFmt.sampleRate = 48000;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
    outFmt.channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    EXPECT_TRUE(resampler.Init(inFmt, outFmt));

    ResampleRun run;
    const int packetFrames = inRate / 100;
    const int64_t packets = static_cast<int64_t>(seconds * 100.0);
    int64_t frame = 0;
    for (int64_t p = 0; p < packets; ++p) {
        // The lane re-issues the compensation every 250 ms like production does.
        if (delta != 0 && p % 25 == 0 && (!engageAfterOneSecond || p >= 100)) {
            EXPECT_GE(swr_set_compensation(resampler.GetSwrContext(), delta, 48000 * 10), 0);
        }
        std::vector<float> in(static_cast<size_t>(packetFrames) * 2);
        for (int i = 0; i < packetFrames; ++i, ++frame) {
            const float s = static_cast<float>(
                0.5 * std::sin(2.0 * 3.14159265358979323846 * sineHz * static_cast<double>(frame) / inRate));
            in[static_cast<size_t>(i) * 2] = s;
            in[static_cast<size_t>(i) * 2 + 1] = s;
        }
        uint8_t** out = nullptr;
        int outSamples = 0;
        EXPECT_TRUE(resampler.Process(reinterpret_cast<const uint8_t*>(in.data()),
                                      static_cast<int>(in.size() * sizeof(float)), &out, &outSamples));
        if (out && outSamples > 0) {
            const float* f = reinterpret_cast<const float*>(out[0]);
            for (int i = 0; i < outSamples; ++i) {
                run.left.push_back(f[static_cast<size_t>(i) * 2]);
            }
        }
        run.inSamples += packetFrames;
        run.outSamples += outSamples;
        AudioResampler::FreeOutputBuffer(out);
    }
    return run;
}

}  // namespace

TEST(PlacementDriftResamplerTest, SameRateCompensationRemovesAndAddsTheRequestedSamples) {
    const ResampleRun baseline = RunIntakeResampler(48000, 0, 10.0, false);
    const ResampleRun removed = RunIntakeResampler(48000, -240, 10.0, false);
    const ResampleRun added = RunIntakeResampler(48000, 240, 10.0, false);
    // The resampler holds back its half filter length (a constant, not a rate effect).
    EXPECT_NEAR(static_cast<double>(removed.outSamples - baseline.outSamples), -240.0, 24.0);
    EXPECT_NEAR(static_cast<double>(added.outSamples - baseline.outSamples), 240.0, 24.0);
}

TEST(PlacementDriftResamplerTest, DownsamplingLoopbackCompensationRemovesTheRequestedSamples) {
    const ResampleRun baseline = RunIntakeResampler(192000, 0, 10.0, false);
    const ResampleRun removed = RunIntakeResampler(192000, -96, 10.0, false);
    EXPECT_NEAR(static_cast<double>(baseline.outSamples), 480000.0, 40.0);
    EXPECT_NEAR(static_cast<double>(removed.outSamples - baseline.outSamples), -96.0, 16.0);
}

TEST(PlacementDriftResamplerTest, EngagingMidStreamIsClickFreeAndKeepsTheContentAligned) {
    // A passthrough microphone-style stream, then the lane engages after one second at ~-200 ppm. The
    // 100.25 Hz tone is at its positive peak exactly when the lane engages: the worst case for a start-up
    // transient (a tone crossing zero there would hide it).
    constexpr double kHz = 100.25;
    const ResampleRun run = RunIntakeResampler(48000, -96, 4.0, true, kHz);
    const ResampleRun untouched = RunIntakeResampler(48000, 0, 4.0, true, kHz);

    // 96 samples per 10 s over the 3 s after the engagement are ~29 samples removed. The resampler also
    // keeps its half filter length (16 samples) in flight from then on: a one-time 0.33 ms hold-back that the
    // lane sees as a small seam error and regulates, not a content shift (checked below).
    EXPECT_NEAR(static_cast<double>(run.outSamples - untouched.outSamples), -29.0 - 16.0, 8.0);

    const double maxSlope = 2.0 * 3.14159265358979323846 * kHz / 48000.0 * 0.5;  // peak sine step
    double worstStep = 0.0;
    for (size_t i = 1; i < run.left.size(); ++i) {
        worstStep = std::max(worstStep, std::abs(static_cast<double>(run.left[i]) - run.left[i - 1]));
    }
    // A deleted-overlap cut is a step of up to the full signal amplitude; engaging adds none: the worst
    // sample-to-sample step over the whole run, engagement included, is the sine's own slope.
    EXPECT_LT(worstStep, maxSlope * 1.05);

    // Content alignment: which input offset (in samples) best explains the output around a window?
    auto bestOffset = [&](size_t begin) {
        int best = 0;
        double bestErr = 1e9;
        for (int k = -60; k <= 60; ++k) {
            double err = 0.0;
            for (size_t i = begin; i < begin + 1000 && i < run.left.size(); ++i) {
                const double ideal =
                    0.5 * std::sin(2.0 * 3.14159265358979323846 * kHz * (static_cast<double>(i) + k) / 48000.0);
                err += std::abs(ideal - run.left[i]);
            }
            if (err < bestErr) {
                bestErr = err;
                best = k;
            }
        }
        return best;
    };
    EXPECT_EQ(bestOffset(40000), 0);         // before the engagement
    EXPECT_NEAR(bestOffset(60000), 0, 4);    // 0.25 s after: only the ~2 samples removed so far
    EXPECT_NEAR(bestOffset(180000), 26, 4);  // later: exactly the ~26 samples the compensation removed
}
