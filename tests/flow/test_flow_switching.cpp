// Switching between frame generation runtimes the way the dx12_fg_switch_test app and GTA/Talos do: the
// device and queue stay, the swapchain is replaced by the next runtime's. The overlay must stay on every
// presented frame across each switch, with the published state following the runtime.

#include "tests/flow/flow_test_support.h"
#include "tests/flow/device_view_answers.h"

namespace {

using namespace ce::flow;

void CheckFFXUiComposition(bool premultiplied) {
    SetEnvironmentVariableA("CE_FLOW_FFX_COMPOSE_UI", "1");
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrNestedPresenter = true;
    options.fsrUi = FSRUiResource::kFullSize;
    options.fsrUiBuffering = FSRUiBuffering::kSingleTexture;
    options.fsrTestSolidUi = true;
    options.fsrTestPremultipliedUi = premultiplied;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto enable = reinterpret_cast<void (*)(bool)>(lookup("flowSampleNestedOutput"));
    const auto read = reinterpret_cast<uint64_t (*)(UINT, UINT, uint32_t*, UINT*)>(lookup("flowReadNestedOutput"));
    ASSERT_NE(enable, nullptr);
    ASSERT_NE(read, nullptr);
    enable(true);
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    for (int frame = 0; frame < 40; ++frame) {
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
        ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    }
    uint32_t pixel = 0;
    UINT index = 0;
    ASSERT_GT(read(500, 300, &pixel, &index), 0u);
    EXPECT_GE(pixel & 255u, 125u);
    EXPECT_LE(pixel & 255u, 155u);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(4)) << game.Error();
    ASSERT_GT(read(500, 300, &pixel, &index), 0u);
    EXPECT_GE(pixel & 255u, 125u);
    EXPECT_LE(pixel & 255u, 155u);
    enable(false);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, FFXStraightAlphaUiComposesDuringAndAfterGeneration) {
    CheckFFXUiComposition(false);
}

TEST(FlowSwitch, FFXPremultipliedUiComposesDuringAndAfterGeneration) {
    CheckFFXUiComposition(true);
}

void CheckFFXBufferedUiKeepsTheScheduledFramePixels(bool reshade) {
    SetEnvironmentVariableA("CE_FLOW_FFX_COMPOSE_UI", "1");
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrNestedPresenter = true;
    options.fsrUi = FSRUiResource::kFullSize;
    options.fsrUiBuffering = FSRUiBuffering::kSwapchainCopy;
    options.fsrTestAnimatedUi = true;
    if (reshade)
        options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto enable = reinterpret_cast<void (*)(bool)>(lookup("flowSampleNestedOutput"));
    const auto setPoint = reinterpret_cast<void (*)(UINT, UINT)>(lookup("flowSetNestedHistoryPoint"));
    const auto read = reinterpret_cast<uint64_t (*)(uint64_t, uint64_t*, uint32_t*)>(lookup("flowReadNestedHistory"));
    ASSERT_NE(enable, nullptr);
    ASSERT_NE(setPoint, nullptr);
    ASSERT_NE(read, nullptr);
    setPoint(500, 300);
    enable(true);
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(16)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(2)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(8)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(2)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    uint64_t frame = 0;
    uint32_t pixel = 0;
    const uint64_t count = read(0, &frame, &pixel);
    ASSERT_EQ(count, 52u);
    for (uint64_t i = 0; i < count; ++i) {
        SCOPED_TRACE(i);
        ASSERT_EQ(read(i, &frame, &pixel), count);
        ASSERT_GT(frame, 0u);
        if (frame % 2 == 1)
            EXPECT_GE(pixel & 255u, 125u) << "frame " << frame;
        else
            EXPECT_LT(pixel & 255u, 35u) << "frame " << frame;
    }
    enable(false);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, FFXBufferedUiKeepsTheScheduledFramePixels) {
    CheckFFXBufferedUiKeepsTheScheduledFramePixels(false);
}

