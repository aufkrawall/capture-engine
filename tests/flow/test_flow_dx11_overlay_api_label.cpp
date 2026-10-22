#include "tests/flow/flow_test_support.h"

#include "hook/runtime/api_translator_modules.h"

#include <string>

// The D3D11 overlay's API label (hook/d3d11/dx11_hook_overlay.cpp) while a d3d9.dll replacement sits
// over Windows' D3D11. DXUP's D3D9 frontend renders through whatever d3d11/dxgi the game folder offers;
// with only its d3d9.dll installed that is Windows', and the overlay read "DX11" (Mirror's Edge,
// 20261022_162106). Another replacement under the same file name (ReShade here) is no translator: the
// D3D11 label stays. The stand-ins differ only in their version resource (tests/flow/fakes/translators).
namespace {
using ce::api_translator::D3D9RuntimeKind;

HMODULE FlowHook() {
    return GetModuleHandleW(L"capture_hook_x64.dll");
}

std::string OverlayLabel(unsigned d3d11Minor) {
    const auto label =
        reinterpret_cast<void (*)(unsigned, char*, size_t)>(GetProcAddress(FlowHook(), "CEFlow_DX11OverlayApiLabel"));
    char text[64] = {};
    if (label)
        label(d3d11Minor, text, sizeof(text));
    return text;
}

int D3D9RuntimeKindNow() {
    const auto kind = reinterpret_cast<int (*)()>(GetProcAddress(FlowHook(), "CEFlow_D3D9RuntimeKind"));
    return kind ? kind() : -1;
}

TEST(FlowDX11OverlayApiLabel, DxupD3D9OverWindowsD3D11NamesTheGamesApi) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    ASSERT_EQ(GetModuleHandleW(L"d3d9.dll"), nullptr) << "the stand-in must be the process's d3d9.dll";
    EXPECT_EQ(OverlayLabel(0), "DX11");

    const HMODULE frontend = LoadLibraryA("dxup\\d3d9.dll");
    ASSERT_NE(frontend, nullptr) << GetLastError();
    EXPECT_EQ(D3D9RuntimeKindNow(), static_cast<int>(D3D9RuntimeKind::kDXUP));
    EXPECT_EQ(OverlayLabel(0), "DX9 (DXUP)");
    EXPECT_EQ(OverlayLabel(1), "DX9 (DXUP)") << "the game's API, not the revision of DXUP's own D3D11 use";
    FreeLibrary(frontend);
}

TEST(FlowDX11OverlayApiLabel, AnotherD3D9ReplacementKeepsTheD3D11Label) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    ASSERT_EQ(GetModuleHandleW(L"d3d9.dll"), nullptr) << "the stand-in must be the process's d3d9.dll";

    const HMODULE replacement = LoadLibraryA("reshade\\d3d9.dll");
    ASSERT_NE(replacement, nullptr) << GetLastError();
    EXPECT_EQ(D3D9RuntimeKindNow(), static_cast<int>(D3D9RuntimeKind::kOtherReplacement));
    EXPECT_EQ(OverlayLabel(0), "DX11");
    EXPECT_EQ(OverlayLabel(1), "DX11.1");
    FreeLibrary(replacement);
}

}  // namespace
