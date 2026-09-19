#include <gtest/gtest.h>

#include <windows.h>

#include <cstring>
#include <memory>

#include "../hook/common/ngx_drs_override.h"
#include "../hook/common/reflex_defs.h"
#include "../hook/wrappers/iat_hook.h"

namespace {

using ce::ngx_drs::DlssDrsOverrides;
using ce::ngx_drs::FillSubstitutedSetting;
using ce::ngx_drs::GetConfiguredOverrides;
using ce::ngx_drs::GetConfiguredPreset;
using ce::ngx_drs::HasAnyOverride;
using ce::ngx_drs::IsDlssDrsConsumerModule;
using ce::ngx_drs::IsDlssDrsConsumerModuleName;
using ce::ngx_drs::IsFrameGenerationSnippetModulePath;
using ce::ngx_drs::kDrsDynamicTargetFrameRateAuto;
using ce::ngx_drs::kDrsFrameGenerationModeAuto;
using ce::ngx_drs::kDrsFrameGenerationModeDynamic;
using ce::ngx_drs::kDrsFrameGenerationModeOff;
using ce::ngx_drs::DrsVSyncModeForPresentOverride;
using ce::ngx_drs::kDrsVSyncModeForceOff;
using ce::ngx_drs::kDrsVSyncModeForceOn;
using ce::ngx_drs::kDrsVSyncModePassive;
using ce::ngx_drs::kVSyncModeDrsSettingId;
using ce::ngx_drs::kDrsFrameGenerationModeOn;
using ce::ngx_drs::kDynamicMultiFrameCountMaxDrsSettingId;
using ce::ngx_drs::kDynamicTargetFrameRateDrsSettingId;
using ce::ngx_drs::kFrameGenerationModeDrsSettingId;
using ce::ngx_drs::kMultiFrameCountDrsSettingId;
using ce::ngx_drs::kNvApiIdDrsGetSetting;
using ce::ngx_drs::kNvDrsCurrentProfileLocation;
using ce::ngx_drs::kNvDrsDwordType;
using ce::ngx_drs::kNvDrsSettingVer1;
using ce::ngx_drs::kRenderPresetDrsSettingId;
using ce::ngx_drs::MultiplierToDrsGeneratedFrames;
using ce::ngx_drs::Normalize;
using ce::ngx_drs::NormalizePreset;
using ce::ngx_drs::NvDrsSetting;
using ce::ngx_drs::PresetIdToLetter;
using ce::ngx_drs::ResolveSubstitutedValue;
using ce::ngx_drs::SetConfiguredOverrides;
using ce::ngx_drs::ShouldSubstituteSetting;
using ce::ngx_drs::ShouldWrapQueryInterface;

DlssDrsOverrides PresetOnly(uint32_t preset) {
    DlssDrsOverrides overrides;
    overrides.renderPreset = preset;
    return overrides;
}

DlssDrsOverrides ModeOnly(uint8_t mode) {
    DlssDrsOverrides overrides;
    overrides.frameGenerationMode = mode;
    return overrides;
}

uint32_t Resolved(const DlssDrsOverrides& overrides, uint32_t settingId) {
    uint32_t value = 0xDEADBEEFu;
    return ResolveSubstitutedValue(overrides, settingId, value) ? value : 0u;
}

// Restores the process-wide overrides so ordering between tests cannot matter.
class NgxDrsOverrideTest : public ::testing::Test {
protected:
    void SetUp() override { saved_ = GetConfiguredOverrides(); }
    void TearDown() override { SetConfiguredOverrides(saved_); }

private:
    DlssDrsOverrides saved_;
};

TEST_F(NgxDrsOverrideTest, PresetNormalizationCoversTheWholeAlphabet) {
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

TEST_F(NgxDrsOverrideTest, OnlyTheFrameGenerationSnippetMatchesTheSnippetPredicate) {
    EXPECT_TRUE(IsFrameGenerationSnippetModulePath("C:\\game\\nvngx_dlssg.dll"));
    EXPECT_TRUE(IsFrameGenerationSnippetModulePath("c:\\game\\NVNGX_DLSSG.DLL"));

    // The super-resolution snippet, the NGX core and the game itself are not
    // the snippet, even though some of them are DRS consumers.
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\nvngx_dlss.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\game\\sl.common.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath("C:\\windows\\system32\\nvngx.dll"));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath(nullptr));
    EXPECT_FALSE(IsFrameGenerationSnippetModulePath(""));
}

