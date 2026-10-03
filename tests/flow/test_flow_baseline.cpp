// A D3D12 game without frame generation: the inject overlay must be on every presented frame from the
// first one CE renders, drawn once, with frame generation published as off.

#include <gtest/gtest.h>

#include "tests/flow/flow_host.h"

namespace {

std::string CurrentTestName() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::string(info->test_suite_name()) + "." + info->name();
}

TEST(FlowBaseline, OverlayCoversEveryPresentWithoutFrameGeneration) {
    ce::flow::FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(120)) << game.Error();

    const CEFlowOverlayCoverage coverage = game.Coverage();
    EXPECT_EQ(coverage.presents, 120u) << "CE accounts every Present exactly once; logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.uncovered, 0u) << "logs: " << game.LogDirectory();
    EXPECT_EQ(coverage.doubleDraws, 0u) << "logs: " << game.LogDirectory();

    const CEFlowPublishedFG published = game.PublishedFG();
    EXPECT_EQ(published.type, 0);
    EXPECT_LT(published.multiplier, 2);
}

}  // namespace
