#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "hook/ngx/ngx_drs_override.h"
#include "hook/pacing/reflex_defs.h"
#include "hook/hooking/iat_hook.h"
#include "source_fragment_reader.h"

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

// A copy of a harmless system DLL mapped from a private directory under a chosen file name. It
// stands in for a Streamline core the title loaded before CE's loader notification existed
// (Witcher 3 Remastered, 20261008_211749): already in the process, never offered to a hook.
class MappedModuleCopy {
public:
    explicit MappedModuleCopy(const char* fileName) {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("ce_drs_sweep_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()) +
                "_" + fileName);
        std::error_code error;
        fs::create_directories(dir_, error);
        char systemDir[MAX_PATH] = {};
        GetSystemDirectoryA(systemDir, MAX_PATH);
        path_ = dir_ / fileName;
        fs::copy_file(fs::path(systemDir) / "msimg32.dll", path_, fs::copy_options::overwrite_existing, error);
        if (!error)
            module_ = LoadLibraryW(path_.c_str());
    }

    ~MappedModuleCopy() {
        if (module_)
            FreeLibrary(module_);
        std::error_code error;
        std::filesystem::remove_all(dir_, error);
    }

    MappedModuleCopy(const MappedModuleCopy&) = delete;
    MappedModuleCopy& operator=(const MappedModuleCopy&) = delete;

    HMODULE module() const { return module_; }

    // Drops the test's own reference and reports whether the image is gone, which is only true
    // when whoever else touched the module released every reference it took.
    bool UnloadAndReportGone() {
        if (!module_)
            return true;
        FreeLibrary(module_);
        module_ = nullptr;
        return GetModuleHandleW(path_.c_str()) == nullptr;
    }

private:
    std::filesystem::path dir_;
    std::filesystem::path path_;
    HMODULE module_ = nullptr;
};

void CollectVisitedModule(void* module, const char* /*modulePath*/, void* context) {
    static_cast<std::vector<void*>*>(context)->push_back(module);
}

TEST(NgxDrsStartupSweepTest, VisitsAStreamlineCoreThatWasMappedBeforeAnyHookCouldSeeIt) {
    MappedModuleCopy core("sl.common.dll");
    MappedModuleCopy bystander("not_a_streamline_module.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();
    ASSERT_NE(bystander.module(), nullptr) << "GetLastError=" << GetLastError();

    std::vector<void*> visited;
    const uint32_t count = ce::ngx_drs::ForEachLoadedDlssDrsConsumer(&CollectVisitedModule, &visited);

    EXPECT_EQ(count, visited.size());
    EXPECT_NE(std::find(visited.begin(), visited.end(), static_cast<void*>(core.module())), visited.end());
    // Only consumers are offered the GetProcAddress import patch; every other module in the
    // process keeps the narrower sweep it already had.
    EXPECT_EQ(std::find(visited.begin(), visited.end(), static_cast<void*>(bystander.module())), visited.end());
}

TEST(NgxDrsStartupSweepTest, MatchesTheLoaderNotificationsPredicateForEveryNamedConsumer) {
    MappedModuleCopy dlssg("nvngx_dlssg.dll");
    MappedModuleCopy interposer("sl.interposer.dll");
    ASSERT_NE(dlssg.module(), nullptr) << "GetLastError=" << GetLastError();
    ASSERT_NE(interposer.module(), nullptr) << "GetLastError=" << GetLastError();

    std::vector<void*> visited;
    ce::ngx_drs::ForEachLoadedDlssDrsConsumer(&CollectVisitedModule, &visited);

    EXPECT_NE(std::find(visited.begin(), visited.end(), static_cast<void*>(dlssg.module())), visited.end());
    EXPECT_NE(std::find(visited.begin(), visited.end(), static_cast<void*>(interposer.module())), visited.end());
}

TEST(NgxDrsStartupSweepTest, DoesNotLeakAReferenceOnTheModulesItVisits) {
    MappedModuleCopy core("sl.common.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();

    std::vector<void*> visited;
    ce::ngx_drs::ForEachLoadedDlssDrsConsumer(&CollectVisitedModule, &visited);
    ASSERT_FALSE(visited.empty());

    // A pin that is never released would keep a Streamline core the title unloads (DLSS-G off)
    // mapped for the rest of the process.
    EXPECT_TRUE(core.UnloadAndReportGone());
}

TEST(NgxDrsStartupSweepTest, WithoutAVisitorThereIsNothingToDo) {
    EXPECT_EQ(ce::ngx_drs::ForEachLoadedDlssDrsConsumer(nullptr, nullptr), 0u);
}

struct ImageSection {
    uint8_t* begin = nullptr;
    size_t size = 0;
};

// First section of `module` that is (or is not) writable, never executable.
ImageSection FindSection(HMODULE module, bool writable) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const bool isWritable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        const bool isExecutable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        const size_t size = section->Misc.VirtualSize;
        if (isWritable == writable && !isExecutable && size >= 64)
            return {base + section->VirtualAddress, size};
    }
    return {};
}

