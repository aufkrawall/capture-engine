// Switching between Intel XeSS frame generation and the other frame generation runtimes the way a game's menu
// does: the device and queue stay, the swapchain is replaced by the next runtime's in one frame (XeFG's context is
// destroyed when the game leaves it). The overlay must stay on every presented frame across each switch, in every
// direction, with the published status following the runtime: XeFG <-> FSR FG (with and without FSR's present
// callback), XeFG <-> DLSS FG, and XeFG -> all off.

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

constexpr int kXeFGStatusType = 4;  // ce::overlay_metrics::FGMetricType::kXeFG

void SwitchBetweenXeFGAndTheOtherRuntimes(bool fsrPresentCallback, FSRUiResource fsrUi) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kXeFG;
    options.fsrUi = fsrUi;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, fsrPresentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "XeFG -> FSR FG");

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, fsrPresentCallback)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "FSR FG -> XeFG");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "XeFG -> DLSS FG");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 4, "DLSS FG -> XeFG 4x");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 0, 1, "XeFG -> all off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowXeFGSwitch, TalosStyleSwitchesKeepTheOverlay) {
    SwitchBetweenXeFGAndTheOtherRuntimes(true, FSRUiResource::kNone);
}

TEST(FlowXeFGSwitch, GTAStyleSwitchesKeepTheOverlay) {
    SwitchBetweenXeFGAndTheOtherRuntimes(false, FSRUiResource::kPlaceholder);
}

// Talos (20261104_183540): the first ProcessFrame on XeFG's chain saw Streamline's FG gone, 21 s late, and armed the
// 300-frame grace against heuristic false positives. It trusted only FSR's API, so a generating XeFG read as off for
// exactly 300 frames. SetEnabled with XeFG's own swapchain registered is as explicit as ffxConfigure.
TEST(FlowXeFGSwitch, TheStatusFollowsXeFGRightAfterDLSSFG) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "30 frames after DLSS FG -> XeFG");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

constexpr int kFSRStatusType = 2;  // ce::overlay_metrics::FGMetricType::kFSR

void RenderFramesNeverReadingAsFSR(FlowGame& game, int frames, const char* where) {
    for (int frame = 0; frame < frames; ++frame) {
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
        ASSERT_NE(game.PublishedFG().type, kFSRStatusType)
            << where << ", frame " << frame << "; logs: " << game.LogDirectory();
    }
}

// Talos (20261104_195245), XeFG the first frame generation of the run: the first Present on XeFG's chain read the new
// queue as FSR FG (`FG detected via queue change`) and published "FSR FG 2x" for that present. The way back from that
// false transition then cleared the queue ownership XeFG had just been given. XeFG's queue says nothing about FSR FG,
// neither when the game arrives on it nor when the game leaves it.
TEST(FlowXeFGSwitch, XeFGAsTheFirstRuntimeNeverReadsAsFSRFG) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "Streamline without FG");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
        RenderFramesNeverReadingAsFSR(game, 30, "XeFG's chain, generation off");
        ExpectPublished(game, 0, 1, "XeFG's chain, generation off");
        ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
        RenderFramesNeverReadingAsFSR(game, 300, "XeFG generating");
        ExpectPublished(game, kXeFGStatusType, 2, "XeFG generating");
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
        RenderFramesNeverReadingAsFSR(game, 300, "back on Streamline's chain");
        ExpectPublished(game, 0, 1, "back on Streamline's chain");
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The game tries XeFG's chain and leaves before XeFG generates anything (Talos took 1.4 s to start). The false FSR
// transition used to be the only FG transition of such a trip; now there is none, and the overlay has to follow the
// new chain and queue both ways by itself while the status stays off.
TEST(FlowXeFGSwitch, ATripThroughXeFGWithoutGenerationKeepsTheOverlayAndReadsOff) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
        RenderFramesNeverReadingAsFSR(game, 30, "XeFG's chain, never generating");
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
        RenderFramesNeverReadingAsFSR(game, 150, "the game's next chain");
        ExpectPublished(game, 0, 1, "the game's next chain");
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// OptiScaler with FGInput=DLSSG and FGOutput=XeFG: the game keeps asking Streamline for DLSS-G while XeFG presents
// every frame (session 20261023_183119 published "DLSS FG 2x" and drew on half the frames). A DLSS-G signal beside a
// runtime-presented swapchain is a front: the status stays XeFG, and a DLSS-G state taken before XeFG appeared is
// retired when it does.
TEST(FlowXeFGSwitch, AStreamlineFGSignalBesideXeFGIsAFront) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kNative;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "DLSS-G signal on before XeFG");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 3)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 4, "DLSS-G signal on while XeFG presents");

    ASSERT_TRUE(game.SetXeFGFrameGeneration(false, 3)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
    ExpectPublished(game, 0, 1, "XeFG off, DLSS-G signal still on");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
