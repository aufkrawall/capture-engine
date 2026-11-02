#include <gtest/gtest.h>

#include "hook/d3d9/dx9_pacing_policy.h"

namespace {

using ce::dx9_pacing_policy::CalculatePresentOverheadUs;
using ce::dx9_pacing_policy::EvaluatePacing;

TEST(Dx9PacingPolicyTest, FullscreenHardwareVSyncActiveSkipsSoftwareFallback) {
    // When live interval matches the requested interval (e.g. 1 == 1),
    // hardware handles VSync; no fallback or extra pacing is needed.
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/false,
        /*liveInterval=*/1,
        /*presentUs=*/6900,
        /*fpsLimiterActivelyLimiting=*/false);

    EXPECT_FALSE(decision.needsFullscreenFallback);
    EXPECT_FALSE(decision.shouldPace);
    EXPECT_FALSE(decision.skipDueToLimiter);
}

TEST(Dx9PacingPolicyTest, FullscreenIntervalMismatchEnablesFallback) {
    // If live interval is 0 (or mismatch) despite vsync override = 1,
    // the driver failed to apply the requested interval, so fallback pacing triggers.
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/false,
        /*liveInterval=*/0,
        /*presentUs=*/500,
        /*fpsLimiterActivelyLimiting=*/true);

    EXPECT_TRUE(decision.needsFullscreenFallback);
    EXPECT_TRUE(decision.shouldPace);
    EXPECT_FALSE(decision.skipDueToLimiter);
}

TEST(Dx9PacingPolicyTest, WindowedFastPresentPacesWhenLimiterInactive) {
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/true,
        /*liveInterval=*/1,
        /*presentUs=*/1500,
        /*fpsLimiterActivelyLimiting=*/false);

    EXPECT_FALSE(decision.needsFullscreenFallback);
    EXPECT_TRUE(decision.shouldPace);
    EXPECT_FALSE(decision.skipDueToLimiter);
}

TEST(Dx9PacingPolicyTest, WindowedSkipsSoftwarePacingWhenFpsLimiterActive) {
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/true,
        /*liveInterval=*/1,
        /*presentUs=*/1500,
        /*fpsLimiterActivelyLimiting=*/true);

    EXPECT_FALSE(decision.needsFullscreenFallback);
    EXPECT_FALSE(decision.shouldPace);
    EXPECT_TRUE(decision.skipDueToLimiter);
}

TEST(Dx9PacingPolicyTest, OverridesDisabledOrImmediateIntervalSkipsPacing) {
    const auto disabled = EvaluatePacing(
        /*vsyncShouldOverride=*/false,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/true,
        /*liveInterval=*/1,
        /*presentUs=*/1000,
        /*fpsLimiterActivelyLimiting=*/false);
    EXPECT_FALSE(disabled.shouldPace);

    const auto immediate = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/0,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/true,
        /*liveInterval=*/0,
        /*presentUs=*/1000,
        /*fpsLimiterActivelyLimiting=*/false);
    EXPECT_FALSE(immediate.shouldPace);
}

TEST(Dx9PacingPolicyTest, TranslationRuntimeSkipsPacing) {
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/false,
        /*nonSystemRuntimeLoaded=*/true,
        /*windowed=*/true,
        /*liveInterval=*/1,
        /*presentUs=*/1000,
        /*fpsLimiterActivelyLimiting=*/false);

    EXPECT_FALSE(decision.shouldPace);
}

TEST(Dx9PacingPolicyTest, StagingCaptureRecordingSkipsPacing) {
    const auto decision = EvaluatePacing(
        /*vsyncShouldOverride=*/true,
        /*vsyncPresentInterval=*/1,
        /*stagingCaptureActiveAndRecording=*/true,
        /*nonSystemRuntimeLoaded=*/false,
        /*windowed=*/true,
        /*liveInterval=*/1,
        /*presentUs=*/1000,
        /*fpsLimiterActivelyLimiting=*/false);

    EXPECT_FALSE(decision.shouldPace);
}

TEST(Dx9PacingPolicyTest, CalculatePresentOverheadDeductsHardwareWaits) {
    // 8ms total frame hook time:
    // 1ms fps limit wait + 6ms driver Present wait (hardware VSync) + 0.5ms prerender wait = 7.5ms intentional
    // Actual overhead: 8000 - 1000 - 6000 - 500 = 500 us (not 7000 us)
    const int64_t overheadUs = CalculatePresentOverheadUs(
        /*totalUs=*/8000,
        /*fpsLimitUs=*/1000,
        /*presentCallUs=*/6000,
        /*prerenderWaitUs=*/500);

    EXPECT_EQ(overheadUs, 500);
}

TEST(Dx9PacingPolicyTest, CalculatePresentOverheadClampsToZero) {
    const int64_t overheadUs = CalculatePresentOverheadUs(
        /*totalUs=*/5000,
        /*fpsLimitUs=*/2000,
        /*presentCallUs=*/4000,
        /*prerenderWaitUs=*/0);

    EXPECT_EQ(overheadUs, 0);
}

}  // namespace
