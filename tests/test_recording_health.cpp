#include <gtest/gtest.h>

#include "../common/capture_pipeline_policy.h"

namespace policy = ce::capture_policy;

TEST(RecordingHealthPolicyTest, HealthyCfrWobbleDoesNotLatchDamage) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/249});

    EXPECT_EQ(state.flags, 0u);
    EXPECT_EQ(state.peakDebtMs, 249u);
    EXPECT_STREQ(policy::GetRecordingHealthStatus(state.flags), "healthy");
}

TEST(RecordingHealthPolicyTest, SustainedEncoderPressureLatchesCauseWithoutInventingDamage) {
    policy::RecordingHealthState state;
    const policy::RecordingHealthObservation pressure = {
        /*videoLive=*/true, /*cfrEnabled=*/true, /*encoderPressure=*/true,
        /*muxPressure=*/false, /*timelineDebtMs=*/100};

    state = policy::UpdateRecordingHealth(state, pressure);
    EXPECT_FALSE(policy::HasRecordingCapacityCause(state.flags));
    state = policy::UpdateRecordingHealth(state, pressure);

    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags,
                                                policy::kRecordingHealthFlagEncoderPressureObserved));
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
}

TEST(RecordingHealthPolicyTest, EncoderDebtLatchesDegradationAndSurvivesRecovery) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/true, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/2400});
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagSevere));

    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/1800});
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagRecovering));
    EXPECT_STREQ(policy::GetRecordingHealthStatus(state.flags), "degraded");

    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/0});
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagRecovering));
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_EQ(state.currentDebtMs, 0u);
    EXPECT_EQ(state.peakDebtMs, 2400u);
}

TEST(RecordingHealthPolicyTest, MuxDebtHasAnIndependentLatchedCause) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/true,
                                                   /*timelineDebtMs=*/750});

    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags,
                                                policy::kRecordingHealthFlagMuxPressureObserved));
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags,
                                                 policy::kRecordingHealthFlagEncoderPressureObserved));
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_STREQ(policy::GetRecordingHealthCause(state.flags), "mux");
}

TEST(RecordingHealthPolicyTest, SourceOnlyDebtDoesNotBlameEncoderOrMux) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/5000});

    EXPECT_FALSE(policy::HasRecordingCapacityCause(state.flags));
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_EQ(state.peakDebtMs, 5000u);

    const policy::RecordingHealthObservation laterMinorPressure = {
        /*videoLive=*/true, /*cfrEnabled=*/true, /*encoderPressure=*/true,
        /*muxPressure=*/false, /*timelineDebtMs=*/100};
    state = policy::UpdateRecordingHealth(state, laterMinorPressure);
    state = policy::UpdateRecordingHealth(state, laterMinorPressure);
    EXPECT_TRUE(policy::HasRecordingCapacityCause(state.flags));
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_EQ(state.capacityAttributedDebtMs, 0u);

    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/6000});
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_EQ(state.peakDebtMs, 6000u);
    EXPECT_EQ(state.capacityAttributedDebtMs, 0u);
}

TEST(RecordingHealthPolicyTest, LaterLargerSourceDebtDoesNotBecomeCapacityDominant) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/true, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/600});
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
    EXPECT_TRUE(policy::IsRecordingCapacityDebtDominant(state));

    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/0});
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/6000});

    EXPECT_EQ(state.capacityAttributedDebtMs, 600u);
    EXPECT_EQ(state.peakDebtMs, 6000u);
    EXPECT_FALSE(policy::IsRecordingCapacityDebtDominant(state));
}

TEST(RecordingHealthPolicyTest, VfrNeverCreatesCfrDebtHealthState) {
    policy::RecordingHealthState state;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/false,
                                                   /*encoderPressure=*/true, /*muxPressure=*/true,
                                                   /*timelineDebtMs=*/5000});

    EXPECT_EQ(state.flags, 0u);
    EXPECT_EQ(state.currentDebtMs, 0u);
    EXPECT_EQ(state.peakDebtMs, 0u);
}

TEST(RecordingHealthPolicyTest, OverlayKeepsPureSourceLimitationQuietButShowsCausalRecovery) {
    EXPECT_FALSE(policy::IsWgcCaptureLimitedForOverlay(0));
    EXPECT_TRUE(policy::IsWgcCaptureLimitedForOverlay(policy::kWgcCaptureHealthFlagSourceStarved));
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(policy::kEncoderOverloadFlagEncoder,
                                                   policy::kWgcCaptureHealthFlagSourceStarved),
              policy::kOverlayWarningNone);

    const uint32_t recovering = policy::kRecordingHealthFlagEncoderPressureObserved |
                                policy::kRecordingHealthFlagTimelineDebt |
                                policy::kRecordingHealthFlagRecovering;
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(0, policy::kWgcCaptureHealthFlagSourceStarved, recovering),
              policy::kOverlayWarningRecordingRecovering);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(
                  0, policy::kWgcCaptureHealthFlagSchedulerLimited,
                  recovering | policy::kRecordingHealthFlagVideoDegraded),
              policy::kOverlayWarningRecordingRecovering);
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(
                  0, policy::kWgcCaptureHealthFlagSchedulerLimited,
                  policy::kRecordingHealthFlagEncoderPressureObserved |
                      policy::kRecordingHealthFlagVideoDegraded),
              policy::kOverlayWarningRecordingDegraded);
}