TEST_F(NgxDrsOverrideTest, DrsConsumerRecognitionCoversBothReadersAndOtaRenamedPlugins) {
    // nvngx_dlssg reads the render preset; sl.common performs the NvAPI call
    // Streamline's multi-frame keys travel through, and sl.dlss_g asks it to.
    EXPECT_TRUE(IsDlssDrsConsumerModuleName("C:\\game\\nvngx_dlssg.dll"));
    EXPECT_TRUE(IsDlssDrsConsumerModuleName("C:\\game\\sl.common.dll"));
    EXPECT_TRUE(IsDlssDrsConsumerModuleName("C:\\game\\SL.DLSS_G.dll"));
    EXPECT_TRUE(IsDlssDrsConsumerModuleName("C:\\game\\sl.interposer.dll"));

    EXPECT_FALSE(IsDlssDrsConsumerModuleName("C:\\game\\nvngx_dlss.dll"));
    EXPECT_FALSE(IsDlssDrsConsumerModuleName("C:\\game\\game.exe"));
    EXPECT_FALSE(IsDlssDrsConsumerModuleName("C:\\windows\\system32\\nvapi64.dll"));
    EXPECT_FALSE(IsDlssDrsConsumerModuleName(nullptr));
    EXPECT_FALSE(IsDlssDrsConsumerModuleName(""));

    // A Streamline plugin the driver downloaded over the air is mapped under a
    // content-addressed name; only its slGetPluginFunction export identifies it.
    const char* otaPlugin =
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\134656\\files\\160_E658703.dll";
    EXPECT_FALSE(IsDlssDrsConsumerModuleName(otaPlugin));
    EXPECT_TRUE(IsDlssDrsConsumerModule(otaPlugin, /*exportsStreamlinePluginEntry=*/true));
    EXPECT_FALSE(IsDlssDrsConsumerModule(otaPlugin, /*exportsStreamlinePluginEntry=*/false));
    // The export alone is enough; the name check is the fallback, not a gate.
    EXPECT_TRUE(IsDlssDrsConsumerModule("C:\\game\\sl.common.dll", /*exportsStreamlinePluginEntry=*/false));
}

TEST_F(NgxDrsOverrideTest, QueryInterfaceIsClaimedOnlyForTheDrsGetterFromAConsumer) {
    const char* snippet = "C:\\game\\nvngx_dlssg.dll";
    const DlssDrsOverrides preset = PresetOnly(1u);

    EXPECT_TRUE(ShouldWrapQueryInterface(preset, kNvApiIdDrsGetSetting, snippet, false));
    // Streamline's core is now a first-class consumer, not a passed-over module.
    EXPECT_TRUE(ShouldWrapQueryInterface(ModeOnly(kDlssFGModeDynamic), kNvApiIdDrsGetSetting,
                                         "C:\\game\\sl.common.dll", false));

    // Unconfigured must never touch the resolution path at all.
    EXPECT_FALSE(ShouldWrapQueryInterface(DlssDrsOverrides{}, kNvApiIdDrsGetSetting, snippet, false));
    EXPECT_FALSE(ShouldWrapQueryInterface(PresetOnly(27u), kNvApiIdDrsGetSetting, snippet, false));

    // Reflex's SetSleepMode/Sleep and every other NvAPI entry stay untouched.
    EXPECT_FALSE(ShouldWrapQueryInterface(preset, NVAPI_ID_D3D_SetSleepMode, snippet, false));
    EXPECT_FALSE(ShouldWrapQueryInterface(preset, NVAPI_ID_D3D_Sleep, snippet, false));

    EXPECT_FALSE(ShouldWrapQueryInterface(preset, kNvApiIdDrsGetSetting, "C:\\game\\game.exe", false));
    EXPECT_FALSE(ShouldWrapQueryInterface(preset, kNvApiIdDrsGetSetting, nullptr, false));
}

