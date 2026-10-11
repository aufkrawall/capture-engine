#include "tests/flow/flow_test_support.h"
#include "tests/flow/translation_frontend.h"

namespace {
using namespace ce::flow;
using namespace ce::flow::translation;

class BorrowedIdentityFrontend final : public ApiFrontend {
public:
    using ApiFrontend::ApiFrontend;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        return iid == __uuidof(IUnknown) ? real_->QueryInterface(iid, out) : ApiFrontend::QueryInterface(iid, out);
    }
};

TEST(FlowTranslationOwner, NestedFrontendsCacheBothPresentMethodsWithoutRepeatedPublication) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    ComPtr<ApiFrontend> inner;
    inner.Attach(new ApiFrontend(native.Get(), device.Get()));
    ComPtr<ApiFrontend> outer;
    outer.Attach(new ApiFrontend(inner.Get(), device.Get()));
    game.ReleasePresentVTableHooks();
    ASSERT_TRUE(game.InstallPresentVTableHooks(outer.Get()));
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
    for (unsigned frame = 0; frame < 6; ++frame) {
        advance(FlowGame::kFrameIntervalMicroseconds);
        ASSERT_EQ(PresentFrontend(outer.Get(), frame % 2 != 0), S_OK);
        EXPECT_TRUE(owns(outer.Get()));
        EXPECT_TRUE(owns(inner.Get()));
        EXPECT_FALSE(owns(native.Get()));
        EXPECT_EQ(outer->deviceQueries, 2u);
        EXPECT_EQ(inner->deviceQueries, 2u);
        EXPECT_EQ(outer->privateDataWrites, 1u);
        EXPECT_EQ(inner->privateDataWrites, 2u);  // own publication plus the outer forwarded write
    }
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 6u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 6u);
    capture(false);
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, NativeAndTranslatedViewsOfOneOutputKeepIndependentDecisions) {
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
    ComPtr<ApiFrontend> translated, nativeProxy;
    translated.Attach(new ApiFrontend(native.Get(), device.Get()));
    nativeProxy.Attach(new ApiFrontend(native.Get(), nullptr));
    for (unsigned pass = 0; pass < 4; ++pass) {
        EXPECT_TRUE(owns(translated.Get()));
        EXPECT_FALSE(owns(nativeProxy.Get()));
        EXPECT_FALSE(owns(native.Get()));
        EXPECT_EQ(translated->deviceQueries, 2u);
        EXPECT_EQ(nativeProxy->deviceQueries, 1u);
        EXPECT_EQ(translated->privateDataWrites, 1u);
        EXPECT_EQ(nativeProxy->privateDataWrites, 0u);
    }
    ExpectNoDebugLayerErrors();
}

TEST(FlowTranslationOwner, ForwardedUnknownCannotSupplyAFrontendLifetime) {
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
    ComPtr<BorrowedIdentityFrontend> frontend;
    frontend.Attach(new BorrowedIdentityFrontend(native.Get(), device.Get()));
    EXPECT_TRUE(owns(frontend.Get()));
    EXPECT_TRUE(owns(frontend.Get()));
    EXPECT_EQ(frontend->deviceQueries, 4u);  // delegation still works, without borrowing the output's lifetime
    EXPECT_FALSE(owns(native.Get()));
    ExpectNoDebugLayerErrors();
}
}  // namespace
