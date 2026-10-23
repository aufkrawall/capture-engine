// FSR frame generation from a tool's own FidelityFX copy (fake runtime, tests/flow/fakes/fidelityfx): the game's
// amd_fidelityfx_framegeneration_dx12.dll is loaded, and a second image of the same name from the tool's folder
// creates the frame-generation swapchain and generates, its exports read from its own export table
// (GameOptions::fsrToolRuntimeCopy). That is OptiScaler with FGOutput=FSRFG (20261023_221541): CE hooked the image
// its name lookup found, never saw the tool's create or configure, kept the protected FFX startup quiesced and drew
// no overlay. Every runtime image's create is observed from its own entry, the creating image's configure from the
// first hook-thread pass on: every presented frame carries the overlay once and the status follows the tool's
// runtime, also on the present-callback route where only the breakpoint sees each configure.

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

void GenerateWithAToolsRuntimeCopy(bool presentCallback) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrToolRuntimeCopy = true;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "the tool's FSR FG on");
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "the tool's FSR FG off");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowFSRToolRuntime, PresentCallbackRouteObservesTheCreatingImage) { GenerateWithAToolsRuntimeCopy(true); }

TEST(FlowFSRToolRuntime, NoCallbackRouteObservesTheCreatingImage) { GenerateWithAToolsRuntimeCopy(false); }

}  // namespace