TEST_F(NgxDrsOverrideTest, ForcedModeTranslatesToTheDriverEnum) {
    // EValues_NGX_DLSSG_MODE: 1 off, 2 on, 3 auto, 4 dynamic. CE's own
    // configuration enum is deliberately a different numbering.
    EXPECT_EQ(Resolved(ModeOnly(kDlssFGModeOff), kFrameGenerationModeDrsSettingId), kDrsFrameGenerationModeOff);
    EXPECT_EQ(Resolved(ModeOnly(kDlssFGModeFixed), kFrameGenerationModeDrsSettingId), kDrsFrameGenerationModeOn);
    EXPECT_EQ(Resolved(ModeOnly(kDlssFGModeAuto), kFrameGenerationModeDrsSettingId), kDrsFrameGenerationModeAuto);
    EXPECT_EQ(Resolved(ModeOnly(kDlssFGModeDynamic), kFrameGenerationModeDrsSettingId),
              kDrsFrameGenerationModeDynamic);

    // Default and an out-of-range value both mean "leave the key alone", and
    // the runtime logs anything else as an invalid mode, so CE never emits one.
    uint32_t unused = 0;
    EXPECT_FALSE(ResolveSubstitutedValue(ModeOnly(kDlssFGModeDefault), kFrameGenerationModeDrsSettingId, unused));
    EXPECT_FALSE(ResolveSubstitutedValue(ModeOnly(200u), kFrameGenerationModeDrsSettingId, unused));
}

TEST_F(NgxDrsOverrideTest, MultiFrameCadenceIsExpressedAsGeneratedFrames) {
    // Profile Inspector's "2x".."6x" are driver values 1..5: the number of
    // generated frames between two rendered ones.
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(2), 1u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(3), 2u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(4), 3u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(5), 4u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(6), 5u);

    // 1x is not a cadence and 7x does not exist; both mean untouched.
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(0), 0u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(1), 0u);
    EXPECT_EQ(MultiplierToDrsGeneratedFrames(7), 0u);

    DlssDrsOverrides overrides;
    overrides.fixedCountMultiplier = 4;
    overrides.dynamicMaxMultiplier = 6;
    EXPECT_EQ(Resolved(overrides, kMultiFrameCountDrsSettingId), 3u);
    EXPECT_EQ(Resolved(overrides, kDynamicMultiFrameCountMaxDrsSettingId), 5u);

    // The two cadences are independent keys; configuring one must not answer
    // the other.
    DlssDrsOverrides dynamicOnly;
    dynamicOnly.dynamicMaxMultiplier = 6;
    uint32_t unused = 0;
    EXPECT_FALSE(ResolveSubstitutedValue(dynamicOnly, kMultiFrameCountDrsSettingId, unused));
}

TEST_F(NgxDrsOverrideTest, TargetFrameRateUsesTheDriverAutoSentinelForMaxRefresh) {
    DlssDrsOverrides overrides;
    overrides.dynamicTargetFps = kDlssFGTargetFpsMaxRefresh;
    EXPECT_EQ(Resolved(overrides, kDynamicTargetFrameRateDrsSettingId), kDrsDynamicTargetFrameRateAuto);

    overrides.dynamicTargetFps = 144;
    EXPECT_EQ(Resolved(overrides, kDynamicTargetFrameRateDrsSettingId), 144u);
    overrides.dynamicTargetFps = 1;
    EXPECT_EQ(Resolved(overrides, kDynamicTargetFrameRateDrsSettingId), 1u);
    overrides.dynamicTargetFps = 1000;
    EXPECT_EQ(Resolved(overrides, kDynamicTargetFrameRateDrsSettingId), 1000u);

    // Above CE's accepted range the value is dropped rather than forwarded; the
    // runtime would log it as an invalid dynamic target frame rate.
    uint32_t unused = 0;
    overrides.dynamicTargetFps = 1001;
    EXPECT_FALSE(ResolveSubstitutedValue(overrides, kDynamicTargetFrameRateDrsSettingId, unused));
    overrides.dynamicTargetFps = kDlssFGTargetFpsDefault;
    EXPECT_FALSE(ResolveSubstitutedValue(overrides, kDynamicTargetFrameRateDrsSettingId, unused));
}