TEST(FlowSwitch, FFXBufferedUiKeepsScheduledPixelsThroughReShade) {
    CheckFFXBufferedUiKeepsTheScheduledFramePixels(true);
}

TEST(FlowSwitch, NestedPresenterProbeReadsTheNativeBufferBeforePresent) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrNestedPresenter = true;
    options.fsrUi = FSRUiResource::kFullSizeWithSuspendedPlaceholder;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto enable = reinterpret_cast<void (*)(bool)>(lookup("flowSampleNestedOutput"));
    const auto read = reinterpret_cast<uint64_t (*)(UINT, UINT, uint32_t*, UINT*)>(lookup("flowReadNestedOutput"));
    ASSERT_NE(enable, nullptr);
    ASSERT_NE(read, nullptr);
    enable(true);
    ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    uint32_t first = 0, second = 0;
    UINT firstIndex = 0, secondIndex = 0;
    const uint64_t firstSerial = read(500, 300, &first, &firstIndex);
    ASSERT_GT(firstSerial, 0u);
    ASSERT_TRUE(game.RenderFrames(8, FlowGame::PresentMethod::kPresent1)) << game.Error();
    const uint64_t secondSerial = read(500, 300, &second, &secondIndex);
    EXPECT_GT(secondSerial, firstSerial);
    EXPECT_NE(second, first);  // The game's green scene component changes every frame.
    EXPECT_LT(firstIndex, 7u);
    EXPECT_LT(secondIndex, 7u);
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    uint32_t activePixel = 0;
    UINT activeIndex = 0;
    const uint64_t activeSerial = read(500, 300, &activePixel, &activeIndex);
    EXPECT_GT(activeSerial, secondSerial);
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(4)) << game.Error();
    uint32_t suspendedPixel = 0;
    UINT suspendedIndex = 0;
    EXPECT_GT(read(500, 300, &suspendedPixel, &suspendedIndex), activeSerial);
    enable(false);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// 0.1.6951, dx12_fg_switch_test (session 20261003_070202, 07:05:29-07:05:42): after an FSR phase, DLSS-G on,
// off (explicit slDLSSGSetOptions, make-before-break keep-alive) and on again 1.5 s later lost the overlay for
// 1549 presents / 6.5 s: "PostSL SKIP - FG transition cooldown active (60 frames left)" the whole time. Here the
// scenario also caught the 90-present blank on the FSR -> Streamline swapchain change and a use-after-free in
// CE's post-FSR queue probe that removed the device.
TEST(FlowSwitch, PostFSRDLSSWarmResumeKeepsTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    for (int cycle = 0; cycle < 3; ++cycle) {
        ASSERT_TRUE(game.SetFSRFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
        ASSERT_TRUE(game.SetFSRFrameGeneration(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    }
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(500)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on after FSR");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(210)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS FG off after FSR");
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(900)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG warm resume");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// The menu switches of the 0.1.6951 runs, each in one frame: FSR FG on -> DLSS FG on (the FidelityFX swapchain
// replaced by Streamline's), DLSS FG on -> FSR FG on, FSR FG on -> all off (a native swapchain). Talos composes
// FSR FG through the present callback, GTA without one over a 1x1 UI placeholder.
// unloadDlssgFirst: GTA unloads DLSS-G (slFreeResources, slSetFeatureLoaded(false)) while its swapchain proxy is still
// alive, and the FidelityFX swapchain for the window is created after the proxy's release (session 20261110_182518).
// overlays: Present-chain overlays around the FidelityFX runtime's and the game's own swapchains (not Streamline's).
void SwitchRuntimesWithFrameGenerationOn(FlowGame& game, bool presentCallback, FSRUiResource ui,
                                         bool unloadDlssgFirst = false,
                                         const std::vector<PresentChainOverlay>& overlays = {}) {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = ui;
    options.presentChainOverlays = overlays;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "FSR FG -> DLSS FG");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    if (unloadDlssgFirst) {
        ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    }
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, presentCallback)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "DLSS FG -> FSR FG");

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, presentCallback)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG -> all off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, TalosStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, true, FSRUiResource::kNone);
}

