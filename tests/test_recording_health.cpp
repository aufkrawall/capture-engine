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
