// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>
#include "mediaengine/audio/app_audio_drain_diagnostics.h"
#include "mediaengine/audio/audio_sync_utils.h"

namespace {
using ce::audio::AppAudioDrainDiagnostics;
using ce::audio::CfrAppAudioBacklogDrainReason;

struct Diagnostics {
    static void Capture(const char* line, void* context) {
        static_cast<std::vector<std::string>*>(context)->emplace_back(line);
    }
    std::vector<std::string> lines;
    AppAudioDrainDiagnostics meter{Capture, &lines};
    ce::audio::AppAudioDrainLogContext context{3, 1, 48000, false, true, false};
    void Observe(bool active, uint64_t nowMs, int32_t delta = 480) {
        ce::audio::CfrAppAudioBacklogDrainDecision decision;
        decision.active = active;
        decision.reason = active ? CfrAppAudioBacklogDrainReason::Active : CfrAppAudioBacklogDrainReason::WithinSlack;
        decision.backlogSamples = active ? 5000 : 4700;
        decision.targetLeadSamples = 4000;
        decision.excessSamples = decision.backlogSamples - decision.targetLeadSamples;
        decision.compensationDelta = active ? delta : 0;
        meter.Observe(decision, context, nowMs);
    }
    void Exceptional(CfrAppAudioBacklogDrainReason reason, uint64_t nowMs) {
        ce::audio::CfrAppAudioBacklogDrainDecision decision;
        decision.reason = reason;
        meter.Observe(decision, context, nowMs);
    }
};

TEST(AppAudioDrainDiagnosticsTest, RoutineOscillationsAreAggregatedWithExactTransitionsAndTime) {
    Diagnostics d;
    d.Observe(true, 0);
    for (uint64_t index = 1; index <= 100; ++index)
        d.Observe(index % 2 == 0, index * 10);
    ASSERT_EQ(d.lines.size(), 1u);
    d.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 1100);
    ASSERT_EQ(d.lines.size(), 2u);
    const auto& summary = d.lines.back();
    EXPECT_NE(summary.find("boundary=stop"), std::string::npos);
    EXPECT_NE(summary.find("totalObservations=101 totalTransitions=100 totalActiveMs=600 totalInactiveMs=500"),
              std::string::npos);
    EXPECT_NE(summary.find("delayRangeMs=97.917..104.167"), std::string::npos);
    EXPECT_NE(summary.find("excessRangeMs=14.583..20.833"), std::string::npos);
    EXPECT_NE(summary.find("totalPeakComp=0.1000%"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, HeartbeatIsBoundedAndDoesNotLoseOscillationCounts) {
    Diagnostics d;
    d.Observe(true, 0);
    for (uint64_t index = 1; index < 500; ++index)
        d.Observe(index % 2 == 0, index * 10);
    ASSERT_EQ(d.lines.size(), 1u);
    d.Observe(true, 5000, 960);
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_NE(d.lines.back().find("[AppDrain] summary"), std::string::npos);
    EXPECT_NE(d.lines.back().find("windowMs=5000 observations=500 transitions=500 activeMs=2500 inactiveMs=2500"),
              std::string::npos);
    EXPECT_NE(d.lines.back().find("peakComp=0.2000%"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, FirstActivationAndForceDrainRemainImmediateWithTheExistingStatePrefix) {
    Diagnostics d;
    d.Observe(false, 0);
    d.Observe(true, 1);
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_NE(d.lines.back().find("[AppDrain] state src=3 track=1 active=1 reason=active"), std::string::npos);
    d.Observe(false, 2);
    ASSERT_EQ(d.lines.size(), 2u);
    d.context.forceDrain = true;
    d.Exceptional(CfrAppAudioBacklogDrainReason::ForceDrain, 3);
    ASSERT_EQ(d.lines.size(), 3u);
    EXPECT_NE(d.lines.back().find("[AppDrain] state src=3 track=1 active=0 reason=force_drain"), std::string::npos);
    EXPECT_NE(d.lines.back().find("forceDrain=1"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, ExceptionalChangesAndRecoveryAreImmediateButRepeatedStatesAreMetered) {
    Diagnostics d;
    d.Exceptional(CfrAppAudioBacklogDrainReason::StartupNotSettled, 0);
    d.Exceptional(CfrAppAudioBacklogDrainReason::StartupNotSettled, 1);
    ASSERT_EQ(d.lines.size(), 1u);
    d.Exceptional(CfrAppAudioBacklogDrainReason::TimelineRecoveryActive, 2);
    ASSERT_EQ(d.lines.size(), 2u);
    d.Observe(false, 3);
    ASSERT_EQ(d.lines.size(), 3u);
    EXPECT_NE(d.lines.back().find("reason=within_slack"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, StartupProtectionChangesRemainImmediateWithinTheRoutineGroup) {
    Diagnostics d;
    d.Observe(false, 0);
    d.context.startupProtected = true;
    d.Observe(false, 1);
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_NE(d.lines.back().find("startupProtected=1"), std::string::npos);
    d.context.startupProtected = false;
    d.context.startupSettled = false;
    d.Observe(false, 2);
    ASSERT_EQ(d.lines.size(), 3u);
    EXPECT_NE(d.lines.back().find("startupSettled=0 startupProtected=0"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, PeakCompensationHandlesNegativeDeltaAndTheRouteSampleRate) {
    Diagnostics d;
    d.context.sampleRate = 96000;
    d.Observe(true, 0, std::numeric_limits<int32_t>::min());
    d.Observe(false, 5000);
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_NE(d.lines.back().find("peakComp=223696.2133%"), std::string::npos);
    EXPECT_NE(d.lines.back().find("totalDelayRangeMs=48.958..52.083"), std::string::npos);
    EXPECT_NE(d.lines.back().find("totalTargetRangeMs=41.667..41.667"), std::string::npos);
    EXPECT_NE(d.lines.back().find("totalExcessRangeMs=7.292..10.417"), std::string::npos);
    EXPECT_LT(d.lines.back().size(), 1535u);
    EXPECT_NE(d.lines.back().find("suppressed=0"), std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, QuietFinalTailIsFlushedOnceWithoutInventingObservations) {
    Diagnostics d;
    d.Observe(false, 0);
    d.Observe(true, 10);
    d.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 40);
    ASSERT_EQ(d.lines.size(), 3u);
    EXPECT_NE(d.lines.back().find("windowMs=30 observations=0 transitions=0 activeMs=30 inactiveMs=0"),
              std::string::npos);
    EXPECT_NE(d.lines.back().find("totalObservations=2 totalTransitions=1 totalActiveMs=30 totalInactiveMs=10"),
              std::string::npos);
    EXPECT_NE(d.lines.back().find("peakComp=0.1000%"), std::string::npos);
    d.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 41);
    EXPECT_EQ(d.lines.size(), 3u);
}

TEST(AppAudioDrainDiagnosticsTest, EpochFlushPreservesTotalsAndExcludesTheUnobservedGap) {
    Diagnostics d;
    d.Observe(true, 0);
    d.meter.Flush(AppAudioDrainDiagnostics::Boundary::Epoch, 100);
    ASSERT_EQ(d.lines.size(), 2u);
    EXPECT_NE(d.lines.back().find("boundary=epoch"), std::string::npos);
    d.Observe(false, 200);
    d.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 250);
    ASSERT_EQ(d.lines.size(), 4u);
    EXPECT_NE(d.lines.back().find("totalObservations=2 totalTransitions=0 totalActiveMs=100 totalInactiveMs=50"),
              std::string::npos);
}

TEST(AppAudioDrainDiagnosticsTest, RoutesAreIndependentAndResetStartsANewRecording) {
    Diagnostics a, b;
    b.context.source = 6;
    a.Observe(true, 0);
    b.Observe(true, 0);
    a.Observe(false, 1);
    a.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 2);
    b.meter.Flush(AppAudioDrainDiagnostics::Boundary::Stop, 2);
    ASSERT_EQ(a.lines.size(), 2u);
    ASSERT_EQ(b.lines.size(), 2u);
    EXPECT_NE(b.lines.back().find("src=6 track=1"), std::string::npos);
    EXPECT_NE(b.lines.back().find("totalObservations=1 totalTransitions=0 totalActiveMs=2"), std::string::npos);
    a.meter.Reset();
    a.Observe(true, 3);
    EXPECT_NE(a.lines.back().find("totalObservations=1 totalTransitions=0 totalActiveMs=0"), std::string::npos);
}

}  // namespace
