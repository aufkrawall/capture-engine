#include "tests/flow/flow_test_support.h"
#include "tests/flow/queue_dispatch_probe.h"

namespace {
using namespace ce::flow;

TEST(FlowQueueDispatch, UntrackedImplementationUsesItsOwnEntry) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        ASSERT_NE(first->CurrentECL(), second->CurrentECL());
        // Reject the wrong target before invoking it: a native method cannot accept this proxy layout.
        ASSERT_EQ(game.QueueOriginal(first.Get()), first->CurrentECL());
        ASSERT_EQ(game.QueueOriginal(second.Get()), second->CurrentECL());
        game.ForwardQueue(first.Get());
        game.ForwardQueue(second.Get());
        EXPECT_EQ(first->Calls(), 1u);
        EXPECT_EQ(second->Calls(), 1u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowQueueDispatch, InstalledImplementationsRetainExactOriginals) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* firstOriginal = first->CurrentECL();
        void* secondOriginal = second->CurrentECL();
        ASSERT_NE(firstOriginal, secondOriginal);
        game.TrackQueue(first.Get());
        game.TrackQueue(second.Get());
        // Duplicate discovery preserves the predecessor rather than inserting the detour into its own chain.
        game.TrackQueue(first.Get());
        for (int i = 0; i < 16; ++i) {
            ASSERT_EQ(game.QueueOriginal(first.Get()), firstOriginal);
            ASSERT_EQ(game.QueueOriginal(second.Get()), secondOriginal);
            game.ForwardQueue(first.Get());
            game.ForwardQueue(second.Get());
        }
        game.ResetQueueBindings();
        ASSERT_EQ(game.QueueOriginal(first.Get()), firstOriginal);
        ASSERT_EQ(game.QueueOriginal(second.Get()), secondOriginal);
        game.ForwardQueue(first.Get());
        game.ForwardQueue(second.Get());
        EXPECT_EQ(first->Calls(), 17u);
        EXPECT_EQ(second->Calls(), 17u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

}  // namespace
