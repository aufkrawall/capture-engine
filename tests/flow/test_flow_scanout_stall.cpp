// The scanout-stall report (hook/present/scanout_stall_report.h): a game that keeps presenting while the display stream
// the sensor service publishes stands still gets one snapshot of what decides it, and the first display change after a
// Streamline handoff is timed. The scenarios publish the display stream themselves (FlowGame::PublishDisplayTransition).
// GTA V Enhanced, session 20261110_182518: 19.5 s of presents on the new DLSS-G chain with no flip reaching the screen.

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

std::string ReadHookLog(const FlowGame& game) {
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    return std::string{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
}

std::string LineAt(const std::string& text, size_t position) {
    return text.substr(position, text.find('\n', position) - position);
}

TEST(FlowScanoutStall, AStandingStillDisplayIsReportedOnceWithTheWindowFactsAndEndsWhenItResumes) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(10)) << game.Error();
    // The stream is active and moving: no episode.
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(game.PublishDisplayTransition());
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    }
    EXPECT_EQ(ReadHookLog(game).find("[SCANOUT STALL]"), std::string::npos);

    // 90 presents (~625 ms of the game's clock) and no transition reaches the screen.
    ASSERT_TRUE(game.RenderFrames(90)) << game.Error();
    ASSERT_TRUE(game.PublishDisplayTransition());
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();

    const std::string text = ReadHookLog(game);
    const auto start = text.find("[SCANOUT STALL] ep=1 start sc=");
    ASSERT_NE(start, std::string::npos) << game.LogDirectory();
    const std::string startLine = LineAt(text, start);
    // The harness window sits off the desktop, so DXGI answers every Present with DXGI_STATUS_OCCLUDED and the report
    // says so: the histogram is what tells "dropped" from "shown".
    EXPECT_NE(startLine.find("results=[0x087A0001="), std::string::npos) << startLine;
    EXPECT_NE(startLine.find("frameStats("), std::string::npos) << startLine;
    const auto swapchain = text.find("[SCANOUT STALL] ep=1 swapchain fmt=", start);
    const auto window = text.find("[SCANOUT STALL] ep=1 window visible=", start);
    const auto above = text.find("[SCANOUT STALL] ep=1 above total=", start);
    ASSERT_NE(swapchain, std::string::npos) << game.LogDirectory();
    ASSERT_NE(window, std::string::npos) << game.LogDirectory();
    ASSERT_NE(above, std::string::npos) << game.LogDirectory();
    // The test window sits off the desktop: the report says it does not cover its monitor.
    EXPECT_NE(LineAt(text, window).find("flipBlockers="), std::string::npos) << LineAt(text, window);
    EXPECT_NE(LineAt(text, window).find("not-covering-monitor"), std::string::npos) << LineAt(text, window);

    const auto end = text.find("[SCANOUT STALL] ep=1 end durationUs=", start);
    ASSERT_NE(end, std::string::npos) << game.LogDirectory();
    const std::string endLine = LineAt(text, end);
    EXPECT_NE(endLine.find("firstChange=own-flip"), std::string::npos) << endLine;
    EXPECT_NE(endLine.find("sequenceAdvance=1"), std::string::npos) << endLine;
    EXPECT_NE(endLine.find("windowChanged=0"), std::string::npos) << endLine;
    // One snapshot per episode, not one per present.
    EXPECT_EQ(text.find("[SCANOUT STALL] ep=1 start sc=", start + 1), std::string::npos);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScanoutStall, NothingIsReportedWithoutADisplayStream) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    // No sensor: the stream never publishes, and that says nothing about the screen.
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();
    EXPECT_EQ(ReadHookLog(game).find("[SCANOUT STALL]"), std::string::npos);
    ExpectNoDebugLayerErrors();
}

TEST(FlowScanoutStall, TheFirstDisplayChangeAfterAStreamlineHandoffIsTimed) {
    FlowGame game(CurrentTestName());
    // GTA's order: a native chain first (the loading screen), then DLSS-G's chain on the same window.
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kNative;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    for (int i = 0; i < 5; ++i) {  // the loading chain's flips: the stream is live when the handoff arms
        ASSERT_TRUE(game.PublishDisplayTransition());
        ASSERT_TRUE(game.RenderFrames(1)) << game.Error();
    }
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kStreamline)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(40)) << game.Error();
    ASSERT_TRUE(game.PublishDisplayTransition());  // the new chain reaches the screen
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();

    const std::string text = ReadHookLog(game);
    const auto handoff = text.find("[SL STARTUP] handoff=1 sc=");
    const auto change = text.find("firstScreenChange sinceHandoffUs=", handoff);
    ASSERT_NE(handoff, std::string::npos) << game.LogDirectory();
    ASSERT_NE(change, std::string::npos) << game.LogDirectory();
    const std::string line = LineAt(text, text.rfind('\n', change) + 1);
    EXPECT_NE(line.find("[SL STARTUP] handoff=1 sc="), std::string::npos) << line;
    EXPECT_EQ(line.find("presentsBefore=0"), std::string::npos) << line;
    ExpectNoDebugLayerErrors();
}

}  // namespace
