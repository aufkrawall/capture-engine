// A DLSS frame generation game (fake Streamline runtime, tests/flow/fakes/streamline): toggling DLSS-G
// must keep the inject overlay on every presented frame and publish the frame-generation state the
// runtime is in. Frame counts are at 144 FPS on CE's virtual clock (600 frames = 4.2 s).

#include "tests/flow/flow_test_support.h"

#include <fstream>
#include <iterator>
#include <string_view>

#include <sl.h>

namespace {

using ce::flow::CurrentTestName;
using ce::flow::ExpectEveryPresentCoveredOnce;
using ce::flow::ExpectNoDebugLayerErrors;
using ce::flow::FlowGame;
using ce::flow::GameOptions;
using ce::flow::SwapchainKind;

GameOptions StreamlineGameOptions() {
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    return options;
}

TEST(FlowDLSS, ConfigurationBurstsAndLifecycleCallsPreserveRuntimeResults) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();

    // A title can submit many configuration calls before it begins rendering.
    for (int i = 0; i < 40; ++i) {
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    }
    ASSERT_TRUE(game.RenderFrames(2)) << game.Error();

    HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(interposer, nullptr);
    const auto setLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(GetProcAddress(interposer, "slSetFeatureLoaded"));
    const auto isLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(GetProcAddress(interposer, "slIsFeatureLoaded"));
    const auto freeResources = reinterpret_cast<PFun_slFreeResources*>(GetProcAddress(interposer, "slFreeResources"));
    ASSERT_TRUE(setLoaded && isLoaded && freeResources);
    EXPECT_EQ(setLoaded(sl::kFeatureDLSS_G, false), sl::Result::eOk);
    bool loaded = true;
    EXPECT_EQ(isLoaded(sl::kFeatureDLSS_G, loaded), sl::Result::eOk);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(freeResources(sl::kFeatureDLSS_G, sl::ViewportHandle{0u}), sl::Result::eOk);
    EXPECT_EQ(freeResources(sl::kFeatureDLSS_G, sl::ViewportHandle{17u}), sl::Result::eErrorInvalidParameter);

    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_EQ(text.find("Present STALLED for"), std::string::npos);
    // The DLSS-G chain is alive, so the unload is held until its proxy is released (test_flow_dlss_unload_order.cpp):
    // the title was answered eOk and "not loaded" above and Streamline has not seen the call.
    EXPECT_NE(text.find("[SL UNLOAD ORDER] deferred sc="), std::string::npos);
    EXPECT_NE(text.find("[SL LIFECYCLE] slFreeResources returned feature=DLSS-G arg=17 result="), std::string::npos);
}

TEST(FlowDLSS, ResizeAndRetirementReportTheDestroyDispatchBeforeNativeRelease) {
    FlowGame game(CurrentTestName());
    game.EnableTraceLogging();
    auto options = StreamlineGameOptions();
    options.swapchain = SwapchainKind::kNative;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(20)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.ResizeSwapchain(352, 208)) << game.Error();

    const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    ASSERT_NE(interposer, nullptr);
    const auto setLoaded = reinterpret_cast<PFun_slSetFeatureLoaded*>(GetProcAddress(interposer, "slSetFeatureLoaded"));
    const auto freeResources = reinterpret_cast<PFun_slFreeResources*>(GetProcAddress(interposer, "slFreeResources"));
    ASSERT_TRUE(setLoaded && freeResources);
    ASSERT_EQ(freeResources(sl::kFeatureDLSS_G, sl::ViewportHandle{0u}), sl::Result::eOk);
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_EQ(setLoaded(sl::kFeatureDLSS_G, false), sl::Result::eOk);

    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    const auto free = text.find("[SL LIFECYCLE] slFreeResources returned");
    const auto destroyed = text.find("[SL LIFETIME] slHookSwapChainDestroyed returned", free);
    const auto release = text.find("[SL LIFETIME] native Release sc=", destroyed);
    const auto unloaded = text.find("[SL LIFECYCLE] slSetFeatureLoaded enter feature=DLSS-G arg=0", release);
    ASSERT_NE(free, std::string::npos);
    ASSERT_NE(destroyed, std::string::npos) << game.LogDirectory();
    ASSERT_NE(release, std::string::npos) << game.LogDirectory();
    ASSERT_NE(unloaded, std::string::npos);
    // The Release names the module that dropped the proxy's reference and what the chain still holds.
    const std::string releaseLine = text.substr(release, text.find('\n', release) - release);
    EXPECT_NE(releaseLine.find("caller=sl.dlss_g.dll+0x"), std::string::npos) << releaseLine;
    EXPECT_NE(releaseLine.find("returnedRefs="), std::string::npos) << releaseLine;
    ExpectNoDebugLayerErrors();
}

// A menu toggle seconds apart, as in the 0.1.6951 Talos/GTA runs.
void ToggleDLSSFromTheMenu(FlowGame& game, const GameOptions& options) {
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
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
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSS, MenuTogglesKeepTheOverlayAndPublishTheRuntimeState) {
    FlowGame game(CurrentTestName());
    ToggleDLSSFromTheMenu(game, StreamlineGameOptions());
}

// HDR output: Streamline's generated outputs and CE's draws on them in the swapchain's HDR format.
TEST(FlowDLSS, HDR10MenuTogglesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options = StreamlineGameOptions();
    options.backBufferFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    ToggleDLSSFromTheMenu(game, options);
}

