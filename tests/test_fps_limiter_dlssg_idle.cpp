#include "test_fps_limiter_shared.h"

#include "hook/ngx/dlssg_health_policy.h"

// DLSS-G enabled but generating nothing (menus, loading). Talos 20261019_182822: the runtime reported
// `[DLSSG HEALTH] ON but NOT interpolating` for 7 s while the limiter kept `fg=1/2x fgProof=d3d-sleep`
// and the game ran - and recorded - at 60 fps under a 120 fps capture-sync cap.
namespace {

using ce::fg_runtime::ApplicationFrameSource;
using ce::fg_runtime::OutputRatioWindow;

// A DLSS FG on/off edge resets both proofs (FGCompatibility::SetDLSSFGActive); two edges restore the
// API state the test found.
void ResetDLSSGIdleState() {
    g_FGCompat.SetDLSSGHealthIdle(false);
    const bool active = g_FGCompat.IsDLSSFGApiActive();
    g_FGCompat.SetDLSSFGActive(!active);
    g_FGCompat.SetDLSSFGActive(active);
}

void RunFrames(uint32_t frames, uint32_t outputsPerFrame,
               ApplicationFrameSource source = ApplicationFrameSource::kPresentHook) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t output = 0; output < outputsPerFrame; ++output) {
            g_FGCompat.NoteRuntimeOutput();
        }
        g_FGCompat.NoteApplicationFrame(source);
    }
}

void ProveSingleOutputGroups() {
    RunFrames(OutputRatioWindow::kWindowFrames, 1);
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
    RunFrames(OutputRatioWindow::kWindowFrames * 3, 3);
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle())
        << "flip-metered MFG reports presented==1 while its callbacks show 3 outputs per frame";

    ProveSingleOutputGroups();
    EXPECT_TRUE(g_FGCompat.IsDLSSGGenerationIdle());
    RunFrames(OutputRatioWindow::kWindowFrames / 4 + 1, 2);
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle()) << "resumed generation ends idleness within a few frames";
    ResetDLSSGIdleState();
}

// Talos 20261019_182822 after FSR FG: the game's Present into Streamline never reached the DXGI hook, so
// only the title's PresentStart markers bound its frames. Once they flow, present-hook frames do not
// count as well: two boundaries per frame would halve 2x generation into a fake one-output ratio.
TEST(DLSSGIdleStateTest, TitleMarkersProveIdlenessWhereThePresentHookSeesNothing) {
    ResetDLSSGIdleState();
    g_FGCompat.SetDLSSGHealthIdle(true);
    RunFrames(OutputRatioWindow::kWindowFrames, 1, ApplicationFrameSource::kTitleMarker);
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle()) << "the first marker restarts the window";
    RunFrames(OutputRatioWindow::kWindowFrames, 1, ApplicationFrameSource::kTitleMarker);
    EXPECT_TRUE(g_FGCompat.IsDLSSGGenerationIdle());

    for (uint32_t frame = 0; frame < OutputRatioWindow::kWindowFrames * 2; ++frame) {
        g_FGCompat.NoteRuntimeOutput();
        g_FGCompat.NoteRuntimeOutput();
        g_FGCompat.NoteApplicationFrame(ApplicationFrameSource::kPresentHook);
        g_FGCompat.NoteApplicationFrame(ApplicationFrameSource::kTitleMarker);
    }
    EXPECT_FALSE(g_FGCompat.IsDLSSGGenerationIdle()) << "2x with both boundaries per frame is not idle";
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

    RunFrames(OutputRatioWindow::kWindowFrames / 4 + 1, 2);
    ConfirmDLSSFGPacing();
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

TEST_F(DLSSGIdleLimiterTest, FlipMeteredGenerationKeepsTheFactor) {
    g_FGCompat.SetDLSSGHealthIdle(true);  // presented == 1 under hardware flip metering
    RunFrames(OutputRatioWindow::kWindowFrames * 2, 2);
    limiter.Apply();
    EXPECT_EQ(limiter.GetResolvedCadence().targetFps, 60);
}

}  // namespace
