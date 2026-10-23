// FSR frame generation from a tool's own FidelityFX copy (fake runtime, tests/flow/fakes/fidelityfx): the game's
// amd_fidelityfx_framegeneration_dx12.dll is loaded, and a second image of the same name from the tool's folder
// creates the frame-generation swapchain and generates, its exports read from its own export table
// (GameOptions::fsrToolRuntimeCopy). That is OptiScaler with FGOutput=FSRFG (20261023_221541): CE hooked the image
// its name lookup found, never saw the tool's create or configure, kept the protected FFX startup quiesced and drew
// no overlay. Every runtime image's create is observed from its own entry, the creating image's configure from the
// first hook-thread pass on: every presented frame carries the overlay once and the status follows the tool's
// runtime, also on the present-callback route where only the breakpoint sees each configure.

#include "tests/flow/flow_test_support.h"

#include <fstream>
#include <iterator>

namespace {

using namespace ce::flow;

void GenerateWithAToolsRuntimeCopy(bool presentCallback, bool ffxInputs = false) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrToolRuntimeCopy = true;
    options.fsrToolFFXInputs = ffxInputs;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "the tool's FSR FG on");
    ASSERT_TRUE(game.ResizeSwapchain(720, 400)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    if (ffxInputs) {
        ASSERT_TRUE(game.DetachForeignFFXInputs()) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG after the tool detached its input hooks");
    }
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "the tool's FSR FG off");
    if (ffxInputs) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG re-enabled after input hook detach");
        ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        ExpectPublished(game, 0, 1, "FSR FG disabled again");
        ASSERT_TRUE(game.ReloadForeignFFXRuntime()) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG after the tool runtime was unloaded and recreated");
        ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    }

    if (ffxInputs) {
        std::ifstream log(game.LogDirectory() + "/hook_debug.log");
        ASSERT_TRUE(log.is_open());
        const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        EXPECT_NE(text.find("native ffxConfigure observer installed below foreign entry"), std::string::npos);
        EXPECT_EQ(text.find("Keeping the ffxConfigure breakpoint armed"), std::string::npos);
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowFSRToolRuntime, PresentCallbackRouteObservesTheCreatingImage) { GenerateWithAToolsRuntimeCopy(true); }

TEST(FlowFSRToolRuntime, NoCallbackRouteObservesTheCreatingImage) { GenerateWithAToolsRuntimeCopy(false); }

TEST(FlowFSRToolRuntime, ForeignFFXInputsPreserveThePresentCallbackRoute) { GenerateWithAToolsRuntimeCopy(true, true); }

TEST(FlowFSRToolRuntime, ForeignFFXInputsPreserveTheNoCallbackRoute) { GenerateWithAToolsRuntimeCopy(false, true); }

}  // namespace