TEST(FlowSwitch, GTAStyleRuntimeSwitchesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, false, FSRUiResource::kPlaceholder);
}

// GTA V Enhanced's real order at DLSS FG -> FSR FG: DLSS-G is unloaded before its swapchain proxy is released.
TEST(FlowSwitch, GTAStyleSwitchAfterTheDLSSUnloadKeepsTheOverlayAndCreatesTheFSRChain) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, false, FSRUiResource::kPlaceholder, true);
}

// The same switches in GTA V Enhanced with ReShade (session gtafsroverlaystutteryghosty): Rockstar's and Epic's
// overlays and ReShade around AMD's real swapchain and the game's own, GTA's full-size HUD as the UI resource.
TEST(FlowSwitch, GTAStyleSwitchesUnderReShadeAndLauncherOverlaysKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    SwitchRuntimesWithFrameGenerationOn(game, false, FSRUiResource::kFullSize, true,
                                        {{OverlayStandIn::kRockstar, 0}, {OverlayStandIn::kEpic, 0},
                                         {OverlayStandIn::kReShade, 0}});
}

// GTA V Enhanced + ReShade, DLSS FG -> FSR FG (session 20261112_013802): the FidelityFX swapchain is created through
// Streamline, whose DLSS-G plugin creates the native one and presents it from inside AMD's Present, copying AMD's image
// into the back buffer. The overlay stayed on AMD's UI resource at the rendered rate after the switch (a juddering
// frame time graph); the final-batch route must own the outputs in every FSR phase, before and after DLSS FG.
TEST(FlowSwitch, GTAStyleDLSSToFSRSwitchThroughStreamlinesPresenterKeepsTheOverlayAtOutputRate) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrUi = FSRUiResource::kPlaceholder;
    options.fsrNestedPresenter = true;
    options.presentChainOverlays = {{OverlayStandIn::kReShade, 0}};
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");
    ExpectFinalBatchRouteOwnsTheOutputs(game, "FSR FG on");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "FSR FG -> DLSS FG");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 2, 2, "DLSS FG -> FSR FG");
    ExpectFinalBatchRouteOwnsTheOutputs(game, "DLSS FG -> FSR FG");

    ASSERT_TRUE(game.SetFSRFrameGeneration(false, false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG -> all off");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, FirstStreamlinePassthroughOutputDrawsAfterRapidFSRSwitches) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
        ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
        ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
        ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/true)) << game.Error();
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();

        // The replacement presents one passthrough frame before SetOptions(ON).
        // The departing FidelityFX surface cannot cover this new backbuffer.
        const auto before = game.Coverage();
        const auto physicalBefore = game.PhysicalPresents();
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        const auto after = game.Coverage();
        EXPECT_EQ(game.PhysicalPresents() - physicalBefore, 1u);
        EXPECT_EQ(after.presents - before.presents, 1u);
        EXPECT_EQ(after.draws - before.draws, 1u) << "logs: " << game.LogDirectory();
        ExpectPublished(game, 0, 1, "first Streamline passthrough");

        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(24)) << game.Error();
        ExpectPublished(game, 1, 2, "DLSS FG resumed");
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// A disabled Streamline replacement must keep drawing after its one-shot prewarm is consumed.
TEST(FlowSwitch, DisabledStreamlineReplacementOnNewQueueKeepsEveryOutput) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    game.AdvanceVirtualClock(3'000'000);
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectPublished(game, 0, 1, "disabled chain A");
    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto nativeBinding = reinterpret_cast<void (*)(IDXGISwapChain**, ID3D12CommandQueue**)>(
        lookup("flowNativeBinding"));
    const auto inspectBinding = reinterpret_cast<uint64_t (*)(IDXGISwapChain*, ID3D12CommandQueue**, bool)>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_StreamlineBinding"));
    const auto startupPending = reinterpret_cast<bool (*)()>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_StreamlineStartupPending"));
    const auto candidateToken = reinterpret_cast<uint64_t (*)(IDXGISwapChain*)>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_StreamlineNormalToken"));
    const auto tryPromote = reinterpret_cast<bool (*)(IDXGISwapChain*, ID3D12CommandQueue*, uint64_t)>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_TryPromoteStreamlineBinding"));
    ASSERT_NE(nativeBinding, nullptr);
    ASSERT_NE(inspectBinding, nullptr);
    ASSERT_NE(tryPromote, nullptr);
    ASSERT_NE(candidateToken, nullptr);
    ASSERT_NE(startupPending, nullptr);
    const auto sampleOutput = reinterpret_cast<void (*)(bool)>(lookup("flowSamplePhysicalOutput"));
    const auto differentPixels = reinterpret_cast<uint64_t (*)()>(lookup("flowLastOutputDifferentPixels"));
    ASSERT_NE(sampleOutput, nullptr);
    ASSERT_NE(differentPixels, nullptr);

    uint64_t previousToken = 0;
    for (int cycle = 0; cycle < 2; ++cycle) {
        SCOPED_TRACE(cycle);
        const auto previousEpoch = game.PostSLLifecycle().epoch;
        IDXGISwapChain* oldNative = nullptr;
        ID3D12CommandQueue* oldPresentQueue = nullptr;
        nativeBinding(&oldNative, &oldPresentQueue);
        ID3D12CommandQueue* oldRegisteredQueue = nullptr;
        const uint64_t oldLifetime = inspectBinding(oldNative, &oldRegisteredQueue, false);
        ASSERT_NE(oldLifetime, 0u);
        ASSERT_EQ(oldRegisteredQueue, oldPresentQueue);
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> oldPresenterOwner = oldPresentQueue;
        const auto oldQueue = game.RetainGameQueue();
        ASSERT_TRUE(game.ReplaceStreamlineSwapchainOnNewQueue()) << game.Error();
        ASSERT_NE(game.RetainGameQueue().Get(), oldQueue.Get());
        EXPECT_FALSE(game.TryConfirmPostSLEpoch(previousEpoch));
        IDXGISwapChain* native = nullptr;
        ID3D12CommandQueue* presentQueue = nullptr;
        nativeBinding(&native, &presentQueue);
        ID3D12CommandQueue* registeredQueue = nullptr;
        const uint64_t lifetime = inspectBinding(native, &registeredQueue, false);
        ASSERT_NE(lifetime, oldLifetime);
        ASSERT_NE(presentQueue, oldPresentQueue);
        ASSERT_EQ(registeredQueue, presentQueue);
        EXPECT_EQ(inspectBinding(native, &registeredQueue, true), 0u);
        const uint64_t token = candidateToken(native);
        ASSERT_NE(token, 0u);
        EXPECT_FALSE(tryPromote(native, oldPresentQueue, token));
        if (previousToken != 0) {
            EXPECT_NE(token, previousToken);
            EXPECT_FALSE(tryPromote(native, presentQueue, previousToken));
        }
        sampleOutput(true);
        for (int output = 0; output < 4; ++output) {
            SCOPED_TRACE(output);
            if (output == 2)
                game.AdvanceVirtualClock(3'000'000);
            const auto before = game.Coverage();
            const auto physicalBefore = game.PhysicalPresents();
            ASSERT_TRUE(game.RenderFrame()) << game.Error();
            const auto after = game.Coverage();
            EXPECT_EQ(game.PhysicalPresents() - physicalBefore, 1u);
            EXPECT_EQ(after.presents - before.presents, 1u) << game.LogDirectory();
            EXPECT_EQ(after.draws - before.draws, 1u) << game.LogDirectory();
            EXPECT_EQ(after.doubleDraws - before.doubleDraws, 0u);
            EXPECT_GT(differentPixels(), 100u) << "overlay pixels absent on physical output; " << game.LogDirectory();
            EXPECT_NE(inspectBinding(native, &registeredQueue, true), 0u);
            EXPECT_FALSE(startupPending()) << "disabled normal route must finish startup";
            EXPECT_EQ(registeredQueue, presentQueue);
            ExpectPublished(game, 0, 1, "disabled replacement");
        }
        previousToken = token;
        ASSERT_TRUE(game.ResizeSwapchain(640, 360)) << game.Error();
        EXPECT_EQ(inspectBinding(native, &registeredQueue, true), 0u);
        EXPECT_EQ(candidateToken(native), 0u);
        EXPECT_FALSE(tryPromote(native, presentQueue, token));
        for (int output = 0; output < 2; ++output) {
            SCOPED_TRACE(output);
            const auto before = game.Coverage();
            const auto physicalBefore = game.PhysicalPresents();
            ASSERT_TRUE(game.RenderFrame()) << game.Error();
            const auto after = game.Coverage();
            EXPECT_EQ(game.PhysicalPresents() - physicalBefore, 1u);
            EXPECT_EQ(after.draws - before.draws, 1u) << game.LogDirectory();
            EXPECT_EQ(after.doubleDraws - before.doubleDraws, 0u);
            EXPECT_GT(differentPixels(), 100u) << "overlay pixels absent after resize; " << game.LogDirectory();
            ExpectPublished(game, 0, 1, "resized disabled replacement");
        }
        sampleOutput(false);
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
        game.AdvanceVirtualClock(3'000'000);
        ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
        ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, NativeReturnDuringHeldDLSSOffDrawsItsFirstOutput) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(12)) << game.Error();
    ASSERT_TRUE(game.WaitForFSRPresents()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
    ASSERT_TRUE(game.PostSLLifecycle().confirmedInEpoch) << game.LogDirectory();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.DLSSFrameGenerationRunning()) << "startup protection must hold the OFF request";
    ASSERT_TRUE(game.DisableStreamlineReflex()) << game.Error();
    EXPECT_TRUE(game.PostSLLifecycle().confirmedInEpoch);
    ExpectPublished(game, 1, 2, "confirmed proxy before native replacement");
    ASSERT_TRUE(game.ReturnToNativeSwapchainOnNewQueue()) << game.Error();
    const auto before = game.Coverage();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 1u);
    EXPECT_EQ(after.draws - before.draws, 1u) << game.LogDirectory();
    ExpectPublished(game, 0, 1, "first native frame after a held DLSS OFF");

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(24)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG re-enabled after the native frame");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, InactiveFidelityFXWithAProxyDeviceViewKeepsTheOverlayAfterDLSS) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    options.fsrWrappedGameQueue = true;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ExpectPublished(game, 1, 2, "DLSS FG on");

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/true)) << game.Error();
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    ASSERT_EQ(game.RetainGameSwapchain()->GetBuffer(0, IID_PPV_ARGS(&buffer)), S_OK);
    Microsoft::WRL::ComPtr<ID3D12Device> native;
    ASSERT_EQ(game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&native)), S_OK);
    {
        DeviceViewAnswers proxyDevice(buffer.Get(), native.Get(), /*allowCommandListCreation=*/false);
        Microsoft::WRL::ComPtr<ID3D12Device> reported;
        ASSERT_EQ(buffer->GetDevice(IID_PPV_ARGS(&reported)), S_OK);
        ASSERT_NE(reported.Get(), native.Get());
        buffer.Reset();

        // Menus can present through FidelityFX for many frames before enabling FG.
        ASSERT_TRUE(game.RenderFrames(24)) << game.Error();
        ExpectPublished(game, 0, 1, "FSR swapchain presenting with FG off");
    }
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(24)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Reproduction and fix for the 2,424 consecutive presents without overlay after FSR off:
// When FSR is turned off, the overlay must not disappear. Either through passthrough callback
// or through normal fallback once swapchain and queue are safe, every presented frame must be
// covered once and published status must reflect FG off.
TEST(FlowSwitch, FSROffContinuityCoversEveryPresentAndPublishesOff) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // Enable FSR with callback
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    // Turn FSR OFF: tests overlay continuity across presents
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(300)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG off");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Effect-context destroy and recreate while the presentation swapchain remains alive:
// Covers final callback from the departing lifetime, an explicit null callback toggle,
// and rejected configure packets without losing the overlay.
TEST(FlowSwitch, EffectContextRecreateWhileSwapchainAlivePreservesOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 1. Enable FSR
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on before context destroy");

    // The provider rejects a valid OFF packet with no callback. On the next
    // frame the provider still calls the previously accepted app delegate.
    const unsigned callbacksBefore = game.FSRAppPresentCallbackCalls();
    ASSERT_TRUE(game.SendRejectedFSRConfigure());
    ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    EXPECT_GT(game.FSRAppPresentCallbackCalls(), callbacksBefore);
    ExpectPublished(game, 2, 2, "FSR FG on after rejected OFF configure");

    // 3. Destroy effect context only while FFX swapchain stays alive
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    // Render frames across departing lifetime: swapchain is still presenting
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG off after destroying its effect context");

    // 4. Recreate effect context on the same swapchain and re-enable with null app callback
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(200)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on after recreating its effect context");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSwitch, DisabledStartupEffectRecreationKeepsTheFidelityFXOutputQueue) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(20)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(2)) << game.Error();

    // Recreate the effect before the first enabled configure. AMD's native swapchain
    // and its present queue survive the resize and the effect-context lifetime.
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    const auto before = game.PostProcess();
    ASSERT_TRUE(game.RenderFrames(20)) << game.Error();
    const auto after = game.PostProcess();
    EXPECT_GT(after.corrected + after.routeApplied, before.corrected + before.routeApplied);
    EXPECT_EQ(after.failed + after.routeFailed, before.failed + before.routeFailed);
    ExpectPublished(game, 0, 1, "FSR disabled on the retained output");
    ExpectNoDebugLayerErrors();
}

