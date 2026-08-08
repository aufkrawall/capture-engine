#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <memory>

#include "../hook/common/ngx_fg_preset_override.h"
#include "../hook/common/reflex_defs.h"
#include "../hook/wrappers/iat_hook.h"

namespace {

using ce::ngx_fg_preset::FillSubstitutedSetting;
using ce::ngx_fg_preset::GetConfiguredPreset;
using ce::ngx_fg_preset::IsFrameGenerationSnippetModulePath;
using ce::ngx_fg_preset::kNvApiIdDrsGetSetting;
using ce::ngx_fg_preset::kNvDrsCurrentProfileLocation;
using ce::ngx_fg_preset::kNvDrsDwordType;
using ce::ngx_fg_preset::kNvDrsSettingVer1;
using ce::ngx_fg_preset::kRenderPresetDrsSettingId;
using ce::ngx_fg_preset::NormalizePreset;
using ce::ngx_fg_preset::NvDrsSetting;
using ce::ngx_fg_preset::PresetIdToLetter;
using ce::ngx_fg_preset::SetConfiguredPreset;
using ce::ngx_fg_preset::ShouldSubstituteSetting;
using ce::ngx_fg_preset::ShouldWrapQueryInterface;

// Restores the process-wide preset so ordering between tests cannot matter.
class NgxFgPresetOverrideTest : public ::testing::Test {
protected:
    void SetUp() override { savedPreset_ = GetConfiguredPreset(); }
    void TearDown() override { SetConfiguredPreset(savedPreset_); }

private:
    uint32_t savedPreset_ = 0;
};

TEST_F(NgxFgPresetOverrideTest, PresetNormalizationCoversTheWholeAlphabet) {
    EXPECT_EQ(NormalizePreset(0u), 0u);
    EXPECT_EQ(NormalizePreset(1u), 1u);
    EXPECT_EQ(NormalizePreset(2u), 2u);
    // NVIDIA defines only A and B today; the driver value is a plain index, so
    // later letters must survive rather than be clamped away.
    EXPECT_EQ(NormalizePreset(26u), 26u);
    EXPECT_EQ(NormalizePreset(27u), 0u);
    EXPECT_EQ(NormalizePreset(0xFFFFFFFFu), 0u);

    EXPECT_EQ(PresetIdToLetter(1u), 'A');
    EXPECT_EQ(PresetIdToLetter(2u), 'B');
    EXPECT_EQ(PresetIdToLetter(26u), 'Z');
    EXPECT_EQ(PresetIdToLetter(0u), '?');
    EXPECT_EQ(PresetIdToLetter(27u), '?');
}

TEST_F(NgxFgPresetOverrideTest, OnlyTheFrameGenerationSnippetIsRecognized) {
    EXPECT_TRUE(IsFrameGenerationSnippetModulePath("C:\\game\\nvngx_dlssg.dll"));
    EXPECT_TRUE(IsFrameGenerationSnippetModulePath("c:\\game\\NVNGX_DLSSG.DLL"));

    // The super-resolution snippet, the Streamline plugins, the NGX core and the
    // game itself must keep the untouched driver pointer.
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\nvngx_dlss.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\sl.dlss_g.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\sl.common.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\windows\\system32\\nvngx.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\game.exe"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath(nullptr));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath(""));
}

TEST_F(NgxFgPresetOverrideTest, QueryInterfaceIsClaimedOnlyForTheDrsGetterFromTheSnippet) {
    const char* snippet = "C:\\game\\nvngx_dlssg.dll";

    EXPECT_TRUE(ShouldWrapQueryInterface(1u, kNvApiIdDrsGetSetting, snippet));

    // Unconfigured must never touch the resolution path at all.
    EXPECT_FALSE(ShouldWrapQueryInterface(0u, kNvApiIdDrsGetSetting, snippet));
    EXPECT_FALSE(ShouldWrapQueryInterface(27u, kNvApiIdDrsGetSetting, snippet));

    // Reflex's SetSleepMode/Sleep and every other NvAPI entry stay untouched.
    EXPECT_FALSE(ShouldWrapQueryInterface(1u, NVAPI_ID_D3D_SetSleepMode, snippet));
    EXPECT_FALSE(ShouldWrapQueryInterface(1u, NVAPI_ID_D3D_Sleep, snippet));

    EXPECT_FALSE(ShouldWrapQueryInterface(1u, kNvApiIdDrsGetSetting, "C:\\game\\sl.dlss_g.dll"));
    EXPECT_FALSE(ShouldWrapQueryInterface(1u, kNvApiIdDrsGetSetting, "C:\\game\\game.exe"));
    EXPECT_FALSE(ShouldWrapQueryInterface(1u, kNvApiIdDrsGetSetting, nullptr));
}

TEST_F(NgxFgPresetOverrideTest, SubstitutionIsLimitedToTheRenderPresetSettingAndKnownStructVersion) {
    EXPECT_TRUE(ShouldSubstituteSetting(2u, kRenderPresetDrsSettingId, kNvDrsSettingVer1));

    EXPECT_FALSE(ShouldSubstituteSetting(0u, kRenderPresetDrsSettingId, kNvDrsSettingVer1));

    // The other seven DRS keys the snippet reads in the same loop (private flags,
    // multi-frame count limits, ...) must pass through untouched.
    EXPECT_FALSE(ShouldSubstituteSetting(2u, 0x10E41DF6u, kNvDrsSettingVer1));
    EXPECT_FALSE(ShouldSubstituteSetting(2u, 0x104596A1u, kNvDrsSettingVer1));
    EXPECT_FALSE(ShouldSubstituteSetting(2u, 0x10308298u, kNvDrsSettingVer1));

    // A struct version this ABI mirror does not describe must be forwarded, not
    // written into.
    EXPECT_FALSE(ShouldSubstituteSetting(2u, kRenderPresetDrsSettingId, 0u));
    EXPECT_FALSE(ShouldSubstituteSetting(2u, kRenderPresetDrsSettingId, kNvDrsSettingVer1 + 0x10000u));
}

TEST_F(NgxFgPresetOverrideTest, SubstitutedSettingLooksLikeAnExplicitCurrentProfileValue) {
    auto setting = std::make_unique<NvDrsSetting>();
    memset(setting.get(), 0xCD, sizeof(NvDrsSetting));
    setting->version = kNvDrsSettingVer1;

    FillSubstitutedSetting(*setting, kRenderPresetDrsSettingId, 2u);

    EXPECT_EQ(setting->settingId, kRenderPresetDrsSettingId);
    EXPECT_EQ(setting->settingType, kNvDrsDwordType);
    // The snippet ignores a value that does not claim to come from the current
    // profile, and treats a predefined value as "not set".
    EXPECT_EQ(setting->settingLocation, kNvDrsCurrentProfileLocation);
    EXPECT_EQ(setting->isCurrentPredefined, 0u);
    EXPECT_EQ(setting->isPredefinedValid, 0u);
    EXPECT_EQ(setting->currentValue.u32Value, 2u);
    // The caller's version stamp is left alone.
    EXPECT_EQ(setting->version, kNvDrsSettingVer1);
}

TEST_F(NgxFgPresetOverrideTest, SubstitutedSettingRejectsOutOfRangePresetValues) {
    auto setting = std::make_unique<NvDrsSetting>();
    memset(setting.get(), 0, sizeof(NvDrsSetting));

    FillSubstitutedSetting(*setting, kRenderPresetDrsSettingId, 99u);
    EXPECT_EQ(setting->currentValue.u32Value, 0u);
}

TEST_F(NgxFgPresetOverrideTest, ConfiguredPresetRoundTripsAndRejectsOutOfRange) {
    SetConfiguredPreset(0u);
    EXPECT_EQ(GetConfiguredPreset(), 0u);
    EXPECT_FALSE(ce::ngx_fg_preset::IsArmed());

    SetConfiguredPreset(2u);
    EXPECT_EQ(GetConfiguredPreset(), 2u);
    EXPECT_TRUE(ce::ngx_fg_preset::IsArmed());

    SetConfiguredPreset(99u);
    EXPECT_EQ(GetConfiguredPreset(), 0u);
    EXPECT_FALSE(ce::ngx_fg_preset::IsArmed());
}

TEST_F(NgxFgPresetOverrideTest, DynamicHookExceptionIsScopedToTheSnippetAndOneExport) {
    using IATHook::ShouldAllowNgxFrameGenerationPresetDynamicHook;

    EXPECT_TRUE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "nvapi_QueryInterface"));

    // Unarmed, the DLSS-G snippet keeps the blanket Streamline/FG bypass.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(false, true, "nvapi_QueryInterface"));
    // Other Streamline/FG modules are never granted the exception.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, false, "nvapi_QueryInterface"));
    // And no other export is pulled through it.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "nvapi_Direct_GetMethod"));
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "CreateDXGIFactory2"));
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, nullptr));
}

}  // namespace
