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

enum class FinalBatchRoute {
    kAnyOwner,       // UI baseline or final-batch route: either draws each output once
    kOwnsTheOutputs  // the proven final-batch route draws every output after AMD composed it
};

void ToggleFSR(FlowGame& game, bool presentCallback, const GameOptions& options, FinalBatchRoute finalBatch) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "before FSR FG");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on");
        if (finalBatch == FinalBatchRoute::kOwnsTheOutputs)
            ExpectFinalBatchRouteOwnsTheOutputs(game, cycle == 0 ? "first FSR FG cycle" : "second FSR FG cycle");
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

void ToggleFSR(FlowGame& game, bool presentCallback, FSRUiResource ui = FSRUiResource::kNone,
               FSRUiBuffering buffering = FSRUiBuffering::kSwapchainCopy,
               FinalBatchRoute finalBatch = FinalBatchRoute::kAnyOwner) {
    ToggleFSR(game, presentCallback, FidelityFXGameOptions(ui, buffering), finalBatch);
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

TEST(FlowFSR, AStackOfQueueProxiesKeepsTheOverlayOnStartupAndGeneratedOutputs) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.fsrWrappedGameQueue = true;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// Talos + OptiScaler (session 20261024_015207): AMD presented the first outputs after each enable from the game
// thread, and Steam's and OptiScaler's start-up messages submitted after AMD's composition and then stopped. The route
// proven on the game thread or anchored on such a message left 7 outputs without the overlay; every output must keep
// exactly one draw, and the route must still own the outputs once the runtime presents from its own thread.
TEST(FlowFSR, PresenterMigrationAndAPresentChainOverlayThatStopsLeaveNoOutputUncovered) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kEmpty, FSRUiBuffering::kSwapchainCopy);
    options.fsrPassthroughFramesAfterEnable = 8;
    options.fsrPresentChainOverlayOutputs = 120;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

TEST(FlowFSR, RelayedCompositionKeepsTheOverlayAtOutputRateWhenAForeignOverlayStops) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.fsrRelayOutputSubmissions = true;
    options.fsrPresentChainOverlayOutputs = 120;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// GTA V Enhanced with ReShade after DLSS FG -> FSR FG (session gtafsroverlaystutteryghosty): ReShade's queue proxy
// relays AMD's composition, and the swapchain AMD presents to sits under Social Club, Epic's overlay, Streamline and
// ReShade. The runtime's Present call lay beyond the 20 frames CE searched, so no batch could be told apart from the
// Present chain's, the final-batch route never armed and the overlay stayed in the UI resource at the rendered rate.
TEST(FlowFSR, RelayedCompositionUnderADeepPresentChainKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kSwapchainCopy);
    options.fsrRelayOutputSubmissions = true;
    options.fsrPresentChainDepth = 40;
    options.fsrPresentChainOverlayOutputs = 120;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// GTA V Enhanced + ReShade, DLSS FG -> FSR FG (gtadlssfgtofsrfgcrash, 0.1.7513): AMD's last presenter-thread batch,
// relayed by ReShade's queue proxy, ran on a DIRECT queue the real swapchain was not created on. CE anchored there and
// its first draw onto the back buffer removed the device (ERR_GFX_STATE). The anchor stays on the composition on the
// swapchain's own queue; the debug layer reports any back-buffer write from another queue (id 907).
TEST(FlowFSR, ARelayedRuntimeBatchOnAnotherQueueNeverCarriesTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kSwapchainCopy);
    options.fsrRelayOutputSubmissions = true;
    options.fsrSideQueueBatch = true;
    options.fsrPresentChainDepth = 40;
    options.fsrPresentChainOverlayOutputs = 120;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// The same side batch submitted by the runtime image itself: a direct runtime batch on another queue is no anchor
