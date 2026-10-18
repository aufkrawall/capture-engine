#include "tests/flow/flow_test_support.h"

#include "hook/runtime/api_translator_modules.h"

// Windows' own d3d9.dll must classify as the system runtime: CE probes and hooks D3D9 only then
// (ShouldBootstrapD3D9Hooks), so a native D3D9 game misread as "non-system" would lose its overlay.
// Since 2026-10-18 the classification also names DXVK, DXUP or another replacement by version
// resource; DXUP's d3d9.dll was reported as DXVK until then.
namespace {
using ce::api_translator::D3D9RuntimeKind;

TEST(FlowD3D9RuntimeKind, WindowsD3D9IsTheSystemRuntime) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    const auto kind = reinterpret_cast<int (*)()>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_D3D9RuntimeKind"));
    ASSERT_NE(kind, nullptr);
    const bool loadedBefore = GetModuleHandleW(L"d3d9.dll") != nullptr;
    const HMODULE d3d9 = LoadLibraryExW(L"d3d9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(d3d9, nullptr);
    EXPECT_EQ(kind(), static_cast<int>(D3D9RuntimeKind::kSystem));
    // Cached per loaded image: asked again, the same answer.
    EXPECT_EQ(kind(), static_cast<int>(D3D9RuntimeKind::kSystem));
    if (!loadedBefore) {
        FreeLibrary(d3d9);
        if (!GetModuleHandleW(L"d3d9.dll"))
            EXPECT_EQ(kind(), static_cast<int>(D3D9RuntimeKind::kNotLoaded)) << "an unloaded d3d9.dll is asked again";
    }
}

}  // namespace
