#include "tests/flow/flow_test_support.h"
#include "tests/flow/translation_frontend.h"

namespace {
using namespace ce::flow;
using namespace ce::flow::translation;

TEST(FlowTranslationOwner, UnscopedFrontendDelegatesBeforeItsFirstPresentAndCachesTheDecision) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    ComPtr<ApiFrontend> frontend;
    frontend.Attach(new ApiFrontend(native.Get(), device.Get()));
    game.ReleasePresentVTableHooks();  // move the physical vtable interception above the native inline hook
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(hook, "CEFlow_HasTranslationOwner"));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    ASSERT_NE(owns, nullptr);
    capture(true);
    const auto before = game.Coverage();
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(PresentFrontend(frontend.Get(), false)));
    const unsigned queries = frontend->deviceQueries;
    EXPECT_EQ(queries, 2u);  // rejects DX12, proves DX11; no subsequent device queries
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(PresentFrontend(frontend.Get(), true)));
    EXPECT_TRUE(owns(frontend.Get()));
    EXPECT_FALSE(owns(native.Get()));
    EXPECT_EQ(frontend->deviceQueries, queries);
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 2u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 2u);
    capture(false);
    ExpectNoDebugLayerErrors();
}

// Late injection (Heroes of the Storm + DXUP, 20261025_230718): CE never saw the native output being created, so a
// forwarding frontend had no evidence to prove itself with, and once both Present hooks landed the frontend and the
// output would each have processed every frame. The output's first Present CE processes publishes that evidence.
TEST(FlowTranslationOwner, LateInjectedOutputProvesItsFrontendFromItsFirstProcessedPresent) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto forget =
        reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(hook, "CEFlow_ForgetNativeOutputCreation"));
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(hook, "CEFlow_HasTranslationOwner"));
    ASSERT_NE(forget, nullptr);
    ASSERT_NE(owns, nullptr);
    ASSERT_TRUE(forget(native.Get()));
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    ComPtr<ApiFrontend> frontend;  // forwards private data to the output, as DXUP's D3D11 swapchain does
    frontend.Attach(new ApiFrontend(native.Get(), device.Get()));
    EXPECT_FALSE(owns(frontend.Get()));
    EXPECT_EQ(frontend->deviceQueries, 0u);  // nothing forwarded yet, so no verdict is cached either
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    EXPECT_TRUE(owns(frontend.Get()));
    EXPECT_FALSE(owns(native.Get()));
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, NativeApiProxiesAndUnforwardedDataKeepTheirOwnership) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_HasTranslationOwner"));
    ASSERT_NE(owns, nullptr);
    ComPtr<ApiFrontend> nativeProxy;
    nativeProxy.Attach(new ApiFrontend(native.Get(), nullptr));
    EXPECT_FALSE(owns(nativeProxy.Get()));
    // A native proxy's negative verdict is not cached: caching it would need a Release hook on a
    // DXGI vtable shared by every swapchain of that class. Each probe costs one device query.
    const auto queries = nativeProxy->deviceQueries;
    EXPECT_EQ(queries, 1u);
    EXPECT_FALSE(owns(nativeProxy.Get()));
    EXPECT_EQ(nativeProxy->deviceQueries, queries + 1u);
    ComPtr<ApiFrontend> unforwarded;
    unforwarded.Attach(new ApiFrontend(native.Get(), device.Get(), false));
    EXPECT_FALSE(owns(unforwarded.Get()));
    EXPECT_EQ(unforwarded->deviceQueries, 0u);
    EXPECT_FALSE(owns(native.Get()));
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, ForwardedPrivateDataDelegatesOnlyTheFrontendPresentAndPresent1) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    ComPtr<fake::ForwardingSwapChain> frontend;
    frontend.Attach(new fake::ForwardingSwapChain(native.Get()));
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    using PublishOwner = bool (*)(IDXGISwapChain*, IDXGISwapChain*);
    auto publish = reinterpret_cast<PublishOwner>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                                "CEFlow_PublishTranslationOwner"));
    ASSERT_NE(publish, nullptr);
    ASSERT_TRUE(publish(frontend.Get(), native.Get()));
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    capture(true);
    const auto before = game.Coverage();
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(frontend->Present(0, 0)));
    DXGI_PRESENT_PARAMETERS parameters{};
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_TRUE(SUCCEEDED(frontend->Present1(0, 0, &parameters)));
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 2u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 2u);
    capture(false);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, RecycledFrontendRechecksBothPositiveAndNegativeDecisions) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_HasTranslationOwner"));
    ASSERT_NE(owns, nullptr);
    // Reuse exactly the same allocation, through actual COM Release and destruction.
    alignas(PlacementApiFrontend) unsigned char storage[sizeof(PlacementApiFrontend)];
    auto* frontend = new (storage) PlacementApiFrontend(native.Get(), device.Get(), true);
    EXPECT_TRUE(owns(frontend));
    EXPECT_EQ(ReleaseFrontend(frontend), 0u);
    frontend = new (storage) PlacementApiFrontend(native.Get(), nullptr, true);
    EXPECT_FALSE(owns(frontend));
    EXPECT_EQ(frontend->deviceQueries, 1u);
    EXPECT_EQ(ReleaseFrontend(frontend), 0u);
    frontend = new (storage) PlacementApiFrontend(native.Get(), device.Get(), true);
    EXPECT_TRUE(owns(frontend));
    EXPECT_EQ(frontend->deviceQueries, 2u);
    // A surviving reference must preserve the current lifetime's decision.
    EXPECT_EQ(frontend->AddRef(), 2u);
    EXPECT_EQ(ReleaseFrontend(frontend), 1u);
    EXPECT_TRUE(owns(frontend));
    EXPECT_EQ(frontend->deviceQueries, 2u);
    EXPECT_EQ(ReleaseFrontend(frontend), 0u);
    EXPECT_FALSE(owns(native.Get()));
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, RecycledFrontendCannotUseACacheWhileFinalReleaseIsReturning) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    const auto owns = reinterpret_cast<bool (*)(IDXGISwapChain*)>(GetProcAddress(
        GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_HasTranslationOwner"));
    ASSERT_NE(owns, nullptr);
    alignas(PlacementApiFrontend) unsigned char storage[sizeof(PlacementApiFrontend)];
    auto* frontend = new (storage) PlacementApiFrontend(native.Get(), device.Get(), true);
    EXPECT_TRUE(owns(frontend));
    PlacementApiFrontend* replacement = nullptr;
    frontend->afterFinalRelease = [&] {
        replacement = new (storage) PlacementApiFrontend(native.Get(), nullptr, true);
        // The old object's predecessor has destroyed it, but has not returned to CE yet.
        EXPECT_FALSE(owns(replacement));
        EXPECT_EQ(replacement->deviceQueries, 1u);
    };
    EXPECT_EQ(ReleaseFrontend(frontend), 0u);
    ASSERT_NE(replacement, nullptr);
    EXPECT_FALSE(owns(replacement));
    EXPECT_EQ(replacement->deviceQueries, 2u);
    EXPECT_FALSE(owns(replacement));
    EXPECT_EQ(replacement->deviceQueries, 2u);
    EXPECT_EQ(ReleaseFrontend(replacement), 0u);
    EXPECT_FALSE(owns(native.Get()));
    ExpectNoDebugLayerErrors();
}
}  // namespace
