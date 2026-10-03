// A DLSS frame generation game (fake Streamline runtime, tests/flow/fakes/streamline): toggling DLSS-G
// must keep the inject overlay on every presented frame and publish the frame-generation state the
// runtime is in. Frame counts are at 144 FPS on CE's virtual clock (600 frames = 4.2 s).

#include "tests/flow/flow_test_support.h"

namespace {

using ce::flow::CurrentTestName;
using ce::flow::ExpectEveryPresentCoveredOnce;
using ce::flow::FlowGame;
using ce::flow::GameOptions;
using ce::flow::SwapchainKind;

GameOptions StreamlineGameOptions() {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    return options;
}

// A menu toggle seconds apart, as in the 0.1.6951 Talos/GTA runs.
TEST(FlowDLSS, MenuTogglesKeepTheOverlayAndPublishTheRuntimeState) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    EXPECT_LT(game.PublishedFG().multiplier, 2);

    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        EXPECT_TRUE(game.DLSSFrameGenerationRunning()) << "cycle " << cycle;
        EXPECT_EQ(game.PublishedFG().type, 1) << "cycle " << cycle << "; logs: " << game.LogDirectory();
        EXPECT_EQ(game.PublishedFG().multiplier, 2) << "cycle " << cycle;

        ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
        EXPECT_FALSE(game.DLSSFrameGenerationRunning()) << "cycle " << cycle;
        EXPECT_LT(game.PublishedFG().multiplier, 2) << "cycle " << cycle << "; logs: " << game.LogDirectory();
    }
    ExpectEveryPresentCoveredOnce(game);
}

// CE holds an OFF that arrives inside DLSS-G's startup window (GTA's startup churn) - but only until the
// protection ends: it is the title's latest request and must reach the runtime and the published state.
TEST(FlowDLSS, OffInsideTheStartupWindowIsHeldThenHonored) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(720)) << game.Error();
    EXPECT_FALSE(game.DLSSFrameGenerationRunning()) << "logs: " << game.LogDirectory();
    EXPECT_LT(game.PublishedFG().multiplier, 2) << "logs: " << game.LogDirectory();
    ExpectEveryPresentCoveredOnce(game);
}

}  // namespace
