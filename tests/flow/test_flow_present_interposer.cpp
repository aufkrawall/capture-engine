#include "tests/flow/flow_test_support.h"
#include "tests/flow/present_interposer.h"

namespace {
using namespace ce::flow;

TEST(FlowPresentInterposer, ForeignLayerAboveCEKeepsCoverageAcrossRepairAndRemoval) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        PresentInterposer foreign(game.RetainUnderlyingGameSwapchain().Get(), true);
        ASSERT_TRUE(foreign.Installed());
        ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
        EXPECT_EQ(foreign.Calls(), 3u);
        game.RepairPresentHooks();
        EXPECT_EQ(foreign.CurrentEntry(), foreign.Entry());
        ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
        EXPECT_EQ(foreign.Calls(), 6u);
        EXPECT_TRUE(foreign.Detach());
        ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
        EXPECT_EQ(foreign.Calls(), 6u);
        EXPECT_EQ(foreign.ActiveCalls(), 0u);
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowPresentInterposer, AdmittedForeignProbeCompletesAfterPhysicalRemoval) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        CallBarrier barrier;
        PresentInterposer foreign(game.RetainUnderlyingGameSwapchain().Get(), false, &barrier);
        ASSERT_TRUE(foreign.Installed());
        BlockedPresentCall call(foreign, barrier);
        ASSERT_TRUE(barrier.WaitUntilEntered());
        EXPECT_EQ(foreign.ActiveCalls(), 1u);
        EXPECT_TRUE(foreign.Detach());
        EXPECT_TRUE(SUCCEEDED(call.Finish()));
        EXPECT_EQ(foreign.ActiveCalls(), 0u);
        EXPECT_EQ(foreign.Calls(), 1u);
        ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
        EXPECT_EQ(foreign.Calls(), 1u);
    }
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowPresentInterposer, PresentAndPresent1StatusQueriesLeaveOutputAccountingUnchanged) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    auto swapchain = game.RetainGameSwapchain();
    const auto before = game.Coverage();
    const auto published = game.PublishedFG();
    const UINT buffer = swapchain->GetCurrentBackBufferIndex();
    DXGI_PRESENT_PARAMETERS parameters{};
    for (int probe = 0; probe < 4; ++probe) {
        EXPECT_TRUE(SUCCEEDED(swapchain->Present(0, DXGI_PRESENT_TEST)));
        EXPECT_TRUE(SUCCEEDED(swapchain->Present1(0, DXGI_PRESENT_TEST, &parameters)));
        auto real = game.RetainUnderlyingGameSwapchain();
        EXPECT_TRUE(SUCCEEDED(real->Present(0, DXGI_PRESENT_TEST)));
        Microsoft::WRL::ComPtr<IDXGISwapChain1> real1;
        ASSERT_EQ(real.As(&real1), S_OK);
        EXPECT_TRUE(SUCCEEDED(real1->Present1(0, DXGI_PRESENT_TEST, &parameters)));
    }
    EXPECT_EQ(swapchain->GetCurrentBackBufferIndex(), buffer);
    EXPECT_EQ(game.Coverage().presents, before.presents);
    EXPECT_EQ(game.Coverage().doubleDraws, before.doubleDraws);
    EXPECT_EQ(game.PublishedFG().type, published.type);
    EXPECT_EQ(game.PublishedFG().multiplier, published.multiplier);
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
