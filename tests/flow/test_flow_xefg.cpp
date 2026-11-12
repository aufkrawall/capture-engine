// An Intel XeSS frame generation game (fake libxess_fg.dll, tests/flow/fakes/xefg): XeFG creates and presents a
// native swapchain of its own, generated frames from its presenter thread on its own queue and the real frame on the
// game thread. The inject overlay must be on every one of those outputs, drawn once each, on XeFG's queue, and the
// overlay must show "XeFG Nx" while generation is on - through toggles, multi-frame generation, resize, the
// InitFromSwapChain handover and swapchain recreation.

#include "tests/flow/flow_test_support.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

// HDR10 output: XeFG's generated outputs and CE's draws on them in the swapchain's HDR format.
TEST(FlowXeFG, HDR10TogglesKeepTheOverlayOnEveryOutput) {
    FlowGame game(CurrentTestName());
    GameOptions options = XeFGGameOptions();
    options.backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowXeFG, ScRGBTogglesKeepTheOverlayOnEveryOutput) {
    FlowGame game(CurrentTestName());
    GameOptions options = XeFGGameOptions();
    options.backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent1);
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

TEST(FlowXeFG, PublishesGenerationWhenAnotherComponentOwnsSetEnabled) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 0, 1, "XeFG before generation");
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1, /*statusOnly=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(180)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on through last-present status");
    ASSERT_TRUE(game.SetXeFGFrameGeneration(false, 1, /*statusOnly=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    ExpectPublished(game, 0, 1, "XeFG off through last-present status");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// XeFG's presenter is pacing-critical: with generation on, CE must not block it on the overlay's GPU work, and the
// deferred Signal must still prove every overlay slot (ResetAllocatorForDraw would skip the overlay otherwise, which
// the coverage check would catch). 20261104_144527: the wait cost 2.4-13 ms per output.
TEST(FlowXeFG, GeneratedOutputsNeverWaitOnTheOverlayFence) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent1);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("Overlay fence completion: deferred Signal after Present (fgActive=1 runtimeOutputChain=1"),
              std::string::npos)
        << "generation never reached the overlay fence decision; logs: " << game.LogDirectory();
    EXPECT_EQ(text.find("Overlay fence completion: CPU wait on the completion fence (fgActive=1 runtimeOutputChain=1"),
              std::string::npos)
        << "logs: " << game.LogDirectory();
}

// XeFG's outputs present with Present1, whose detour had no stage recorder: a route that held its presenter for
// milliseconds showed nothing (20261104_144527).
TEST(FlowXeFG, Present1OutputsReportTheirStageCost) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ToggleXeFG(game, 1, FlowGame::PresentMethod::kPresent1);
    EXPECT_EQ(game.DrainPresentStageCalls(), game.PhysicalPresents()) << "logs: " << game.LogDirectory();
    ExpectEveryPresentCoveredOnce(game);
}

// XeFG's rows came from its frame work alone, with present_call_us, pre_present_us and post_present_us 0 and no limiter
// wait (20261104_195245, 20261104_221303). The route now holds that row for the down-call: one row per output, and the
// log names the route's row as held. The hook's clock is virtual here, so the durations themselves prove nothing.
TEST(FlowXeFG, EveryOutputWritesOneRowWithItsDownCallSplit) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    const std::string csvPath = game.LogDirectory() + "/perf_metrics_flow.csv";
    game.StartPerfCsv(csvPath);
    const uint64_t presentsBefore = game.PhysicalPresents();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    const uint64_t presents = game.PhysicalPresents() - presentsBefore;
    game.StopPerfCsv();

    std::ifstream csv(csvPath);
    ASSERT_TRUE(csv.is_open()) << csvPath;
    uint64_t rows = 0;
    for (std::string line; std::getline(csv, line);) {
        ++rows;
    }
    ASSERT_GT(rows, 1u) << csvPath;
    --rows;  // the header
    // The presenter thread may be mid-output at either end of the window.
    const uint64_t slack = 4;
    EXPECT_GE(rows + slack, presents) << "an output without a row; logs: " << game.LogDirectory();
    EXPECT_LE(rows, presents + slack) << "an output with two rows; logs: " << game.LogDirectory();

    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("[PERF CSV] present row of DXGIShared::RuntimeOwnedOutputPresent (api=DX12): the frame's own row, "
                        "with the down-call split"),
              std::string::npos)
        << "logs: " << game.LogDirectory();
}

// Talos sends its first present ID when it enables generation, after hundreds of outputs. The first cadence window
// counted those as its own and read 4x for a window (20261104_183540: 269 outputs, 45 application frames).
TEST(FlowXeFG, TheFirstCadenceWindowCountsOnlyItsOwnOutputs) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetXeFGPresentIds(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetXeFGPresentIds(true)) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();  // 144 frames fill the window, the rest publish its verdict
    ExpectPublished(game, kXeFGStatusType, 2, "after the first cadence window");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Talos's menu (20261104_183540): the game stops sending present IDs, XeFG queues one frame per Present and reports an
// error, and generation stays enabled. The overlay kept reading "XeFG 2x" with stale rates.
TEST(FlowXeFG, AMenuWithoutPresentIdsReadsAsNotGeneratingAndRecovers) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "in the game");
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetXeFGPresentIds(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(450)) << game.Error();  // over two windows without an application frame
        ExpectPublished(game, 0, 1, "in the menu");
        ASSERT_TRUE(game.SetXeFGPresentIds(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(10)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, 2, "right after the menu");
        ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, 2, "measured again after the menu");
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowXeFG, GenerationEnabledInAMenuWithoutPresentIdsReadsAsNotGenerating) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(XeFGGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetXeFGPresentIds(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    for (int cycle = 0; cycle < 2; ++cycle) {
        ASSERT_TRUE(game.SetXeFGFrameGeneration(true, 1)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(450)) << game.Error();
        ExpectPublished(game, 0, 1, "generation enabled without any present IDs");
        ASSERT_TRUE(game.SetXeFGPresentIds(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(10)) << game.Error();
        ExpectPublished(game, kXeFGStatusType, 2, "first present ID after the menu");
        ASSERT_TRUE(game.SetXeFGFrameGeneration(false, 1)) << game.Error();
        ASSERT_TRUE(game.SetXeFGPresentIds(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    }
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
    ASSERT_TRUE(game.ResizeSwapchain(kFrameWidth + 64, kFrameHeight + 40)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, kXeFGStatusType, 2, "XeFG on after resize");
    ASSERT_TRUE(game.ResizeSwapchain(kFrameWidth, kFrameHeight)) << game.Error();
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
