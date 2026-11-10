// GTA V Enhanced DLSS FG -> FSR FG (session 20261110_182518): the title unloads DLSS-G with slSetFeatureLoaded(false)
// while its Streamline swapchain proxy is still alive and drops the proxy only afterwards. Streamline rebuilds its
// hook lists without a disabled plugin, so the proxy's final Release never dispatches slHookSwapChainDestroyed,
// DLSS-G keeps its reference to the native swapchain and DXGI refuses the window's replacement chain
// (E_ACCESSDENIED); the title then dereferences the missing swapchain. The fake Streamline runtime reproduces the
// dispatch rule (fakes/streamline): without CE's unload-order policy these scenarios fail with the real DXGI error
// (0x80070005 at the replacement create, verified before the policy was wired in).

#include "tests/flow/flow_test_support.h"

#include <fstream>
#include <iterator>
#include <string>

namespace {

using ce::flow::CurrentTestName;
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

std::string ReadHookLog(const FlowGame& game) {
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    return std::string{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
}

std::string LineAt(const std::string& text, size_t position) {
    return text.substr(position, text.find('\n', position) - position);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// The order GTA runs at the DLSS -> FSR switch, ending in a replacement chain on the same window (a native one: the
// window's refusal is DXGI's, whoever creates the replacement).
TEST(FlowDLSSUnloadOrder, GTAOrderLetsStreamlineDispatchTheDestroyBeforeTheReplacementChain) {
    FlowGame game(CurrentTestName());
    game.EnableTraceLogging();
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ASSERT_TRUE(game.ResizeSwapchain(352, 208)) << game.Error();

    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    // The title was answered what it asked for; Streamline itself still has the plugin until the proxy is gone.
    EXPECT_FALSE(game.DLSSGReportedLoaded());
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error() << "; logs: " << game.LogDirectory();
    ASSERT_TRUE(game.RenderFrames(20)) << game.Error();
    EXPECT_FALSE(game.DLSSGReportedLoaded());

    const std::string text = ReadHookLog(game);
    const auto deferred = text.find("[SL UNLOAD ORDER] deferred sc=");
    const auto destroyed = text.find("[SL LIFETIME] slHookSwapChainDestroyed returned", deferred);
    const auto release = text.find("[SL LIFETIME] native Release sc=", destroyed);
    const auto forwarded = text.find("[SL UNLOAD ORDER] forwarded reason=proxy-released residualRefs=0", release);
    const auto unload = text.find("[SL LIFECYCLE] slSetFeatureLoaded enter feature=DLSS-G arg=0", forwarded);
    ASSERT_NE(deferred, std::string::npos) << game.LogDirectory();
    ASSERT_NE(destroyed, std::string::npos) << "the destroy hook never ran; logs: " << game.LogDirectory();
    ASSERT_NE(release, std::string::npos) << game.LogDirectory();
    EXPECT_NE(LineAt(text, release).find("returnedRefs=0"), std::string::npos) << LineAt(text, release);
    // The decisive release names who dropped the proxy: its call site first, then the frames above it.
    const auto stack = text.find("[SL LIFETIME] native Release stack sc=", release);
    ASSERT_NE(stack, std::string::npos) << game.LogDirectory();
    EXPECT_NE(LineAt(text, stack).find("frames=sl.dlss_g.dll+0x"), std::string::npos) << LineAt(text, stack);
    EXPECT_NE(LineAt(text, stack).find(" <- "), std::string::npos) << LineAt(text, stack);
    ASSERT_NE(forwarded, std::string::npos) << game.LogDirectory();
    ASSERT_NE(unload, std::string::npos) << "the held unload never reached Streamline; logs: " << game.LogDirectory();
    EXPECT_EQ(Count(text, "[SL UNLOAD ORDER] forwarded"), 1u);
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSSUnloadOrder, ALoadBeforeTheReleaseCancelsTheHeldUnload) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();

    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    EXPECT_FALSE(game.DLSSGReportedLoaded());
    ASSERT_TRUE(game.LoadDLSSG()) << game.Error();
    EXPECT_TRUE(game.DLSSGReportedLoaded());
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    // DLSS-G never left Streamline: the proxy's release dispatches the destroy and the replacement chain is created.
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error() << "; logs: " << game.LogDirectory();

    const std::string text = ReadHookLog(game);
    EXPECT_NE(text.find("[SL UNLOAD ORDER] deferred sc="), std::string::npos);
    EXPECT_NE(text.find("[SL UNLOAD ORDER] cancelled sc="), std::string::npos);
    EXPECT_EQ(text.find("[SL UNLOAD ORDER] forwarded"), std::string::npos);
    // Neither the unload nor the load reached Streamline.
    EXPECT_EQ(text.find("[SL LIFECYCLE] slSetFeatureLoaded enter feature=DLSS-G"), std::string::npos);
    ExpectNoDebugLayerErrors();
}

TEST(FlowDLSSUnloadOrder, ATitleThatKeepsPresentingGetsTheUnloadAtItsNextRequest) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(StreamlineGameOptions())) << game.Error();
    ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();

    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();
    // The title kept the chain in use and asks again: this time Streamline gets it, as it would have the first time.
    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    EXPECT_FALSE(game.DLSSGReportedLoaded());

    const std::string text = ReadHookLog(game);
    const auto deferred = text.find("[SL UNLOAD ORDER] deferred sc=");
    const auto forwarded = text.find("[SL UNLOAD ORDER] forwarded reason=title-kept-chain", deferred);
    const auto unload = text.find("[SL LIFECYCLE] slSetFeatureLoaded enter feature=DLSS-G arg=0", forwarded);
    ASSERT_NE(deferred, std::string::npos) << game.LogDirectory();
    ASSERT_NE(forwarded, std::string::npos) << game.LogDirectory();
    EXPECT_NE(unload, std::string::npos) << game.LogDirectory();
    ExpectNoDebugLayerErrors();
}

// Without a DLSS-G chain there is nothing to wait for: the call is Streamline's at once, as before.
TEST(FlowDLSSUnloadOrder, AnUnloadWithoutALiveDLSSGChainIsForwardedImmediately) {
    FlowGame game(CurrentTestName());
    GameOptions options = StreamlineGameOptions();
    options.swapchain = SwapchainKind::kNative;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(30)) << game.Error();

    ASSERT_TRUE(game.UnloadDLSSG()) << game.Error();
    EXPECT_FALSE(game.DLSSGReportedLoaded());

    const std::string text = ReadHookLog(game);
    EXPECT_NE(text.find("[SL UNLOAD ORDER] not-deferred reason=no-live-chain"), std::string::npos);
    EXPECT_NE(text.find("[SL LIFECYCLE] slSetFeatureLoaded enter feature=DLSS-G arg=0"), std::string::npos);
    EXPECT_EQ(text.find("[SL UNLOAD ORDER] deferred"), std::string::npos);
    ExpectNoDebugLayerErrors();
}

}  // namespace
