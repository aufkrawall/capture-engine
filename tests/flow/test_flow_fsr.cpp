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

// CE's own account of the overlay submission on this thread (CEFlow_DescribeD3D12OverlaySubmission).
std::string DescribeOverlaySubmission() {
    using Describe = int (*)(char*, int);
    const auto describe = reinterpret_cast<Describe>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_DescribeD3D12OverlaySubmission"));
    char text[512] = {};
    if (!describe || describe(text, static_cast<int>(sizeof(text))) <= 0)
        return "CEFlow_DescribeD3D12OverlaySubmission unavailable";
    return text;
}

enum class FinalBatchRoute {
    kAnyOwner,       // UI baseline or final-batch route: either draws each output once
    kOwnsTheOutputs  // the proven final-batch route draws every output after AMD composed it
};

void ToggleFSR(FlowGame& game, bool presentCallback, FSRUiResource ui = FSRUiResource::kNone,
               FSRUiBuffering buffering = FSRUiBuffering::kSwapchainCopy,
               FinalBatchRoute finalBatch = FinalBatchRoute::kAnyOwner) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(FidelityFXGameOptions(ui, buffering))) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "before FSR FG");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on");
        if (finalBatch == FinalBatchRoute::kOwnsTheOutputs) {
            const std::string state = DescribeOverlaySubmission();
            EXPECT_NE(state.find("granted=1"), std::string::npos)
                << "cycle " << cycle << ": " << state << "; logs: " << game.LogDirectory();
        }
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

// No UI resource at all: the HUD is part of the interpolated image. The proxy backbuffer carries the overlay only
// until the final-batch route is proven; drawn there for good, AMD would interpolate it and every output would
// show it as of the last rendered frame (Talos + OptiScaler, session 20261024_010934).
TEST(FlowFSR, TogglesWithoutAPresentCallbackOrUiResourceKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy, FinalBatchRoute::kOwnsTheOutputs);
}

// OptiScaler feeding DLSS-G inputs to FSR FG registers a UI resource without a resource every frame.
TEST(FlowFSR, TogglesWithoutAPresentCallbackAndAnEmptyUiResourceDrawEveryOutputAfterComposition) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kEmpty, FSRUiBuffering::kSwapchainCopy, FinalBatchRoute::kOwnsTheOutputs);
}

// Without AMD's UI copy the game keeps each frame's UI intact itself, here by alternating two textures.
TEST(FlowFSR, TogglesWithoutAPresentCallbackKeepTheOverlayOnAlternatingUiResources) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, FSRUiResource::kFullSize, FSRUiBuffering::kGameAlternates);
}

// A debug-layer error in a scenario carries CE's own account of the submitting thread (flow_host_debug_layer.cpp):
// the route in progress, CE's queue and the topmost route's learned target. Here it must report that route live.
TEST(FlowFSR, ValidationMessagesCarryCEsSubmissionState) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(
        FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kGameAlternates)))
        << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    const std::string state = DescribeOverlaySubmission();
    EXPECT_NE(state.find("ceSubmission=none"), std::string::npos) << state;  // the game thread, between frames
    EXPECT_NE(state.find("ready=1"), std::string::npos) << state;
    EXPECT_NE(state.find("granted=1"), std::string::npos) << state;
    EXPECT_EQ(state.find("queue=0000000000000000"), std::string::npos) << state;
    ExpectNoDebugLayerErrors();
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
