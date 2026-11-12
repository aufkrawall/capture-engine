// A D3D12 game without frame generation: the inject overlay must be on every presented frame from the
// first one CE renders, drawn once, with frame generation published as off.

#include <fstream>
#include <iterator>
#include <string>

#include "tests/flow/flow_test_support.h"

namespace {

using ce::flow::CurrentTestName;

TEST(FlowBaseline, OverlayCoversEveryPresentWithoutFrameGeneration) {
    ce::flow::FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();

    EXPECT_EQ(game.PhysicalPresents(), 120u);
    ce::flow::ExpectEveryPresentCoveredOnce(game);
    // The suite's frame holds the panel (ExpectPanelsFitTheDefaultFrame): the check must have seen it here.
    EXPECT_GE(ce::flow::ExpectPanelsFitTheDefaultFrame(game), 1)
        << "no panel layout logged; logs: " << game.LogDirectory();
    ce::flow::ExpectNoDebugLayerErrors();
    // The post-process pass shades the harness's small square unless the run asks for the whole frame (--verify).
    char full[8] = {};
    const bool fullRun =
        GetEnvironmentVariableA(kCEFlowFullPostProcessVariable, full, sizeof(full)) != 0 && full[0] == '1';
    const std::string extent = std::to_string(fullRun ? 0u : ce::flow::kPostProcessShadedExtent);
    std::ifstream log(game.LogDirectory() + "/hook_debug.log");
    const std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    EXPECT_NE(text.find("Sharpen: flow harness shades " + extent + "x" + extent + " pixels"), std::string::npos)
        << "logs: " << game.LogDirectory();

    const CEFlowPublishedFG published = game.PublishedFG();
    EXPECT_EQ(published.type, 0);
    EXPECT_LT(published.multiplier, 2);
}

}  // namespace
