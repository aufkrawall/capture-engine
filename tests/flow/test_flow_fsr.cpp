// An FSR frame generation game (fake FidelityFX runtime, tests/flow/fakes/fidelityfx): toggling FSR FG on its
// frame generation swapchain must keep the inject overlay on every presented frame and publish the state,
// with the game's present callback (CE draws inside it) and without one (CE's no-callback routes: the UI
// resource AMD composites onto every output - the game's own, or CE's substitute for a placeholder).

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

GameOptions FidelityFXGameOptions(FSRUiResource ui, FSRUiBuffering buffering) {
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = ui;
    options.fsrUiBuffering = buffering;
    return options;
}

void ToggleFSR(FlowGame& game, bool presentCallback, FSRUiResource ui = FSRUiResource::kNone,
               FSRUiBuffering buffering = FSRUiBuffering::kSwapchainCopy) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(FidelityFXGameOptions(ui, buffering))) << game.Error();
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

// Without AMD's UI copy the game keeps each frame's UI intact itself, here by alternating two textures.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnAlternatingUiResources) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kFullSize, FSRUiBuffering::kGameAlternates);
}

// One texture without AMD's UI copy: AMD reads it while the game rewrites it for the next frame.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnASingleLiveUiResource) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kFullSize, FSRUiBuffering::kSingleTexture);
}

// A placeholder without AMD's UI copy: CE's substitute must keep each frame's overlay itself.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnASubstituteWithoutUiCopy) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kPlaceholder, FSRUiBuffering::kGameAlternates);
}

// The game changes its present callback while FSR FG stays on: AMD composes the frames presented before the change
// the old way, the frames after it the new way. Without a callback first (CE's no-callback routes, the topmost route
// owning the frames), then with one (CE's bridge), then null again (AMD keeps calling CE's bridge, which delegates to
// the game's previous callback).
TEST(FlowFSR, PresentCallbackChangesWithFrameGenerationOnKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(
        FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kSwapchainCopy)))
        << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on, present callback");
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on, no present callback");
    }
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
