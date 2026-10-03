// Switching between frame generation runtimes the way the dx12_fg_switch_test app and GTA/Talos do: the
// device and queue stay, the swapchain is replaced by the next runtime's. The overlay must stay on every
// presented frame across each switch, with the published state following the runtime.

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

// 0.1.6951, dx12_fg_switch_test (session 20261003_070202, 07:05:29-07:05:42): after an FSR phase, DLSS-G on,
// off (explicit slDLSSGSetOptions, make-before-break keep-alive) and on again 1.5 s later lost the overlay for
// 1549 presents / 6.5 s: "PostSL SKIP - FG transition cooldown active (60 frames left)" the whole time. Here the
// scenario also caught the 90-present blank on the FSR -> Streamline swapchain change and a use-after-free in
// CE's post-FSR queue probe that removed the device.
TEST(FlowSwitch, PostFSRDLSSWarmResumeKeepsTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    for (int cycle = 0; cycle < 3; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
        ASSERT_TRUE(game.SetFSRFrameGeneration(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    }
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(500)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on after FSR");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(210)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS FG off after FSR");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(900)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG warm resume");
    ExpectEveryPresentCoveredOnce(game);
}

// The menu switches of the 0.1.6951 runs, each in one frame: FSR FG on -> DLSS FG on (the FidelityFX swapchain
// replaced by Streamline's), DLSS FG on -> FSR FG on, FSR FG on -> all off (a native swapchain). Talos composes
// FSR FG through the present callback, GTA without one over a 1x1 UI placeholder.
void SwitchRuntimesWithFrameGenerationOn(FlowGame& game, bool presentCallback, FSRUiResource ui) {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = ui;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "FSR FG -> DLSS FG");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "DLSS FG -> FSR FG");

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG -> all off");
    ExpectEveryPresentCoveredOnce(game);
}

TEST(FlowSwitch, TalosStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, true, FSRUiResource::kNone);
}

TEST(FlowSwitch, GTAStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, false, FSRUiResource::kPlaceholder);
}

}  // namespace
