#include "tests/flow/flow_test_support.h"
#include "tests/flow/queue_device_view_probe.h"

namespace {
using namespace ce::flow;

struct QueueBindingAPI {
    HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
    CEFlow_GetQueueBinding_t snapshot = reinterpret_cast<CEFlow_GetQueueBinding_t>(
        GetProcAddress(hook, "CEFlow_GetQueueBinding"));
    CEFlow_AdoptQueue_t adopt = reinterpret_cast<CEFlow_AdoptQueue_t>(GetProcAddress(hook, "CEFlow_AdoptQueue"));
    CEFlow_BindSwapchainQueue_t bind = reinterpret_cast<CEFlow_BindSwapchainQueue_t>(
        GetProcAddress(hook, "CEFlow_BindSwapchainQueue"));
    explicit operator bool() const { return snapshot && adopt && bind; }
    CEFlowQueueBinding Snapshot() const {
        CEFlowQueueBinding state;
        snapshot(&state);
        return state;
    }
};

TEST(FlowQueueBinding, AuxiliaryDeviceViewsPreserveDLSSOffCoverageBeyondTeardownGrace) {
    FlowGame game(CurrentTestName());
    GameOptions options;
    options.streamline = true;
    options.swapchain = SwapchainKind::kStreamline;
    ASSERT_TRUE(game.CreateDeviceAndSwapchain(options)) << game.Error();
    QueueBindingAPI api;
    ASSERT_TRUE(api);
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ComPtr<ID3D12Device> device;
    ASSERT_EQ(game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&device)), S_OK);
    {
        ComPtr<QueueDeviceViewProbe> view;
        view.Attach(new QueueDeviceViewProbe(device.Get()));
        ComPtr<DeviceViewQueueProbe> auxiliary;
        auxiliary.Attach(new DeviceViewQueueProbe(game.RetainGameQueue().Get(), view.Get()));
        ASSERT_NE(static_cast<ID3D12Device*>(view.Get()), device.Get());
        for (int cycle = 0; cycle < 2; ++cycle) {
            ASSERT_TRUE(game.SetDLSSFrameGeneration(true)) << game.Error();
            ASSERT_TRUE(game.RenderFrames(600)) << game.Error();
            const auto active = api.Snapshot();
            ASSERT_NE(active.successfulPostSLSwapchain, nullptr);
            ASSERT_TRUE(game.SetDLSSFrameGeneration(false)) << game.Error();
            // Interleave wrapper/native discovery on the first OFF frame and after the 600-frame grace.
            for (int frame = 0; frame < 720; ++frame) {
                const auto beforeDiscovery = api.Snapshot();
                api.adopt(auxiliary.Get(), true);
                api.adopt(game.RetainGameQueue().Get(), true);
                const auto retained = api.Snapshot();
                ASSERT_EQ(retained.queue, active.queue) << "cycle=" << cycle << " frame=" << frame;
                ASSERT_EQ(retained.device, active.device);
                ASSERT_EQ(retained.successfulPostSLSwapchain, beforeDiscovery.successfulPostSLSwapchain);
                ASSERT_EQ(retained.capturedSwapchain, beforeDiscovery.capturedSwapchain);
                ASSERT_TRUE(retained.successfulPostSLSwapchain || retained.capturedSwapchain);
                ASSERT_TRUE(game.RenderFrame(frame % 2 ? FlowGame::PresentMethod::kPresent1
                                                       : FlowGame::PresentMethod::kPresent)) << game.Error();
            }
            EXPECT_EQ(auxiliary->DeviceQueries(), 0u);
            EXPECT_FALSE(game.DLSSFrameGenerationRunning());
            ExpectPublished(game, 0, 1, "explicit OFF after auxiliary device-view submissions");
        }
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    EXPECT_EQ(g_deviceViewProbeObjects.load(), 0u);
    ASSERT_TRUE(game.UseSwapchain(SwapchainKind::kNative)) << game.Error();
    ASSERT_TRUE(game.RenderFrames(60)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowQueueBinding, CompletedSwapchainBindingReplacesDeviceBeforePublishingExactProof) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
    QueueBindingAPI api;
    ASSERT_TRUE(api);
    ComPtr<ID3D12Device> device;
    const auto queue = game.RetainGameQueue();
    const auto swapchain = game.RetainUnderlyingGameSwapchain();
    ASSERT_EQ(queue->GetDevice(IID_PPV_ARGS(&device)), S_OK);
    {
        ComPtr<QueueDeviceViewProbe> view;
        view.Attach(new QueueDeviceViewProbe(device.Get()));
        ComPtr<DeviceViewQueueProbe> replacement;
        replacement.Attach(new DeviceViewQueueProbe(queue.Get(), view.Get()));
        api.bind(replacement.Get(), swapchain.Get());
        const auto replaced = api.Snapshot();
        EXPECT_EQ(replaced.queue, replacement.Get());
        EXPECT_EQ(replaced.device, view.Get());
        EXPECT_EQ(replaced.capturedSwapchain, swapchain.Get());
        EXPECT_EQ(replaced.successfulPostSLSwapchain, nullptr);
        api.adopt(queue.Get(), true);
        EXPECT_EQ(api.Snapshot().queue, replacement.Get());
        EXPECT_EQ(api.Snapshot().device, view.Get());
        // An unavailable incoming device must also leave the entire established pair intact.
        ComPtr<DeviceViewQueueProbe> unavailable;
        unavailable.Attach(new DeviceViewQueueProbe(queue.Get(), nullptr));
        api.adopt(unavailable.Get(), false);
        EXPECT_EQ(api.Snapshot().queue, replacement.Get());
        EXPECT_EQ(api.Snapshot().device, view.Get());
        EXPECT_EQ(api.Snapshot().capturedSwapchain, swapchain.Get());
        EXPECT_EQ(unavailable->DeviceQueries(), 1u);
        api.bind(queue.Get(), swapchain.Get());
        const auto restored = api.Snapshot();
        EXPECT_EQ(restored.queue, queue.Get());
        EXPECT_EQ(restored.device, device.Get());
        EXPECT_EQ(restored.capturedSwapchain, swapchain.Get());
    }
    EXPECT_EQ(g_queueProbeObjects.load(), 0u);
    EXPECT_EQ(g_deviceViewProbeObjects.load(), 0u);
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
