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
    ExpectNoDebugLayerErrors();
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
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, TalosStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, true, FSRUiResource::kNone);
}

TEST(FlowSwitch, GTAStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, false, FSRUiResource::kPlaceholder);
}

// Reproduction and fix for the 2,424 consecutive presents without overlay after FSR off:
// When FSR is turned off, the overlay must not disappear. Either through passthrough callback
// or through normal fallback once swapchain and queue are safe, every presented frame must be
// covered once and published status must reflect FG off.
TEST(FlowSwitch, FSROffContinuityCoversEveryPresentAndPublishesOff) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // Enable FSR with callback
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    // Turn FSR OFF: tests overlay continuity across presents
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG off");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Effect-context destroy and recreate while the presentation swapchain remains alive:
// Covers final callback from the departing lifetime, an explicit null callback toggle,
// and rejected configure packets without losing the overlay.
TEST(FlowSwitch, EffectContextRecreateWhileSwapchainAlivePreservesOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 1. Enable FSR
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on before context destroy");

    // 2. Reject an invalid configure packet: must not corrupt bridge or state
    EXPECT_TRUE(game.SendRejectedFSRConfigure());
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();

    // 3. Destroy effect context only while FFX swapchain stays alive
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    // Render frames across departing lifetime: swapchain is still presenting
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();

    // 4. Recreate effect context on the same swapchain and re-enable with null app callback
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Save-load teardown regression: both switch directions followed by temporary FG suspension,
// context recreation, and foreign Present wrapper.
TEST(FlowSwitch, SaveLoadHandoffAndSuspensionKeepsOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrPresentChainOverlayOutputs = 30; // foreign Present wrapper active
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 1. FSR on
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    // 2. Switch FSR -> DLSS
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 1, 2, "FSR -> DLSS");

    // 3. Save-load teardown: temporary FG suspension while in Streamline
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS suspended for save-load");

    // 4. Switch DLSS -> FSR
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "DLSS -> FSR after save-load");

    // 5. Temporary FSR suspension and effect context recreation
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR resumed after context recreation");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