// A GPU driver reset left the encoder failing every tick while the recording
// ran on as live audio over no video, reported as saved.
TEST(VideoOutputFailureStreakTest, OnlyASustainedFailureStreakEndsTheRecording) {
    using ce::capture_policy::ObserveVideoOutputAttempt;
    using ce::capture_policy::VideoOutputFailureStreak;
    constexpr uint64_t kStopMs = ce::capture_policy::kVideoOutputFailureStopMs;

    VideoOutputFailureStreak streak;
    // Failures inside the bound, then a success: no stop, streak cleared.
    for (uint64_t t = 1000; t < 1000 + kStopMs - 1; t += 16) {
        EXPECT_FALSE(ObserveVideoOutputAttempt(streak, false, false, t));
    }
    EXPECT_FALSE(ObserveVideoOutputAttempt(streak, true, false, 1000 + kStopMs));
    EXPECT_EQ(streak.failures, 0u);

    // Back-pressure deferrals are never failures, however long they last.
    for (uint64_t t = 10000; t < 10000 + 2 * kStopMs; t += 16) {
        EXPECT_FALSE(ObserveVideoOutputAttempt(streak, false, true, t));
    }
    EXPECT_EQ(streak.failures, 0u);

    // A sustained streak requests the stop exactly once.
    int stops = 0;
    for (uint64_t t = 30000; t <= 30000 + 2 * kStopMs; t += 16) {
        stops += ObserveVideoOutputAttempt(streak, false, false, t) ? 1 : 0;
    }
    EXPECT_EQ(stops, 1);
    EXPECT_TRUE(streak.stopRequested);
}

TEST(VideoOutputFailureStreakTest, AFewSlowFailuresNeverCrossTheBoundAlone) {
    using ce::capture_policy::ObserveVideoOutputAttempt;
    ce::capture_policy::VideoOutputFailureStreak streak;
    // Two failures far apart in time are not a sustained streak of attempts.
    EXPECT_FALSE(ObserveVideoOutputAttempt(streak, false, false, 0));
    EXPECT_FALSE(ObserveVideoOutputAttempt(streak, false, false, 60000));
    EXPECT_FALSE(streak.stopRequested);
}

// Output loss found at finalization keeps video and audio apart: an audio-only loss used
// to be folded into kRecordingHealthFlagVideoDegraded and announced as "video degraded".
TEST(RecordingHealthPolicyTest, OutputDegradedFlagsKeepVideoAndAudioApart) {
    EXPECT_EQ(policy::ComposeOutputDegradedFlags(false, false), 0u);
    EXPECT_EQ(policy::ComposeOutputDegradedFlags(true, false), policy::kRecordingHealthFlagVideoDegraded);
    EXPECT_EQ(policy::ComposeOutputDegradedFlags(false, true), policy::kRecordingHealthFlagAudioDegraded);
    EXPECT_EQ(policy::ComposeOutputDegradedFlags(true, true), policy::kRecordingHealthDegradedMask);
    EXPECT_NE(policy::kRecordingHealthFlagAudioDegraded, policy::kRecordingHealthFlagVideoDegraded);
    EXPECT_EQ(policy::kRecordingHealthFlagAudioDegraded &
                  (policy::kRecordingHealthCauseMask | policy::kRecordingHealthFlagTimelineDebt |
                   policy::kRecordingHealthFlagRecovering | policy::kRecordingHealthFlagSevere),
              0u);

    EXPECT_STREQ(policy::GetRecordingDegradedScope(0u), "none");
    EXPECT_STREQ(policy::GetRecordingDegradedScope(policy::kRecordingHealthFlagVideoDegraded), "video");
    EXPECT_STREQ(policy::GetRecordingDegradedScope(policy::kRecordingHealthFlagAudioDegraded), "audio");
    EXPECT_STREQ(policy::GetRecordingDegradedScope(policy::kRecordingHealthDegradedMask), "audio_and_video");
}

TEST(RecordingHealthPolicyTest, AudioOnlyLossIsReportedDegradedWithoutACapacityCause) {
    const uint32_t flags = policy::kRecordingHealthFlagAudioDegraded;
    EXPECT_STREQ(policy::GetRecordingHealthStatus(flags), "degraded");
    EXPECT_STREQ(policy::GetRecordingHealthCause(flags), "none");
    // The live warning is about video timeline debt; audio loss is only known at
    // finalization and must never raise "Recording video degraded!".
    EXPECT_EQ(policy::SelectWgcOverlayWarningKind(0u, 0u, flags), policy::kOverlayWarningNone);
}

TEST(RecordingHealthPolicyTest, AudioDegradedSurvivesLiveHealthUpdates) {
    policy::RecordingHealthState state;
    state.flags = policy::kRecordingHealthFlagAudioDegraded;
    state = policy::UpdateRecordingHealth(state, {/*videoLive=*/true, /*cfrEnabled=*/true,
                                                   /*encoderPressure=*/false, /*muxPressure=*/false,
                                                   /*timelineDebtMs=*/0});
    EXPECT_TRUE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagAudioDegraded));
    EXPECT_FALSE(policy::HasRecordingHealthFlag(state.flags, policy::kRecordingHealthFlagVideoDegraded));
}