// Plants `value` in the last slots of the module's data section and puts the old contents back.
class PlantedSlots {
public:
    PlantedSlots(HMODULE module, void* value, size_t count) {
        const ImageSection data = FindSection(module, /*writable=*/true);
        if (!data.begin)
            return;
        const uintptr_t end = reinterpret_cast<uintptr_t>(data.begin) + data.size;
        const uintptr_t first = (end - count * sizeof(void*)) & ~(sizeof(void*) - 1);
        for (size_t i = 0; i < count; ++i) {
            auto** slot = reinterpret_cast<void**>(first) + i;
            slots_.push_back(slot);
            saved_.push_back(*slot);
            *slot = value;
        }
    }
    ~PlantedSlots() {
        for (size_t i = 0; i < slots_.size(); ++i)
            *slots_[i] = saved_[i];
    }
    bool ok() const { return !slots_.empty(); }
    void** slot(size_t i) const { return slots_[i]; }

private:
    std::vector<void**> slots_;
    std::vector<void*> saved_;
};

TEST(NgxDrsCachedPointerRetargetTest, ReplacesCachedCopiesInWritableDataAndPutsThemBack) {
    MappedModuleCopy core("sl.common.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();
    // Distinct, never dereferenced addresses standing in for the driver export and CE's detour.
    void* const driverExport = reinterpret_cast<void*>(0x00007FF6DEAD0010ull);
    void* const detour = reinterpret_cast<void*>(0x00007FF6BEEF0020ull);

    PlantedSlots planted(core.module(), driverExport, 2);
    ASSERT_TRUE(planted.ok());

    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), driverExport, detour), 2u);
    EXPECT_EQ(*planted.slot(0), detour);
    EXPECT_EQ(*planted.slot(1), detour);

    // Idempotent: nothing equals the driver export any more.
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), driverExport, detour), 0u);

    ce::ngx_drs::RestoreRetargetedPointers();
    EXPECT_EQ(*planted.slot(0), driverExport);
    EXPECT_EQ(*planted.slot(1), driverExport);
}

TEST(NgxDrsCachedPointerRetargetTest, RestoreLeavesASlotAnotherPartyHasChangedSince) {
    MappedModuleCopy core("sl.common.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();
    void* const driverExport = reinterpret_cast<void*>(0x00007FF6DEAD0030ull);
    void* const detour = reinterpret_cast<void*>(0x00007FF6BEEF0040ull);
    void* const foreign = reinterpret_cast<void*>(0x00007FF6CAFE0050ull);

    PlantedSlots planted(core.module(), driverExport, 1);
    ASSERT_TRUE(planted.ok());
    ASSERT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), driverExport, detour), 1u);

    *planted.slot(0) = foreign;
    ce::ngx_drs::RestoreRetargetedPointers();
    EXPECT_EQ(*planted.slot(0), foreign);
}