// Save-load teardown regression: both switch directions followed by temporary FG suspension,
// context recreation, and foreign Present wrapper.
TEST(FlowSwitch, SaveLoadHandoffAndSuspensionKeepsOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kFidelityFX;
    options.fsrPresentChainOverlayOutputs = 30; // foreign Present wrapper active
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 1. FSR on
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    // 2. Switch FSR -> DLSS
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 1, 2, "FSR -> DLSS");

    // 3. Save-load teardown: temporary FG suspension while in Streamline
    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();
    ExpectPublished(game, 0, 1, "DLSS suspended for save-load");

    // 4. Switch DLSS -> FSR
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kFidelityFX)) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "DLSS -> FSR after save-load");

    // 5. Temporary FSR suspension with null callback and effect context recreation
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR resumed after context recreation");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

// Regression test for save-load teardown where FSR FG is disabled with null callback
// followed immediately by effect-context destruction while presentation swapchain stays alive.
TEST(FlowSwitch, FSROffWithNullCallbackTearsDownBridgeCleanlyAndKeepsOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.swapchain = SwapchainKind::kFidelityFX;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 1. Enable FSR FG with present callback
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG on");

    // 2. Disable FSR FG with null callback (Avatar save-load pattern)
    ASSERT_TRUE(game.SetFSRFrameGeneration(false, /*presentCallback=*/false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(50)) << game.Error();
    ExpectPublished(game, 0, 1, "FSR FG off");

    // 3. Destroy effect context only while swapchain presents
    ASSERT_TRUE(game.DestroyFSREffectContextOnly()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(100)) << game.Error();

    // 4. Recreate effect context and re-enable with callback
    ASSERT_TRUE(game.RecreateFSREffectContext()) << game.Error();
    ASSERT_TRUE(game.SetFSRFrameGeneration(true, /*presentCallback=*/true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(150)) << game.Error();
    ExpectPublished(game, 2, 2, "FSR FG resumed");

    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
