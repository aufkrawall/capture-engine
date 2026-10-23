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
