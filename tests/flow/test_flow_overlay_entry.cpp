#include "tests/flow/flow_test_support.h"
#include "tests/flow/translation_frontend.h"
#include "common/platform/raii_helpers.h"
#include <array>
#include <cstring>
#include <filesystem>

namespace {
using namespace ce::flow;
using namespace ce::flow::translation;

TEST(FlowOverlayEntry, LateEntryRestoreCyclesPreserveBothPresentMethodsAndOutputWork) {
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    ASSERT_TRUE(game.RenderFrame()) << game.Error();
    ComPtr<IDXGISwapChain4> native;
    ASSERT_EQ(game.RetainUnderlyingGameSwapchain().As(&native), S_OK);
    game.ReleasePresentVTableHooks();
    const auto hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto state = reinterpret_cast<unsigned (*)()>(GetProcAddress(hook, "CEFlow_PresentHookState"));
    using Writer = bool (*)(void*, const void*, const void*, size_t);
    const auto writer = reinterpret_cast<Writer>(GetProcAddress(hook, "CEFlow_WritePresentEntry"));
    const auto capture = reinterpret_cast<void (*)(bool)>(GetProcAddress(hook, "CEFlow_SetInjectCapture"));
    const auto frames = reinterpret_cast<uint32_t (*)()>(GetProcAddress(hook, "CEFlow_InjectCaptureFrames"));
    const auto advance = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook, "CEFlow_AdvanceClock"));
    ASSERT_NE(state, nullptr);
    ASSERT_NE(writer, nullptr);
    ASSERT_NE(capture, nullptr);
    ASSERT_NE(frames, nullptr);
    ASSERT_NE(advance, nullptr);
    // No overlay has loaded yet. Both methods must already have a body view and no entry prepend.
    ASSERT_EQ(state(), 12u);
    void** vtable = *reinterpret_cast<void***>(native.Get());
    void* targets[] = {vtable[8], vtable[22]};
    std::array<std::array<unsigned char, 14>, 2> pristine{};
    for (unsigned method = 0; method < 2; ++method)
        std::memcpy(pristine[method].data(), targets[method], pristine[method].size());

    const auto device = CreateFrontendDevice();
    ASSERT_NE(device, nullptr);
    ComPtr<ApiFrontend> frontend;
    frontend.Attach(new ApiFrontend(native.Get(), device.Get()));
    ASSERT_TRUE(game.InstallPresentVTableHooks(frontend.Get()));
    const auto path = std::filesystem::path(game.LogDirectory()) / "RTSSHooks64.dll";
    ASSERT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(std::filesystem::copy_file("sl.reflex.dll", path));
    const auto removeFile = ce::make_scope_guard([&] { std::filesystem::remove(path); });
    const auto overlay = LoadLibraryW(path.c_str());
    ASSERT_NE(overlay, nullptr);
    const auto unload = ce::make_scope_guard([&] { FreeLibrary(overlay); });
    const auto install = reinterpret_cast<bool (*)(void*, bool, Writer)>(
        GetProcAddress(overlay, "CEFake_InstallPresentEntry"));
    const auto remove = reinterpret_cast<bool (*)(bool)>(GetProcAddress(overlay, "CEFake_RemovePresentEntry"));
    const auto calls = reinterpret_cast<uint32_t (*)(bool)>(GetProcAddress(overlay, "CEFake_PresentEntryCalls"));
    ASSERT_NE(install, nullptr);
    ASSERT_NE(remove, nullptr);
    ASSERT_NE(calls, nullptr);
    capture(true);
    const auto stopCapture = ce::make_scope_guard([&] { capture(false); });
    const auto before = game.Coverage();
    const auto gammaBefore = game.PostProcess();
    unsigned expected = 0;
    // Repeat late install/removal without sleeps or game-specific routing, exercising a translated caller too.
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        bool installed[2]{};
        const auto detach = ce::make_scope_guard([&] {
            for (unsigned method = 0; method < 2; ++method)
                if (installed[method]) EXPECT_TRUE(remove(method != 0));
        });
        for (unsigned method = 0; method < 2; ++method) {
            ASSERT_TRUE(install(targets[method], method != 0, writer));
            installed[method] = true;
        }
        for (unsigned frame = 0; frame < 8; ++frame) {
            advance(FlowGame::kFrameIntervalMicroseconds);
            const bool present1 = frame % 2 != 0;
            DXGI_PRESENT_PARAMETERS parameters{};
            const HRESULT result = frame % 4 >= 2 ? PresentFrontend(frontend.Get(), present1)
                : (present1 ? native->Present1(0, 0, &parameters) : native->Present(0, 0));
            ASSERT_TRUE(SUCCEEDED(result)) << result;
            ++expected;
            EXPECT_EQ(frames(), expected);
            EXPECT_EQ(calls(false) + calls(true), frame + 1);
            EXPECT_EQ(state(), 12u);
        }
        EXPECT_EQ(calls(false), 4u);
        EXPECT_EQ(calls(true), 4u);
    }
    for (unsigned method = 0; method < 2; ++method)
        EXPECT_EQ(std::memcmp(pristine[method].data(), targets[method], pristine[method].size()), 0);
    const auto after = game.Coverage();
    EXPECT_EQ(after.presents - before.presents, expected);
    EXPECT_EQ(after.uncovered, before.uncovered);
    EXPECT_EQ(after.doubleDraws, before.doubleDraws);
    EXPECT_EQ(game.PostProcess().corrected - gammaBefore.corrected, expected);
    EXPECT_EQ(game.PostProcess().uncorrected, gammaBefore.uncorrected);
    ExpectNoDebugLayerErrors();
}
}  // namespace
