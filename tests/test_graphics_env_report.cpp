#include <gtest/gtest.h>

#include <string>

#include "hook/runtime/graphics_env_report.h"

// The 2026-10-29 vkd3d-proton A/B runs set VKD3D_SWAPCHAIN_PRESENT_MODE / VKD3D_DISABLE_EXTENSIONS for GTA, and
// whether they reached the game could only be inferred from swapchain flags. The hook now lists them.

namespace {

namespace report = ce::graphics_env_report;

// A GetEnvironmentStringsW block: entries separated by NUL, terminated by an empty entry.
std::wstring Block(std::initializer_list<const wchar_t*> entries) {
    std::wstring block;
    for (const wchar_t* entry : entries) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

TEST(GraphicsEnvReport, ListsOnlyTheTranslationLayerAndDriverVariables) {
    const std::wstring block = Block({L"=C:=C:\\Games", L"PATH=C:\\Windows", L"VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE",
                                      L"dxvk_hud=fps", L"VKTEMP=1", L"VK_LOADER_LAYERS_DISABLE=~implicit~",
                                      L"USERNAME=someone", L"__GL_SYNC_TO_VBLANK=0"});
    const report::Report r = report::Collect(block.c_str());
    ASSERT_EQ(r.matched, 4u);
    ASSERT_EQ(r.entries.size(), 4u);
    EXPECT_EQ(r.entries[0], L"VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE");
    EXPECT_EQ(r.entries[1], L"dxvk_hud=fps") << "Windows variable names are case-insensitive";
    EXPECT_EQ(r.entries[2], L"VK_LOADER_LAYERS_DISABLE=~implicit~");
    EXPECT_EQ(r.entries[3], L"__GL_SYNC_TO_VBLANK=0");
}

TEST(GraphicsEnvReport, CutsLongValuesAndCapsTheList) {
    const std::wstring longValue(report::kMaxValueChars + 40, L'x');
    const std::wstring longEntry = L"VKD3D_CONFIG=" + longValue;
    std::wstring block = longEntry;
    block.push_back(L'\0');
    for (std::size_t i = 0; i < report::kMaxEntries + 3; ++i) {
        block += L"DXVK_V" + std::to_wstring(i) + L"=1";
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    const report::Report r = report::Collect(block.c_str());
    EXPECT_EQ(r.matched, report::kMaxEntries + 4);
    EXPECT_EQ(r.entries.size(), report::kMaxEntries);
    EXPECT_EQ(r.entries[0],
              L"VKD3D_CONFIG=" + longValue.substr(0, report::kMaxValueChars) + L"...(" +
                  std::to_wstring(longValue.size()) + L" chars)");
}

TEST(GraphicsEnvReport, EmptyOrMissingEnvironmentListsNothing) {
    EXPECT_EQ(report::Collect(nullptr).matched, 0u);
    const std::wstring block = Block({});
    EXPECT_EQ(report::Collect(block.c_str()).matched, 0u);
    const std::wstring noEquals = Block({L"VKD3D_BROKEN"});
    EXPECT_EQ(report::Collect(noEquals.c_str()).matched, 0u);
}

}  // namespace
