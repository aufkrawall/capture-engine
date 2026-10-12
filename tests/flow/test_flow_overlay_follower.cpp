#include "tests/flow/flow_test_support.h"
#include "tests/flow/translation_frontend.h"
#include "common/platform/raii_helpers.h"
#include <filesystem>

namespace {
using namespace ce::flow;
using namespace ce::flow::translation;
using Forward = HRESULT (*)(IDXGISwapChain*, bool);

class OverlayFrontend final : public ApiFrontend {
public:
    OverlayFrontend(IDXGISwapChain4* native, IUnknown* device, Forward& forward)
        : ApiFrontend(native, device), forward_(forward) {}
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT) override {
        return forward_ ? forward_(real_, false) : real_->Present(0, 0);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT, UINT, const DXGI_PRESENT_PARAMETERS* parameters) override {
        return forward_ ? forward_(real_, true) : real_->Present1(0, 0, parameters);
    }
private:
    Forward& forward_;
};

TEST(FlowOverlayFollower, LateOverlayForwardingKeepsNativeAndTranslatedOutputsCovered) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    Forward forward = nullptr;
    ComPtr<OverlayFrontend> frontend;
    frontend.Attach(new OverlayFrontend(native.Get(), device.Get(), forward));
    game.ReleasePresentVTableHooks();
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    const auto helper = reinterpret_cast<void (*)(IDXGISwapChain*, bool)>(
        GetProcAddress(hook, "CEFlow_SetOverlayHelper"));
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    ASSERT_NE(helper, nullptr);
    capture(true);
    const auto stopCapture = ce::make_scope_guard([&] { capture(false); });
    const auto before = game.Coverage();
    const auto gammaBefore = game.PostProcess();
    const auto render = [&](bool translated, bool present1) {
        advance(FlowGame::kFrameIntervalMicroseconds);
        if (translated) return PresentFrontend(frontend.Get(), present1);
        if (forward) return forward(native.Get(), present1);
        DXGI_PRESENT_PARAMETERS parameters{};
        return present1 ? native->Present1(0, 0, &parameters) : native->Present(0, 0);
    };
    for (unsigned frame = 0; frame < 4; ++frame)
        ASSERT_TRUE(SUCCEEDED(render(frame / 2 != 0, frame % 2 != 0)));

    // Reuse a built fake DLL under a real overlay module name, after CE has rendered.
    const auto path = std::filesystem::path(game.LogDirectory()) / "RTSSHooks64.dll";
    ASSERT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(std::filesystem::copy_file("sl.reflex.dll", path));
    const auto removeFake = ce::make_scope_guard([&] { std::filesystem::remove(path); });
    const auto overlay = LoadLibraryW(path.c_str());
    ASSERT_NE(overlay, nullptr);
    const auto unloadFake = ce::make_scope_guard([&] { forward = nullptr; FreeLibrary(overlay); });
    forward = reinterpret_cast<Forward>(GetProcAddress(overlay, "CEFake_ForwardPresent"));
    ASSERT_NE(forward, nullptr);
    for (unsigned frame = 0; frame < 8; ++frame)
        ASSERT_TRUE(SUCCEEDED(render(frame % 4 >= 2, frame % 2 != 0)));
    forward = nullptr;
    for (unsigned frame = 0; frame < 4; ++frame)
        ASSERT_TRUE(SUCCEEDED(render(frame / 2 != 0, frame % 2 != 0)));
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, 16u);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(frames(), 16u);
    const auto gammaAfter = game.PostProcess();
    EXPECT_EQ(gammaAfter.corrected - gammaBefore.corrected, 16u);
    EXPECT_EQ(gammaAfter.uncorrected, gammaBefore.uncorrected);

    // Creation-tagged helper traffic must still leave capture and gamma untouched.
    helper(native.Get(), true);
    const auto untag = ce::make_scope_guard([&] { helper(native.Get(), false); });
    ASSERT_TRUE(SUCCEEDED(render(false, false)));
    ASSERT_TRUE(SUCCEEDED(render(false, true)));
    EXPECT_EQ(frames(), 16u);
    EXPECT_EQ(game.PostProcess().corrected, gammaAfter.corrected);
    ExpectNoDebugLayerErrors();
}
}  // namespace