TEST_F(NgxDrsOverrideTest, SubstitutionIsLimitedToConfiguredKeysAndTheKnownStructVersion) {
    DlssDrsOverrides overrides = PresetOnly(2u);
    EXPECT_TRUE(ShouldSubstituteSetting(overrides, kRenderPresetDrsSettingId, kNvDrsSettingVer1));

    EXPECT_FALSE(ShouldSubstituteSetting(DlssDrsOverrides{}, kRenderPresetDrsSettingId, kNvDrsSettingVer1));

    // A key CE has nothing configured for passes through even while another one
    // is armed - the DLSS-G runtimes read several keys from the same loop.
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, kFrameGenerationModeDrsSettingId, kNvDrsSettingVer1));
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, kMultiFrameCountDrsSettingId, kNvDrsSettingVer1));
    // The keys CE never claims (private flags, VSync mode, menu detection).
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, 0x10E41DF6u, kNvDrsSettingVer1));
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, 0x00A879CFu, kNvDrsSettingVer1));

    // A struct version this ABI mirror does not describe must be forwarded, not
    // written into.
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, kRenderPresetDrsSettingId, 0u));
    EXPECT_FALSE(ShouldSubstituteSetting(overrides, kRenderPresetDrsSettingId, kNvDrsSettingVer1 + 0x10000u));
}

TEST_F(NgxDrsOverrideTest, SubstitutedSettingLooksLikeAnExplicitCurrentProfileValue) {
    auto setting = std::make_unique<NvDrsSetting>();
    memset(setting.get(), 0xCD, sizeof(NvDrsSetting));
    setting->version = kNvDrsSettingVer1;

    FillSubstitutedSetting(*setting, kRenderPresetDrsSettingId, 2u);

    EXPECT_EQ(setting->settingId, kRenderPresetDrsSettingId);
    EXPECT_EQ(setting->settingType, kNvDrsDwordType);
    // The readers ignore a value that does not claim to come from the current
    // profile, and treat a predefined value as "not set".
    EXPECT_EQ(setting->settingLocation, kNvDrsCurrentProfileLocation);
    EXPECT_EQ(setting->isCurrentPredefined, 0u);
    EXPECT_EQ(setting->isPredefinedValid, 0u);
    EXPECT_EQ(setting->currentValue.u32Value, 2u);
    // The caller's version stamp is left alone.
    EXPECT_EQ(setting->version, kNvDrsSettingVer1);
}

TEST_F(NgxDrsOverrideTest, NormalizationDropsEveryOutOfRangeValue) {
    DlssDrsOverrides overrides;
    overrides.renderPreset = 99u;
    overrides.frameGenerationMode = 200u;
    overrides.fixedCountMultiplier = 9;
    overrides.dynamicMaxMultiplier = 1;
    overrides.dynamicTargetFps = 5000;

    const DlssDrsOverrides normalized = Normalize(overrides);
    EXPECT_EQ(normalized.renderPreset, 0u);
    EXPECT_EQ(normalized.frameGenerationMode, kDlssFGModeDefault);
    EXPECT_EQ(normalized.fixedCountMultiplier, 0u);
    EXPECT_EQ(normalized.dynamicMaxMultiplier, 0u);
    EXPECT_EQ(normalized.dynamicTargetFps, kDlssFGTargetFpsDefault);
    EXPECT_FALSE(HasAnyOverride(overrides));
}

TEST_F(NgxDrsOverrideTest, ConfiguredOverridesRoundTripAndRejectOutOfRange) {
    SetConfiguredOverrides(DlssDrsOverrides{});
    EXPECT_EQ(GetConfiguredPreset(), 0u);
    EXPECT_FALSE(ce::ngx_drs::IsArmed());

    SetConfiguredOverrides(PresetOnly(2u));
    EXPECT_EQ(GetConfiguredPreset(), 2u);
    EXPECT_TRUE(ce::ngx_drs::IsArmed());

    SetConfiguredOverrides(PresetOnly(99u));
    EXPECT_EQ(GetConfiguredPreset(), 0u);
    EXPECT_FALSE(ce::ngx_drs::IsArmed());

    // The multi-frame keys arm the unit on their own, without a preset.
    DlssDrsOverrides dynamic;
    dynamic.frameGenerationMode = kDlssFGModeDynamic;
    dynamic.dynamicMaxMultiplier = 6;
    dynamic.dynamicTargetFps = kDlssFGTargetFpsMaxRefresh;
    SetConfiguredOverrides(dynamic);
    EXPECT_TRUE(ce::ngx_drs::IsArmed());
    EXPECT_EQ(GetConfiguredPreset(), 0u);
    EXPECT_EQ(GetConfiguredOverrides().dynamicMaxMultiplier, 6u);
    EXPECT_EQ(GetConfiguredOverrides().dynamicTargetFps, kDlssFGTargetFpsMaxRefresh);
}

