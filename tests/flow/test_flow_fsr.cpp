// An FSR frame generation game (fake FidelityFX runtime, tests/flow/fakes/fidelityfx): toggling FSR FG on its
// frame generation swapchain must keep the inject overlay on every presented frame and publish the state,
// with the game's present callback (CE draws inside it) and without one (CE's no-callback routes: the UI
// resource AMD composites onto every output - the game's own, or CE's substitute for a placeholder).

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

GameOptions FidelityFXGameOptions(FSRUiResource ui) {
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = ui;
    return options;
}

void ToggleFSR(FlowGame& game, bool presentCallback, FSRUiResource ui = FSRUiResource::kNone) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(FidelityFXGameOptions(ui))) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "before FSR FG");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on");
        ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 0, 1, "FSR FG off");
    }
    ExpectEveryPresentCoveredOnce(game);
    // Without a present callback CE attributes each output to its game frame; the fake reports the true one.
    if (!presentCallback)
        EXPECT_GT(game.Coverage().outputFrameChecks, 0u) << "logs: " << game.LogDirectory();
    ExpectNoDebugLayerErrors();
}

TEST(FlowFSR, TogglesWithThePresentCallbackKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, true);
}

// testapp/dx12_fg_switch_fsr.cpp: a backbuffer-sized HUD texture; CE draws its overlay onto it.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnTheGamesUiResource) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kFullSize);
}

// GTA V Enhanced: a 1x1 placeholder; CE registers a backbuffer-sized substitute in its place.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnASubstituteUiResource) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kPlaceholder);
}

// No UI resource at all: the HUD is part of the interpolated image.
TEST(FlowFSR, TogglesWithoutAPresentCallbackOrUiResourceKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false);
}

}  // namespace
