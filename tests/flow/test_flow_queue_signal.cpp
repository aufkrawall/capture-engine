#include "tests/flow/flow_test_support.h"
#include "tests/flow/queue_dispatch_probe.h"

namespace {
using namespace ce::flow;

TEST(FlowQueueSignal, MissingReceiverCannotBorrowSavedSignalEntry) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        ASSERT_NE(first->CurrentSignal(), second->CurrentSignal());
        ASSERT_EQ(game.SignalOriginal(second.Get()), second->CurrentSignal());
        game.TrackSignalQueue(first.Get());
        // Check resolution before the call: a borrowed predecessor cannot accept a null receiver.
        ASSERT_EQ(game.SignalOriginal(nullptr), nullptr);
        EXPECT_EQ(game.ForwardSignal(nullptr, 23), E_FAIL);
        EXPECT_EQ(game.ForwardSignal(second.Get(), 24), S_FALSE);
        EXPECT_EQ(second->SignalCalls(), 1u);
        EXPECT_EQ(second->LastSignalValue(), 24u);
        EXPECT_EQ(first->SignalCalls(), 0u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowQueueSignal, InstalledImplementationsKeepReturnValuesAndRecoverAfterReset) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> first;
        Microsoft::WRL::ComPtr<SecondQueueDispatchProbe> second;
        first.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        second.Attach(new SecondQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* firstOriginal = first->CurrentSignal();
        void* secondOriginal = second->CurrentSignal();
        ASSERT_NE(firstOriginal, secondOriginal);
        game.TrackSignalQueue(first.Get());
        game.TrackSignalQueue(second.Get());
        game.TrackSignalQueue(first.Get());
        for (UINT64 value = 1; value <= 16; ++value) {
            ASSERT_EQ(game.SignalOriginal(first.Get()), firstOriginal);
            ASSERT_EQ(game.SignalOriginal(second.Get()), secondOriginal);
            EXPECT_EQ(first->InvokeSignal(value), S_OK);
            EXPECT_EQ(second->InvokeSignal(value * 2), S_FALSE);
        }
        game.ResetQueueBindings();
        ASSERT_EQ(game.SignalOriginal(first.Get()), firstOriginal);
        ASSERT_EQ(game.SignalOriginal(second.Get()), secondOriginal);
        EXPECT_EQ(first->InvokeSignal(17), S_OK);
        EXPECT_EQ(second->InvokeSignal(34), S_FALSE);
        EXPECT_EQ(first->SignalCalls(), 17u);
        EXPECT_EQ(second->SignalCalls(), 17u);
        EXPECT_EQ(first->LastSignalValue(), 17u);
        EXPECT_EQ(second->LastSignalValue(), 34u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