TEST_F(NgxDrsOverrideTest, DynamicModeStandsTheConfiguredFixedFactorDown) {
    // The runtime cannot both choose a cadence per frame and be told an exact
    // one on every evaluation; the explicit dynamic request wins.
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(3, kDlssFGModeDynamic), 0);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(4, kDlssFGModeDynamic), 0);

    // Every other mode leaves the parameter channel exactly as it was.
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(3, kDlssFGModeDefault), 3);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(3, kDlssFGModeFixed), 3);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(3, kDlssFGModeAuto), 3);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(3, kDlssFGModeOff), 3);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(0, kDlssFGModeFixed), 0);
    EXPECT_EQ(ResolveEffectiveDLSSFGFactor(9, kDlssFGModeDefault), 0);
}

TEST_F(NgxDrsOverrideTest, DynamicHookExceptionIsScopedToDrsConsumersAndOneExport) {
    using IATHook::ShouldAllowNgxFrameGenerationPresetDynamicHook;

    EXPECT_TRUE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "nvapi_QueryInterface"));

    // Unarmed, a DLSS driver-settings consumer keeps the blanket Streamline/FG bypass.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(false, true, "nvapi_QueryInterface"));
    // Other Streamline/FG modules are never granted the exception.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, false, "nvapi_QueryInterface"));
    // And no other export is pulled through it.
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "nvapi_Direct_GetMethod"));
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, "CreateDXGIFactory2"));
    EXPECT_FALSE(ShouldAllowNgxFrameGenerationPresetDynamicHook(true, true, nullptr));
}

// `vsync_mode` cannot reach the DLSS-G runtime any other way: CE's rewrite lands
// on the real dxgi Present, below Streamline's swapchain proxy, and
// `vsyncState.cpp::shouldEnableVSync` consults the driver key before the
// application's request anyway.
TEST_F(NgxDrsOverrideTest, DriverVSyncModeIsAnsweredFromTheResolvedVsyncMode) {
    // fifo / adaptive resolve to presentInterval 1 -> the driver's Force ON.
    EXPECT_EQ(DrsVSyncModeForPresentOverride(true, false, 1), kDrsVSyncModeForceOn);
    // off resolves to interval 0 -> Force OFF.
    EXPECT_EQ(DrsVSyncModeForPresentOverride(true, false, 0), kDrsVSyncModeForceOff);
    // mailbox is not a vertical-blank contract, and default claims nothing;
    // both leave the key to the driver.
    EXPECT_EQ(DrsVSyncModeForPresentOverride(true, true, 0), 0u);
    EXPECT_EQ(DrsVSyncModeForPresentOverride(false, false, 1), 0u);

    DlssDrsOverrides overrides;
    overrides.vsyncMode = kDrsVSyncModeForceOn;
    EXPECT_TRUE(HasAnyOverride(overrides));
    EXPECT_EQ(Resolved(overrides, kVSyncModeDrsSettingId), kDrsVSyncModeForceOn);
    EXPECT_TRUE(ShouldSubstituteSetting(overrides, kVSyncModeDrsSettingId, kNvDrsSettingVer1));

    // The VSync key alone must not answer any DLSS key.
    uint32_t unused = 0;
    EXPECT_FALSE(ResolveSubstitutedValue(overrides, kFrameGenerationModeDrsSettingId, unused));
    EXPECT_FALSE(ResolveSubstitutedValue(overrides, kRenderPresetDrsSettingId, unused));
}

TEST_F(NgxDrsOverrideTest, OnlyTheTwoVSyncValuesTheRuntimeActsOnAreClaimed) {
    // shouldEnableVSync compares against exactly Force ON and Force OFF;
    // PASSIVE is what "no override" already looks like, so claiming it would be
    // a substitution that changes nothing, and anything else is not a value the
    // driver defines.
    DlssDrsOverrides passive;
    passive.vsyncMode = kDrsVSyncModePassive;
    EXPECT_EQ(Normalize(passive).vsyncMode, 0u);
    EXPECT_FALSE(HasAnyOverride(passive));

    DlssDrsOverrides bogus;
    bogus.vsyncMode = 0x12345678u;
    EXPECT_EQ(Normalize(bogus).vsyncMode, 0u);
    EXPECT_FALSE(HasAnyOverride(bogus));

    for (uint32_t value : {kDrsVSyncModeForceOn, kDrsVSyncModeForceOff}) {
        DlssDrsOverrides accepted;
        accepted.vsyncMode = value;
        EXPECT_EQ(Normalize(accepted).vsyncMode, value);
    }
}

}  // namespace
