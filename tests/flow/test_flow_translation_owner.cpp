#include "tests/flow/flow_test_support.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"

namespace {
using namespace ce::flow;

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
    ASSERT_EQ(frontend->Present(0, 0), S_OK);
    DXGI_PRESENT_PARAMETERS parameters{};
    advance(FlowGame::kFrameIntervalMicroseconds);
    ASSERT_EQ(frontend->Present1(0, 0, &parameters), S_OK);
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 2u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 2u);
    capture(false);
    ExpectNoDebugLayerErrors();
}
}  // namespace