// either.
TEST(FlowFSR, ADirectRuntimeBatchOnAnotherQueueNeverCarriesTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.fsrSideQueueBatch = true;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// GTA V Enhanced + ReShade after DLSS FG -> FSR FG (session 20261112_013802): AMD creates its swapchain through
// Streamline, whose sl.dlss_g.dll creates the native one on a queue of its own and presents it from inside AMD's Present.
// AMD composes into the plugin's buffers on AMD's queue; the plugin copies its image into the back buffer on the
// swapchain's queue through ReShade's queue object, then ReShade draws its effects. Every batch on that queue lay inside
// AMD's Present call, so the final-batch route never armed and the overlay stayed in the UI resource at the rendered
// rate (a juddering graph). The plugin's own copy is the anchor; ReShade's and DXGI's work below it never is.
TEST(FlowFSR, ANestedPresenterUnderReShadeKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kPlaceholder, FSRUiBuffering::kSwapchainCopy);
    options.streamline = true;
    options.fsrNestedPresenter = true;
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// The same nested presenter submitting its copy on the native queue itself, with no queue proxy or overlay around it.
TEST(FlowFSR, ANestedPresenterSubmittingDirectlyKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.streamline = true;
    options.fsrNestedPresenter = true;
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// GTA V Enhanced with ReShade, as a stack of real-shaped overlays (fakes/overlay/present_chain_layer.cpp): Rockstar's
// and Epic's overlays and ReShade draw at every Present of AMD's real swapchain, ReShade's queue objects relay AMD's
// composition and the game's submissions, and the game composes a full-size HUD.
TEST(FlowFSR, GTAsOverlayStackWithReShadeKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kSwapchainCopy);
    options.presentChainOverlays = {{OverlayStandIn::kRockstar, 0}, {OverlayStandIn::kEpic, 0},
                                    {OverlayStandIn::kReShade, 0}};
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// Ubisoft Connect's overlay draws at every Present, Steam's start-up toast at the first 120 only, over a game without a
// UI resource: the final-batch route anchors on the runtime's batch beneath both and keeps it when the toast stops.
TEST(FlowFSR, UbisoftAndSteamOverlaysOverTheRuntimesSwapchainLeaveNoOutputUncovered) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.presentChainOverlays = {{OverlayStandIn::kUbisoft, 0}, {OverlayStandIn::kSteam, 120}};
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

// With the game's present callback CE draws inside it; ReShade's effects follow at Present.
TEST(FlowFSR, PresentCallbackUnderReShadeKeepsTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options = FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy);
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ToggleFSR(game, true, options, FinalBatchRoute::kAnyOwner);
}

GameOptions HDR10(GameOptions options) {
    options.backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    return options;
}

GameOptions ScRGB(GameOptions options) {
    options.backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    return options;
}

// HDR output under FSR FG: every route draws into a target of another format than the SDR default - the game's
// R8G8B8A8 HUD over an HDR swapchain, CE's substitute, AMD's HDR outputs - through pipelines of that target's format.
TEST(FlowFSR, HDR10WithoutAPresentCallbackKeepsTheOverlayOnTheGamesUiResource) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, HDR10(FidelityFXGameOptions(FSRUiResource::kFullSize, FSRUiBuffering::kSwapchainCopy)),
              FinalBatchRoute::kAnyOwner);
}

TEST(FlowFSR, HDR10WithoutAPresentCallbackOrUiResourceDrawsEveryOutputAfterComposition) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, HDR10(FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy)),
              FinalBatchRoute::kOwnsTheOutputs);
}

TEST(FlowFSR, ScRGBWithThePresentCallbackKeepsTheOverlay) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, true, ScRGB(FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy)),
              FinalBatchRoute::kAnyOwner);
}

TEST(FlowFSR, HDR10UnderReShadeAndLauncherOverlaysKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options = HDR10(FidelityFXGameOptions(FSRUiResource::kNone, FSRUiBuffering::kSwapchainCopy));
    options.presentChainOverlays = {{OverlayStandIn::kRockstar, 0}, {OverlayStandIn::kEpic, 0},
                                    {OverlayStandIn::kReShade, 0}};
    ToggleFSR(game, false, options, FinalBatchRoute::kOwnsTheOutputs);
}

TEST(FlowFSR, ScRGBWithoutAPresentCallbackKeepsTheOverlayOnASubstituteUiResource) {
    FlowGame game(CurrentTestName());
    ToggleFSR(game, false, ScRGB(FidelityFXGameOptions(FSRUiResource::kPlaceholder, FSRUiBuffering::kSwapchainCopy)),
              FinalBatchRoute::kAnyOwner);
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
