// A tool serving the game's DLSS-G API in Streamline's place (fake tool module, tests/flow/fakes/tool), the two
// OptiScaler runs of session 20261023_221541:
// - the tool wraps a DLSS-G that runs (Streamline's proxy presents every frame): DLSS FG must be shown and drawn
//   like any DLSS-G. CE had taken the foreign-served API alone for a front and kept DLSS FG off; the overlay was gone
//   once generation started.
// - the tool answers the calls itself and generates with its own FidelityFX copy: the game's DLSS-G "enable" is a
//   front, never DLSS FG, and must not retire the FFX swapchain's startup. CE had retired it, drawn the FFX swapchain
//   from the game queue and removed the device (0x887A002B).

#include "tests/flow/flow_test_support.h"

#include <fstream>
#include <iterator>

namespace {

using namespace ce::flow;

TEST(FlowDLSSGApiTool, AToolWrappingARunningDLSSGStaysDLSSFG) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.dlssgApiTool = DLSSGApiTool::kForwards;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on through the tool's wrapper");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS FG off through the tool's wrapper");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSSGApiTool, AnEnableTheToolAnswersLeavesItsFSRFrameGeneration) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrToolRuntimeCopy = true;
    options.dlssgApiTool = DLSSGApiTool::kAnswers;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    // The game turns "DLSS-G" on; the tool answers and starts its FSR frame generation.
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ExpectPublished(game, 0, 1, "the tool's DLSS-G enable, no generation yet");
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "the tool's FSR FG");
    // The title sends its DLSS-G options every frame even though the tool generates with FSR. These are inputs,
    // never a request to tear down the live FSR renderer or begin a DLSS-G UI activation.
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    }
    ExpectPublished(game, 2, 2, "the repeated DLSS-G inputs leave FSR FG active");
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_EQ(text.find("Preparing for Streamline FG enable while live FSR runtime owns the swapchain"), std::string::npos);
    EXPECT_EQ(text.find("Streamline UI bootstrap overlay armed"), std::string::npos);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "the tool's FSR FG off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The game enables DLSS-G through the tool's wrapper before its first frame (a saved setting). Until Streamline
// presents, the signal is indistinguishable from a front and CE holds it; Streamline's first present proves the
// DLSS-G real. CE had saved the request and never applied it: DLSS-G generated while CE published FG off.
TEST(FlowDLSSGApiTool, AWrappedDLSSGEnabledBeforeTheFirstPresentIsDLSSFG) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.dlssgApiTool = DLSSGApiTool::kForwards;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG enabled before Streamline's first present");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS FG off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The tool wraps a running DLSS-G, then the game switches to the tool's FSR FG (the tool answers the DLSS-G calls
// from then on) and keeps sending its DLSS-G options every frame. Streamline's presents of the last window were on
// the swapchain FFX replaced: these inputs are a front from the FFX create on. CE had read them as a running DLSS-G
// for 240 more presents, torn the FSR route down and never published FSR FG.
TEST(FlowDLSSGApiTool, AToolsFSRAfterItsWrappedDLSSGStaysFSRUnderDLSSGInputs) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.dlssgApiTool = DLSSGApiTool::kForwards;
    options.fsrToolRuntimeCopy = true;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG through the tool's wrapper");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(SetEnvironmentVariableA("CE_FLOW_DLSSG_API_TOOL_MODE", "answer"));
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    game.ServiceHookThread();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    }
    ExpectPublished(game, 2, 2, "the tool's FSR FG right after its wrapped DLSS-G");
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 2, 2, "the tool's FSR FG settled");
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_EQ(text.find("Preparing for Streamline FG enable while live FSR runtime owns the swapchain"), std::string::npos);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