TEST(NgxDrsCachedPointerRetargetTest, OnlyWritableNonExecutableDataIsScanned) {
    MappedModuleCopy core("sl.common.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();
    void* const detour = reinterpret_cast<void*>(0x00007FF6BEEF0060ull);

    // A value that really occurs in the module's read-only data (its first non-zero, non-trivial
    // qword) must be left alone, even though it equals the search value.
    const ImageSection readOnly = FindSection(core.module(), /*writable=*/false);
    ASSERT_NE(readOnly.begin, nullptr);
    void* readOnlyValue = nullptr;
    void** readOnlySlot = nullptr;
    for (size_t offset = 0; offset + sizeof(void*) <= readOnly.size; offset += sizeof(void*)) {
        auto** slot = reinterpret_cast<void**>(readOnly.begin + offset);
        const auto bits = reinterpret_cast<uintptr_t>(*slot);
        if (bits > 0xFFFFFFFFull && bits < 0x00007FFFFFFFFFFFull) {
            readOnlyValue = *slot;
            readOnlySlot = slot;
            break;
        }
    }
    if (!readOnlySlot)
        GTEST_SKIP() << "no pointer-like qword in the donor module's read-only data";

    // The writable data must not hold the same value, or the expectation below would be wrong.
    const ImageSection data = FindSection(core.module(), /*writable=*/true);
    ASSERT_NE(data.begin, nullptr);
    for (size_t offset = 0; offset + sizeof(void*) <= data.size; offset += sizeof(void*)) {
        if (*reinterpret_cast<void**>(data.begin + offset) == readOnlyValue)
            GTEST_SKIP() << "donor module holds the probe value in writable data too";
    }

    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), readOnlyValue, detour), 0u);
    EXPECT_EQ(*readOnlySlot, readOnlyValue);
}

TEST(NgxDrsCachedPointerRetargetTest, DegenerateRequestsChangeNothing) {
    MappedModuleCopy core("sl.common.dll");
    ASSERT_NE(core.module(), nullptr) << "GetLastError=" << GetLastError();
    void* const value = reinterpret_cast<void*>(0x00007FF6DEAD0070ull);
    PlantedSlots planted(core.module(), value, 1);
    ASSERT_TRUE(planted.ok());

    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(nullptr, value, value), 0u);
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), nullptr, value), 0u);
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), value, nullptr), 0u);
    EXPECT_EQ(ce::ngx_drs::RetargetCachedPointers(core.module(), value, value), 0u);
    EXPECT_EQ(*planted.slot(0), value);

    // Without a usable detour the NvAPI form must not touch the module either.
    const ce::ngx_drs::CachedNvApiRetarget none = ce::ngx_drs::RetargetCachedNvApiPointers(core.module(), nullptr);
    EXPECT_EQ(none.queryInterface, 0u);
    EXPECT_EQ(none.drsGetSetting, 0u);
}

// The sweep is only worth anything if the two startup paths that can reach already-mapped
// modules actually call it, and the loader notification (which runs under the loader lock and
// must not pin modules) does not.
TEST(NgxDrsStartupSweepTest, IsWiredIntoTheHookThreadAndKeptOutOfTheLoaderNotification) {
    namespace fs = std::filesystem;
    const std::string hookThread =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "runtime" / "main_hookthread.cpp");
    const std::string overlayDetect =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "runtime" / "main_overlay_detect.cpp");
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(overlayDetect.empty());

    // After the process-wide GetProcAddress router exists, which skips Streamline modules.
    const size_t router = hookThread.find("IATHook::InitializeGetProcAddressHook();");
    const size_t sweep = hookThread.find("PatchLoadedDlssDrsConsumers(\"hook thread router\");");
    ASSERT_NE(router, std::string::npos);
    ASSERT_NE(sweep, std::string::npos);
    EXPECT_LT(router, sweep);

    // Arming from a config source sweeps by default; the loader notification opts out.
    EXPECT_NE(overlayDetect.find("if (sweepLoadedModules) {\n    PatchLoadedDlssDrsConsumers(source);"),
              std::string::npos);
    EXPECT_EQ(overlayDetect.find("ArmNgxDrsOverridesIfConfigured(baseName);"), std::string::npos);
    EXPECT_NE(overlayDetect.find("ArmNgxDrsOverridesIfConfigured(baseName, false);"), std::string::npos);

    // Every swept consumer also gets its cached NvAPI pointers retargeted, and the redirected data
    // slots are undone together with the import patches.
    EXPECT_NE(overlayDetect.find("ce::ngx_drs::RetargetCachedNvApiPointers(module, state->queryInterfaceDetour)"),
              std::string::npos);
    EXPECT_NE(overlayDetect.find("g_ReflexLimiter.QueryInterfaceDetourIfReady()"), std::string::npos);
    const std::string iatInit =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "hooking" / "iat_hook_init.cpp");
    ASSERT_FALSE(iatInit.empty());
    EXPECT_NE(iatInit.find("ce::ngx_drs::RestoreRetargetedPointers();"), std::string::npos);
}

}  // namespace