TEST(FlowDLSS, ScRGBMenuTogglesKeepTheOverlay) {
    FlowGame game(CurrentTestName());
    GameOptions options = StreamlineGameOptions();
    options.backBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    options.colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    ToggleDLSSFromTheMenu(game, options);
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
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSS, NativeReturnRejectsDepartedEpochConfirmationAndReactivationRemainsCovered) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    const auto active = game.PostSLLifecycle();
    ASSERT_TRUE(active.confirmedInEpoch);
    EXPECT_EQ(active.callbacksInFlight, 0u);
    EXPECT_EQ(game.PublishedFG().multiplier, 2);

    ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    const auto retired = game.PostSLLifecycle();
    EXPECT_NE(retired.epoch, active.epoch);
    EXPECT_FALSE(retired.callbacksEnabled);
    EXPECT_FALSE(retired.confirmedInEpoch);
    EXPECT_EQ(retired.callbacksInFlight, 0u);
    EXPECT_FALSE(game.TryConfirmPostSLEpoch(active.epoch));
    EXPECT_FALSE(game.PostSLLifecycle().confirmedInEpoch);
    EXPECT_LT(game.PublishedFG().multiplier, 2);

    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
    const auto reactivated = game.PostSLLifecycle();
    EXPECT_TRUE(reactivated.confirmedInEpoch);
    EXPECT_TRUE(reactivated.callbacksEnabled);
    EXPECT_EQ(reactivated.callbacksInFlight, 0u);
    EXPECT_FALSE(game.TryConfirmPostSLEpoch(active.epoch));
    EXPECT_TRUE(game.PostSLLifecycle().confirmedInEpoch);
    EXPECT_EQ(game.PublishedFG().type, 1);
    EXPECT_EQ(game.PublishedFG().multiplier, 2);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSS, ActiveRecreationAfterNativeQueueReturnCoversTheFirstOutput) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.PostSLLifecycle().confirmedInEpoch) << game.LogDirectory();

    for (const int64_t gapUs : {0, 300'000}) {
        const auto previous = game.PostSLLifecycle();
        // The intermediate game-queue swapchain never presents. The runtime stays ON throughout.
        ASSERT_TRUE(game.ReturnToNativeSwapchainThroughStreamline()) << game.Error();
        // The runtime-owned create re-prepares PostSL on its own queue (callbacks may be armed for its first output),
        // but it must never inherit the departed epoch's confirmation.
        const auto handedOff = game.PostSLLifecycle();
        ASSERT_NE(handedOff.epoch, previous.epoch) << game.LogDirectory();
        ASSERT_FALSE(handedOff.confirmedInEpoch) << game.LogDirectory();
        ASSERT_EQ(handedOff.callbacksInFlight, 0u) << game.LogDirectory();
        ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
        ASSERT_TRUE(game.DLSSFrameGenerationRunning());
        ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
        game.AdvanceVirtualClock(gapUs);
        const auto presentsBefore = game.PhysicalPresents();
        ASSERT_TRUE(game.RenderFrame()) << game.Error();
        EXPECT_GT(game.PhysicalPresents(), presentsBefore);
        EXPECT_TRUE(game.PostSLLifecycle().confirmedInEpoch) << game.LogDirectory();
        EXPECT_FALSE(game.TryConfirmPostSLEpoch(previous.epoch));
        EXPECT_EQ(game.PublishedFG().multiplier, 2);
        ExpectEveryPresentCoveredOnce(game);
    }
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    const auto titleMarker = text.rfind("firstTitlePresentStart");
    ASSERT_NE(titleMarker, std::string::npos);
    const auto entry = text.find("firstOutputCallbackEnter", titleMarker);
    ASSERT_NE(entry, std::string::npos);
    const auto entryLine = text.substr(entry, text.find('\n', entry) - entry);
    constexpr std::string_view elapsedField = "sinceHandoffUs=";
    const auto elapsed = entryLine.find(elapsedField);
    ASSERT_NE(elapsed, std::string::npos);
    EXPECT_NEAR(std::stoll(entryLine.substr(elapsed + elapsedField.size())), 300000, 1);
    EXPECT_NE(text.find("firstOutputCallbackReturn", entry), std::string::npos);
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSS, ColdHandoffDrawsTheFirstNativeOutput) {
    FlowGame game(CurrentTestName());
    auto options = StreamlineGameOptions();
    options.swapchain = SwapchainKind::kNative;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(90)) << game.Error();
    const auto before = game.Coverage();
    const auto physicalBefore = game.PhysicalPresents();
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.ResizeSwapchain(352, 208)) << game.Error();
    game.AdvanceVirtualClock(310'000);
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    const HMODULE plugin = GetModuleHandleW(L"sl.dlss_g.dll");
    ASSERT_NE(plugin, nullptr);
    const auto lookup = reinterpret_cast<void* (*)(const char*)>(GetProcAddress(plugin, "slGetPluginFunction"));
    ASSERT_NE(lookup, nullptr);
    const auto presentOnTitleThread = reinterpret_cast<void (*)()>(lookup("flowPresentNextBatchOnTitleThread"));
    ASSERT_NE(presentOnTitleThread, nullptr);
    presentOnTitleThread();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();

    const auto physicalOutputs = game.PhysicalPresents() - physicalBefore;
    const auto after = game.Coverage();
    ASSERT_GT(physicalOutputs, 0u);
    EXPECT_EQ(after.presents - before.presents, physicalOutputs);
    EXPECT_EQ(after.draws - before.draws, physicalOutputs) << game.LogDirectory();
    EXPECT_TRUE(game.PostSLLifecycle().confirmedInEpoch) << game.LogDirectory();
    ExpectEveryPresentCoveredOnce(game);
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    ASSERT_TRUE(log.is_open());
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("exact PostSL startup-transport draw SUBMITTED"), std::string::npos);
    EXPECT_NE(text.find("Reusing warm DX12 overlay backend"), std::string::npos);
    ExpectNoDebugLayerErrors();
}

}  // namespace
