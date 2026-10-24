// An Intel XeSS frame generation game (fake libxess_fg.dll, tests/flow/fakes/xefg): XeFG creates and presents a
// native swapchain of its own, generated frames from its presenter thread on its own queue and the real frame on the
// game thread. The inject overlay must be on every one of those outputs, drawn once each, on XeFG's queue, and the
// overlay must show "XeFG Nx" while generation is on - through toggles, multi-frame generation, resize, the
// InitFromSwapChain handover and swapchain recreation.

#include "tests/flow/flow_test_support.h"

#include <filesystem>

namespace {

using namespace ce::flow;

constexpr int kXeFGStatusType = 4;  // ce::overlay_metrics::FGMetricType::kXeFG

GameOptions XeFGGameOptions(bool fromSwapchain = false) {
    GameOptions options;
    options.swapchain = SwapchainKind::kXeFG;
    options.xefgFromSwapchain = fromSwapchain;
    return options;
}

void ToggleXeFG(FlowGame& game, uint32_t interpolatedFrames, FlowGame::PresentMethod method) {
    ASSERT_TRUE(game.RenderFrames(300, method)) << game.Error();
    ExpectPublished(game, 0, 1, "before XeFG");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetXeFGFrameGeneration(true, interpolatedFrames)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600, method)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, static_cast<int>(interpolatedFrames) + 1, "XeFG on");
        ASSERT_TRUE(game.SetXeFGFrameGeneration(false, interpolatedFrames)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(600, method)) << game.Error();
        ExpectPublished(game, 0, 1, "XeFG off");
    }
}

TEST(FlowXeFG, TogglesKeepTheOverlayOnEveryOutput) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowXeFG, Present1TogglesKeepTheOverlayOnEveryOutput) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent1);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Multi-frame generation: three outputs per game frame from XeFG's presenter thread, then the real one.
TEST(FlowXeFG, MultiFrameGenerationShowsItsFactorAndCoversEveryOutput) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ToggleXeFG(game, 3, FlowGame::PresentMethod::kPresent);
    for (uint32_t frames : {2u, 1u, 3u}) {
        ASSERT_TRUE(game.SetXeFGFrameGeneration(true, frames)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, static_cast<int>(frames) + 1, "XeFG factor change");
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// xefgSwapChainD3D12InitFromSwapChain fails unless the game's swapchain reference is the last one: CE must hold
// none of its own across the call (the fake refuses otherwise, as the SDK does).
TEST(FlowXeFG, InitFromTheGamesSwapchainSucceedsWithCEInjected) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions(true))) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The game resizes XeFG's proxy; XeFG rebinds the native swapchain to its own queue again (ResizeBuffers1).
TEST(FlowXeFG, ResizeKeepsTheOverlayOnTheRuntimesQueue) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.ResizeSwapchain(800, 450)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on after resize");
    ASSERT_TRUE(game.ResizeSwapchain(640, 360)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The game leaves XeFG for its own swapchain and comes back (xefgSwapChainDestroy, then a new context): the
// presenter retires with its context, the native route takes over, and XeFG's next chain is attributed afresh.
TEST(FlowXeFG, DestroyAndRecreateKeepTheOverlayAndTheStatus) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, 2, "XeFG on");
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
        ExpectPublished(game, 0, 1, "native swapchain after XeFG");
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kXeFG)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

std::string RuntimePath(const char* name) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string directory(path);
    return directory.substr(0, directory.find_last_of('\\') + 1) + name;
}

bool EntryIsDetoured(HMODULE module, const char* exportName) {
    const auto* entry = reinterpret_cast<const unsigned char*>(GetProcAddress(module, exportName));
    return entry && (entry[0] == 0xE9 || (entry[0] == 0xFF && entry[1] == 0x25));
}

// A game or tool unloads Intel's runtimes and loads them again: the loader maps fresh, unpatched images at the old
// addresses. CE's records of the unloaded entries refused the new install, so XeFG's context, generation and
// SetPresentId pacing went unobserved and the overlay never showed XeFG.
TEST(FlowXeFG, RuntimesReloadedAtTheirOldAddressesAreObservedAgain) {
    FlowGame game(CurrentTestName());
    const HMODULE xell = LoadLibraryA(RuntimePath("libxell.dll").c_str());
    const HMODULE xefg = LoadLibraryA(RuntimePath("libxess_fg.dll").c_str());
    ASSERT_NE(xell, nullptr);
    ASSERT_NE(xefg, nullptr);
    EXPECT_TRUE(EntryIsDetoured(xefg, "xefgSwapChainSetPresentId"));
    EXPECT_TRUE(EntryIsDetoured(xell, "xellSleep"));
    ASSERT_TRUE(FreeLibrary(xefg));
    ASSERT_TRUE(FreeLibrary(xell));
    ASSERT_EQ(GetModuleHandleA("libxess_fg.dll"), nullptr);
    ASSERT_EQ(GetModuleHandleA("libxell.dll"), nullptr);

    // The game's own load maps both again.
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    const HMODULE reloadedXefg = GetModuleHandleA("libxess_fg.dll");
    const HMODULE reloadedXell = GetModuleHandleA("libxell.dll");
    ASSERT_NE(reloadedXefg, nullptr);
    ASSERT_NE(reloadedXell, nullptr);
    EXPECT_TRUE(EntryIsDetoured(reloadedXefg, "xefgSwapChainSetPresentId"));
    EXPECT_TRUE(EntryIsDetoured(reloadedXell, "xellSleep"));
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on after its runtime reloaded");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// A second copy of Intel's runtime from another folder while the game's is loaded (a tool shipping its own). CE's
// detours forward through one set of originals: detouring the copy re-pointed the game's detoured exports at the
// copy's code, which then ran on the game's contexts. The loaded image keeps CE's detours; the copy stays unobserved.
TEST(FlowXeFG, ASecondRuntimeCopyLeavesTheLoadedImagesDetoursAlone) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    const HMODULE loaded = GetModuleHandleA("libxess_fg.dll");
    ASSERT_NE(loaded, nullptr);
    const auto copyDirectory = std::filesystem::path(game.LogDirectory()) / "second_runtime";
    std::filesystem::create_directories(copyDirectory);
    const auto copyPath = copyDirectory / "libxess_fg.dll";
    std::filesystem::copy_file(RuntimePath("libxess_fg.dll"), copyPath,
                               std::filesystem::copy_options::overwrite_existing);
    const HMODULE copy = LoadLibraryW(copyPath.c_str());
    ASSERT_NE(copy, nullptr);
    ASSERT_NE(copy, loaded);
    EXPECT_FALSE(EntryIsDetoured(copy, "xefgSwapChainSetPresentId"));
    EXPECT_TRUE(EntryIsDetoured(loaded, "xefgSwapChainSetPresentId"));
    ASSERT_TRUE(FreeLibrary(copy));
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on the game's runtime");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
