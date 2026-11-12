// A D3D12 game without frame generation: the inject overlay must be on every presented frame from the
// first one CE renders, drawn once, with frame generation published as off.

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

    const CEFlowPublishedFG published = game.PublishedFG();
    EXPECT_EQ(published.type, 0);
    EXPECT_LT(published.multiplier, 2);
}

}  // namespace
