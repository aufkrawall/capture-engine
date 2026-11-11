// Which runtime presents the window decides who may take the overlay route (ce::fg_presenter).
//
// GTA V Enhanced 0.1.7500 DLSS FG -> FSR FG (session 20261110_222057): the title unloaded DLSS-G, its proxy released the
// native chain and FidelityFX created the window's replacement chain (DXGI reused the address). The FFX swapchain sat in
// its disabled startup (ffxConfigure with generation off) while the title kept passing its last DLSS-G options, mode
// on, to every slDLSSGGetState poll. Once CE's 250 ms post-takeover suppression lapsed, one of those polls published
// DLSS FG, retired the protected FidelityFX startup and armed PostSL; the first PostSL submit drew on the FidelityFX
// chain through the previous Streamline queue and the device was removed (DXGI_ERROR_ACCESS_DENIED).

#include "tests/flow/flow_test_support.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace ce::flow;

std::string ReadHookLog(const FlowGame& game) {
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    return std::string{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
}

constexpr int kDLSSGModeOff = 0;
constexpr int kDLSSGModeOn = 1;

// GTA's DLSS FG -> FSR FG order: DLSS-G off and unloaded while its proxy lives, then the FidelityFX chain for the
// window, first configured with generation off. The title's GetState keeps carrying mode on throughout.
void SwitchDLSSToDisabledFidelityFXStartup(FlowGame& game) {
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/false)) << game.Error();
}

// FSR FG -> DLSS FG as GTA does it: DLSS-G loaded again before its chain replaces the FidelityFX one.
void SwitchFidelityFXToDLSS(FlowGame& game) {
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.LoadDLSSG()) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
}

TEST(FlowFGPresenterOwnership, StaleDLSSGGetStateDuringFidelityFXStartupLeavesTheFidelityFXChainAlone) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.fsrUi = FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.SetDLSSGGetStateOptions(kDLSSGModeOn)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();

    for (int cycle = 0; cycle < 2; ++cycle) {
        SCOPED_TRACE(cycle);
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(240)) << game.Error();
        ExpectPublished(game, 1, 2, "DLSS FG on");

        SwitchDLSSToDisabledFidelityFXStartup(game);
        // 300 frames are about 2 s on the virtual clock: well past every time-bounded suppression after the takeover.
        ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
        ExpectPublished(game, 0, 1, "FidelityFX startup with generation off and a stale DLSS-G GetState(ON)");
        EXPECT_FALSE(game.PostSLLifecycle().callbacksEnabled)
            << "PostSL admitted on the FidelityFX chain; logs: " << game.LogDirectory();

        ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(240)) << game.Error();
        ExpectPublished(game, 2, 2, "FSR FG on after the stale DLSS-G state");

        SwitchFidelityFXToDLSS(game);
        ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();

    const std::string text = ReadHookLog(game);
    EXPECT_NE(text.find("is a front"), std::string::npos) << game.LogDirectory();
}

// The same stale GetState(ON) with DLSS-G loaded and its own chain presenting is DLSS-G's real state: a title that
// enables DLSS-G where slDLSSGSetOptions never shows it still gets DLSS FG, also after an FSR phase.
TEST(FlowFGPresenterOwnership, GetStateOnlyDLSSGOnItsOwnChainIsStillAccepted) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.fsrUi = FSRUiResource::kPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.SetDLSSGGetStateOptions(kDLSSGModeOff)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.SetDLSSGGetStateOptions(kDLSSGModeOn)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(240)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on");

    SwitchDLSSToDisabledFidelityFXStartup(game);
    ASSERT_TRUE(game.SetDLSSGGetStateOptions(kDLSSGModeOff)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(240)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    SwitchFidelityFXToDLSS(game);
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS-G chain presenting with generation off");
    ASSERT_TRUE(game.SetDLSSFrameGenerationUnobserved(true)) << game.Error();
    ASSERT_TRUE(game.SetDLSSGGetStateOptions(kDLSSGModeOn)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "GetState-only DLSS FG on its own chain");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
