#include "tests/flow/flow_test_support.h"
#include "tests/flow/signal_interposer.h"

namespace {
using namespace ce::flow;

TEST(FlowSignalInterposer, ForeignFollowerSurvivesCERemovalAndBindingReset) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> probe;
        probe.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* original = probe->CurrentSignal();
        game.TrackSignalQueue(probe.Get());
        SignalInterposer foreign(game, *probe.Get());
        ASSERT_TRUE(foreign.Installed());
        EXPECT_EQ(probe->InvokeSignal(1), S_OK);
        EXPECT_TRUE(game.RemoveSignalQueue(probe.Get()));
        EXPECT_EQ(probe->CurrentSignal(), foreign.Entry());
        game.ResetQueueBindings();
        ASSERT_EQ(game.SignalOriginal(probe.Get()), original)
            << "reject a foreign -> CE -> foreign cycle before calling";
        EXPECT_EQ(probe->InvokeSignal(2), S_OK);
        EXPECT_EQ(foreign.Calls(), 2u);
        EXPECT_TRUE(foreign.Detach());
        EXPECT_EQ(game.SignalOriginal(probe.Get()), original);
        EXPECT_EQ(probe->InvokeSignal(3), S_OK);
        EXPECT_TRUE(game.RemoveSignalQueue(probe.Get()));
        EXPECT_EQ(probe->CurrentSignal(), original);
        EXPECT_EQ(probe->InvokeSignal(4), S_OK);
        EXPECT_EQ(probe->SignalCalls(), 4u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSignalInterposer, CEAboveForeignProviderRestoresTheEstablishedChain) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> probe;
        probe.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* original = probe->CurrentSignal();
        SignalInterposer foreign(game, *probe.Get());
        ASSERT_TRUE(foreign.Installed());
        game.TrackSignalQueue(probe.Get());
        EXPECT_EQ(probe->InvokeSignal(1), S_OK);
        EXPECT_EQ(foreign.Calls(), 1u);
        EXPECT_FALSE(foreign.Detach()) << "a provider cannot overwrite the CE layer above it";
        EXPECT_TRUE(game.RemoveSignalQueue(probe.Get()));
        EXPECT_EQ(probe->CurrentSignal(), foreign.Entry());
        EXPECT_TRUE(foreign.Detach());
        game.ResetQueueBindings();
        EXPECT_EQ(probe->CurrentSignal(), original);
        EXPECT_EQ(probe->InvokeSignal(2), S_OK);
        EXPECT_EQ(probe->SignalCalls(), 2u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowSignalInterposer, AdmittedCallbackFinishesAcrossRemovalAndReset) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    {
        Microsoft::WRL::ComPtr<FirstQueueDispatchProbe> probe;
        probe.Attach(new FirstQueueDispatchProbe(game.RetainGameQueue().Get()));
        void* original = probe->CurrentSignal();
        CallBarrier barrier;
        SignalInterposer foreign(game, *probe.Get(), &barrier);
        ASSERT_TRUE(foreign.Installed());
        game.TrackSignalQueue(probe.Get());
        BlockedSignalCall call(*probe.Get(), barrier);
        ASSERT_TRUE(barrier.WaitUntilEntered());
        EXPECT_EQ(foreign.ActiveCalls(), 1u);
        EXPECT_EQ(probe->SignalCalls(), 0u);
        EXPECT_TRUE(game.RemoveSignalQueue(probe.Get()));
        EXPECT_TRUE(foreign.Detach());
        game.ResetQueueBindings();
        EXPECT_EQ(probe->CurrentSignal(), original);
        EXPECT_EQ(call.Finish(), S_OK);
        EXPECT_EQ(foreign.ActiveCalls(), 0u);
        EXPECT_EQ(probe->SignalCalls(), 1u);
        EXPECT_EQ(probe->LastSignalValue(), 91u);
        EXPECT_EQ(probe->InvokeSignal(92), S_OK);
        EXPECT_EQ(probe->SignalCalls(), 2u);
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
