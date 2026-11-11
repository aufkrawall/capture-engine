// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// Third-party overlays hooked into the Present chain of the game's own swapchain (fakes/overlay/present_chain_layer.cpp,
// built under each overlay's module name): each draws its own UI at Present before it calls the swapchain below it, and
// ReShade's queue object carries the game's submissions. After the initial startup admission, CE must draw once on
// every Present, whichever overlays draw, stop drawing or are identified by name, in SDR and HDR alike.

#include "tests/flow/flow_test_support.h"

namespace {

using namespace ce::flow;

void StartOverlay(FlowGame& game, FlowGame::PresentMethod method) {
    // Epic/Social Club deliberately defer cold backend initialization. Render until the first visible draw, using
    // the scenario's virtual frame clock, and measure that initial exclusion instead of hiding any later gap.
    for (int frame = 0; frame < 300 && game.Coverage().presents == 0; ++frame)
        ASSERT_TRUE(game.RenderFrame(method)) << game.Error();
    ASSERT_EQ(game.Coverage().presents, 1u) << "overlay never started; logs: " << game.LogDirectory();
    std::printf("[  FLOW  ] cold-start presents before first overlay=%llu\n",
                static_cast<unsigned long long>(game.PhysicalPresents() - 1));
}

void RenderUnderOverlays(FlowGame& game, const GameOptions& options, FlowGame::PresentMethod method) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_NO_FATAL_FAILURE(StartOverlay(game, method));
    const uint64_t coldStartupPresents = game.PhysicalPresents() - 1;
    ASSERT_TRUE(game.RenderFrames(300, method)) << game.Error();
    EXPECT_EQ(game.PhysicalPresents(), coldStartupPresents + 301);
    ExpectEveryPresentCoveredOnce(game, coldStartupPresents);
    ExpectNoDebugLayerErrors();
}

TEST(FlowPresentChainOverlays, ASteamToastAboveReShadeLeavesEveryPresentCoveredOnce) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.presentChainOverlays = {{OverlayStandIn::kSteam, 60}, {OverlayStandIn::kReShade, 0}};
    RenderUnderOverlays(game, options, FlowGame::PresentMethod::kPresent);
}

// Every overlay stand-in at once, Present1 through all of them.
TEST(FlowPresentChainOverlays, FiveLauncherAndEffectOverlaysPassPresent1Through) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.presentChainOverlays = {{OverlayStandIn::kRockstar, 0}, {OverlayStandIn::kEpic, 0},
                                    {OverlayStandIn::kUbisoft, 0}, {OverlayStandIn::kSteam, 90},
                                    {OverlayStandIn::kReShade, 0}};
    RenderUnderOverlays(game, options, FlowGame::PresentMethod::kPresent1);
}

TEST(FlowPresentChainOverlays, ColdStartupResourcesNeedNoExtraWallClockSettle) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.presentChainOverlays = {{OverlayStandIn::kEpic, 0}, {OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    // Advance the existing window/resume admission gates, then let backend/RTV/sync initialization finish
    // without advancing time again. The virtual clock is ahead of the host's frame schedule, so the next few
    // frames cannot satisfy a newly armed 100 ms resource-prime pause. GPU fences still complete normally.
    for (int step = 0; step < 3; ++step) {
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        game.AdvanceVirtualClock(1000000);
    }
    for (int step = 0; step < 4 && game.Coverage().presents == 0; ++step)
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ASSERT_GT(game.Coverage().presents, 0u)
        << "startup waited for time after resource readiness: " << game.LogDirectory();
    const uint64_t coldStartupPresents = game.PhysicalPresents() - game.Coverage().presents;
    ASSERT_TRUE(game.RenderFrames(12)) << game.Error();
    ExpectEveryPresentCoveredOnce(game, coldStartupPresents);
    ExpectNoDebugLayerErrors();
}

TEST(FlowPresentChainOverlays, HDR10GameUnderReShadeAndEpicLeavesEveryPresentCoveredOnce) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.presentChainOverlays = {{OverlayStandIn::kEpic, 0}, {OverlayStandIn::kReShade, 0}};
    options.backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    RenderUnderOverlays(game, options, FlowGame::PresentMethod::kPresent);
}

TEST(FlowPresentChainOverlays, ScRGBGameUnderReShadeLeavesEveryPresentCoveredOnce) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    options.backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    RenderUnderOverlays(game, options, FlowGame::PresentMethod::kPresent);
}

void ReplaceNativePresentationQueue(bool reShade) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    if (reShade)
        options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_NO_FATAL_FAILURE(StartOverlay(game, FlowGame::PresentMethod::kPresent));
    const uint64_t coldStartupPresents = game.PhysicalPresents() - 1;
    ASSERT_TRUE(game.RenderFrames(149)) << game.Error();
    ExpectPublished(game, 0, 1, "before native queue replacement");
    ASSERT_TRUE(game.ReturnToNativeSwapchainOnNewQueue()) << game.Error();
    const auto before = game.Coverage();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    EXPECT_EQ(game.Coverage().draws - before.draws, 1u) << game.LogDirectory();
    ExpectPublished(game, 0, 1, "first output on a replacement native queue");
    ASSERT_TRUE(game.RenderFrames(149)) << game.Error();
    EXPECT_EQ(game.PhysicalPresents(), coldStartupPresents + 300);
    ExpectEveryPresentCoveredOnce(game, coldStartupPresents);
    ExpectNoDebugLayerErrors();
}

TEST(FlowPresentChainOverlays, ReShadeWrapsAReplacementNativeQueue) {
    ReplaceNativePresentationQueue(true);
}

TEST(FlowPresentChainOverlays, UnwrappedReplacementNativeQueueKeepsTheOverlay) {
    ReplaceNativePresentationQueue(false);
}

}  // namespace
