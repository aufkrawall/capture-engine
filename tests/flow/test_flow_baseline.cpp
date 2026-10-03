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

    const CEFlowPublishedFG published = game.PublishedFG();
    EXPECT_EQ(published.type, 0);
    EXPECT_LT(published.multiplier, 2);
}

}  // namespace
