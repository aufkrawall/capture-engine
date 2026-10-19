#include "test_fps_limiter_shared.h"

#include "hook/ngx/dlssg_health_policy.h"

// DLSS-G enabled but generating nothing (menus, loading). Talos 20261019_182822: the runtime reported
// `[DLSSG HEALTH] ON but NOT interpolating` for 7 s while the limiter kept `fg=1/2x fgProof=d3d-sleep`
// and the game ran - and recorded - at 60 fps under a 120 fps capture-sync cap.
namespace {

void ResetDLSSGIdleState() {
    g_FGCompat.SetDLSSGHealthIdle(false);
    g_FGCompat.NoteRuntimeOutputGroup(2);
}

void ProveSingleOutputGroups() {
    for (uint32_t i = 0; i < FGCompatibility::kSingleOutputGroupsForIdle; ++i) {
        g_FGCompat.NoteRuntimeOutputGroup(1);
    }
}

TEST(DLSSGIdlePolicyTest, IdleWithdrawsOnlyDlssFrameGenerationProof) {
    using ce::fg_runtime::RuntimeMode;
    using ce::fps_limiter_policy::IsFrameGenerationProducingForPacing;
    EXPECT_TRUE(IsFrameGenerationProducingForPacing(RuntimeMode::kDLSSFG, true, true, false, false));
    EXPECT_FALSE(IsFrameGenerationProducingForPacing(RuntimeMode::kDLSSFG, true, true, false, true));
    EXPECT_FALSE(IsFrameGenerationProducingForPacing(RuntimeMode::kDLSSFG, true, false, true, true));
    EXPECT_TRUE(IsFrameGenerationProducingForPacing(RuntimeMode::kFSRFG, true, false, false, true))
        << "the DLSS-G health signal says nothing about FSR";
}

TEST(DLSSGIdlePolicyTest, IdleDriverIntervalAsksForTheFactorOnTop) {
    using ce::fps_limiter_policy::ResolveIdleGenerationDriverTargetFps;
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(120, true, 2, true), 240);
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(120, true, 4, true), 480);
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(120, false, 2, true), 120);
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(120, true, 2, false), 120) << "FSR is invisible to the driver";
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(120, true, 1, true), 120);
    EXPECT_EQ(ResolveIdleGenerationDriverTargetFps(0, true, 2, true), 0);
}

TEST(DLSSGIdlePolicyTest, HealthStreakThreshold) {
    using ce::streamline_runtime_policy::IsDLSSGIdleForPacingEvidence;
    EXPECT_FALSE(IsDLSSGIdleForPacingEvidence(ce::streamline_runtime_policy::kDLSSGIdleForPacingStreak - 1));
    EXPECT_TRUE(IsDLSSGIdleForPacingEvidence(ce::streamline_runtime_policy::kDLSSGIdleForPacingStreak));
}

TEST(DLSSGIdleStateTest, BothProofsMustAgree) {
    ResetDLSSGIdleState();
    ProveSingleOutputGroups();
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle()) << "single outputs alone are what FG-off looks like";

    ResetDLSSGIdleState();
    g_FGCompat.SetDLSSGHealthIdle(true);
    for (int i = 0; i < 100; ++i) {
        g_FGCompat.NoteRuntimeOutputGroup(3);
    }
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle())
        << "flip-metered MFG reports presented==1 while its callbacks show 3 outputs per frame";

    ProveSingleOutputGroups();
    EXPECT_TRUE(g_FGCompat.IsDLSSGGenerationIdle());
    g_FGCompat.NoteRuntimeOutputGroup(0);
    EXPECT_TRUE(g_FGCompat.IsDLSSGGenerationIdle()) << "no completed group is no evidence either way";
    g_FGCompat.NoteRuntimeOutputGroup(2);
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle()) << "one generated frame ends idleness at once";
    ResetDLSSGIdleState();
}

class DLSSGIdleLimiterTest : public FpsLimiterTest {
protected:
    void SetUp() override {
        FpsLimiterTest::SetUp();
        UseVirtualClock();
        ResetDLSSGIdleState();
        mockShm->runtimeState.captureRequested = true;
        mockShm->runtimeState.isRecording = true;
        mockShm->fpsLimiter.SetCaptureSyncEnabled(true);
        mockShm->fpsLimiter.SetCaptureSyncMultiplier(1);
        mockShm->fpsLimiter.SetCaptureFps(120);
        mockShm->fpsLimiter.SetCaptureSyncLimiterMode(static_cast<uint32_t>(LimiterMode::kBasic));
        g_FGCompat.SetDLSSFGMultiplier(2);
        g_FGCompat.SetDLSSFGActive(true);
        ConfirmDLSSFGPacing();
    }
    void TearDown() override {
        ResetDLSSGIdleState();
        g_FGCompat.SetDLSSFGActive(false);
        FpsLimiterTest::TearDown();
    }
};

TEST_F(DLSSGIdleLimiterTest, AnIdleRuntimeIsPacedAtTheFullTargetAndResumedGenerationHalvesAgain) {
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60) << "producing 2x: half the 120 fps output per frame";

    g_FGCompat.SetDLSSGHealthIdle(true);
    ProveSingleOutputGroups();
    ConfirmDLSSFGPacing();
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 120) << "nothing is generated: every frame is output";

    g_FGCompat.NoteRuntimeOutputGroup(2);
    ConfirmDLSSFGPacing();
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

TEST_F(DLSSGIdleLimiterTest, FlipMeteredGenerationKeepsTheFactor) {
    g_FGCompat.SetDLSSGHealthIdle(true);  // presented == 1 under hardware flip metering
    for (int i = 0; i < 64; ++i) {
        g_FGCompat.NoteRuntimeOutputGroup(2);
    }
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

}  // namespace
