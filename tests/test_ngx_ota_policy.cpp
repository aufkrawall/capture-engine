#include <gtest/gtest.h>

#include <string>

#include "../common/config.h"
#include "../hook/common/ngx_ota_policy.h"

namespace {

using ce::ngx_ota::DisableUpdaterEnvironmentValue;
using ce::ngx_ota::ImageBaseName;
using ce::ngx_ota::IsNgxUpdaterImage;
using ce::ngx_ota::ModeName;
using ce::ngx_ota::ShouldClearStreamlineOtaPreferences;
using ce::ngx_ota::ShouldRefuseUpdaterLaunch;
using ce::ngx_ota::ShouldSuppressConfiguredRuntimeOverrides;
using ce::ngx_ota::WritesEnvironment;

TEST(NgxOtaPolicy, ParsesTheThreeDocumentedModes) {
    EXPECT_EQ(ParseNgxOtaMode("default"), kNgxOtaModeDefault);
    EXPECT_EQ(ParseNgxOtaMode(""), kNgxOtaModeDefault);
    EXPECT_EQ(ParseNgxOtaMode("off"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("OFF"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("on"), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode(" on "), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode("\"on\""), kNgxOtaModeOn);
    EXPECT_EQ(ParseNgxOtaMode("0"), kNgxOtaModeOff);
    EXPECT_EQ(ParseNgxOtaMode("1"), kNgxOtaModeOn);
}

// A typo must not silently force an NGX policy in either direction: both
// forced modes change what the driver does, so only an explicit value may
// select one.
TEST(NgxOtaPolicy, UnrecognizedValuesFallBackToDefaultRatherThanAForcedMode) {
    for (const char* value : {"yes", "enable", "disabled", "auto", "2", "of", "onn"}) {
        EXPECT_EQ(ParseNgxOtaMode(value), kNgxOtaModeDefault) << value;
    }
}

TEST(NgxOtaPolicy, ParsesNgxLogLevels) {
    EXPECT_EQ(ParseNgxLogLevel("default"), kNgxLogLevelDefault);
    EXPECT_EQ(ParseNgxLogLevel(""), kNgxLogLevelDefault);
    EXPECT_EQ(ParseNgxLogLevel("off"), kNgxLogLevelOff);
    EXPECT_EQ(ParseNgxLogLevel("on"), kNgxLogLevelOn);
    EXPECT_EQ(ParseNgxLogLevel("verbose"), kNgxLogLevelVerbose);
    EXPECT_EQ(ParseNgxLogLevel("nonsense"), kNgxLogLevelDefault);
}

// `default` means "do not write the variable at all", which has no environment
// representation - writing "0" would be an explicit off, not an absence.
TEST(NgxOtaPolicy, LogLevelEnvironmentValueMapsOntoNgxOwnScale) {
    EXPECT_EQ(NgxLogLevelEnvironmentValue(kNgxLogLevelDefault), nullptr);
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelOff), "0");
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelOn), "1");
    EXPECT_STREQ(NgxLogLevelEnvironmentValue(kNgxLogLevelVerbose), "2");
}

TEST(NgxOtaPolicy, ImageBaseNameHandlesBothSeparatorsAndALeadingQuote) {
    EXPECT_STREQ(ImageBaseName("C:\\Windows\\System32\\nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName("C:/drivers/nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName("\"C:\\a b\\nvngx_update.exe\" -bootstrap"), "nvngx_update.exe\" -bootstrap");
    EXPECT_STREQ(ImageBaseName("nvngx_update.exe"), "nvngx_update.exe");
    EXPECT_STREQ(ImageBaseName(nullptr), "");
}

// `_nvngx.dll` may pass the updater as lpApplicationName or only inside
// lpCommandLine, so both spellings have to resolve identically.
TEST(NgxOtaPolicy, RecognizesTheUpdaterAsPathOrCommandLine) {
    EXPECT_TRUE(IsNgxUpdaterImage("nvngx_update.exe"));
    EXPECT_TRUE(IsNgxUpdaterImage("NVNGX_UPDATE.EXE"));
    EXPECT_TRUE(IsNgxUpdaterImage(
        "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_b20cc8aeaed64fc2\\nvngx_update.exe"));
    EXPECT_TRUE(IsNgxUpdaterImage("\"C:\\drivers\\nvngx_update.exe\" -bootstrap -api 1"));
    EXPECT_TRUE(IsNgxUpdaterImage("C:\\drivers\\nvngx_update.exe -forced_update"));
}

// The refusal must never reach a different NVIDIA binary, least of all one whose
// name merely starts or ends the same way.
TEST(NgxOtaPolicy, DoesNotMatchAnyNeighbouringImage) {
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx_update_helper.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx_updater.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("my_nvngx_update.exe.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvngx.dll"));
    EXPECT_FALSE(IsNgxUpdaterImage("nvcontainer.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage("update.exe"));
    EXPECT_FALSE(IsNgxUpdaterImage(""));
    EXPECT_FALSE(IsNgxUpdaterImage(nullptr));
}

TEST(NgxOtaPolicy, OnlyOffRefusesTheUpdaterLaunch) {
    EXPECT_TRUE(ShouldRefuseUpdaterLaunch(kNgxOtaModeOff, /*targetIsUpdater=*/true));
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeDefault, true));
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeOn, true));
    // No mode may ever refuse a process that is not the updater.
    for (uint8_t mode : {kNgxOtaModeDefault, kNgxOtaModeOff, kNgxOtaModeOn}) {
        EXPECT_FALSE(ShouldRefuseUpdaterLaunch(mode, /*targetIsUpdater=*/false)) << ModeName(mode);
    }
}

// `off` publishes the suppression, `on` clears an inherited one, and `default`
// leaves the environment exactly as the process received it.
TEST(NgxOtaPolicy, EnvironmentValueDistinguishesSuppressClearAndLeaveAlone) {
    EXPECT_STREQ(DisableUpdaterEnvironmentValue(kNgxOtaModeOff), "1");
    EXPECT_STREQ(DisableUpdaterEnvironmentValue(kNgxOtaModeOn), "");
    EXPECT_EQ(DisableUpdaterEnvironmentValue(kNgxOtaModeDefault), nullptr);

    EXPECT_TRUE(WritesEnvironment(kNgxOtaModeOff));
    EXPECT_TRUE(WritesEnvironment(kNgxOtaModeOn));
    EXPECT_FALSE(WritesEnvironment(kNgxOtaModeDefault));
}

// The two forced modes act on opposite sides and must never overlap: `on` stands
// CE's own DLL overrides down so the driver's OTA files load, while `off` clears
// Streamline's OTA preference bits so they do not.
TEST(NgxOtaPolicy, ForcedModesActOnOppositeMechanismsAndNeverBoth) {
    EXPECT_TRUE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeOn));
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeOff));
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeDefault));

    EXPECT_TRUE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeOff));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeOn));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeDefault));

    for (uint8_t mode : {kNgxOtaModeDefault, kNgxOtaModeOff, kNgxOtaModeOn}) {
        EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(mode) && ShouldClearStreamlineOtaPreferences(mode))
            << ModeName(mode);
    }
}

// `default` is the shipped behaviour and must remain completely inert: no
// refusal, no environment write, no override suppression, no preference edit.
TEST(NgxOtaPolicy, DefaultModeChangesNothing) {
    EXPECT_FALSE(ShouldRefuseUpdaterLaunch(kNgxOtaModeDefault, true));
    EXPECT_EQ(DisableUpdaterEnvironmentValue(kNgxOtaModeDefault), nullptr);
    EXPECT_FALSE(ShouldSuppressConfiguredRuntimeOverrides(kNgxOtaModeDefault));
    EXPECT_FALSE(ShouldClearStreamlineOtaPreferences(kNgxOtaModeDefault));
    EXPECT_FALSE(WritesEnvironment(kNgxOtaModeDefault));
}

TEST(NgxOtaPolicy, ModeAndRefusalTablesCoverEveryDefinedValue) {
    EXPECT_TRUE(IsNgxOtaMode(kNgxOtaModeDefault));
    EXPECT_TRUE(IsNgxOtaMode(kNgxOtaModeOn));
    EXPECT_FALSE(IsNgxOtaMode(static_cast<uint8_t>(kNgxOtaModeOn + 1)));
    EXPECT_TRUE(IsNgxLogLevel(kNgxLogLevelVerbose));
    EXPECT_FALSE(IsNgxLogLevel(static_cast<uint8_t>(kNgxLogLevelVerbose + 1)));

    EXPECT_STREQ(ModeName(kNgxOtaModeDefault), "default");
    EXPECT_STREQ(ModeName(kNgxOtaModeOff), "off");
    EXPECT_STREQ(ModeName(kNgxOtaModeOn), "on");
}

// Every refusal reason the hook can publish must have user-facing text, or the
// tray balloon and the host warning would surface an empty explanation.
TEST(RuntimeOverrideRefusal, EveryReasonExceptNoneHasExplanatoryText) {
    EXPECT_STREQ(RuntimeOverrideRefusalText(kRuntimeOverrideRefusalNone), "");
    for (uint32_t reason : {kRuntimeOverrideRefusalForeignStreamlineCore, kRuntimeOverrideRefusalGenerationMismatch,
                            kRuntimeOverrideRefusalDuplicateModule, kRuntimeOverrideRefusalNgxOtaForcedOn}) {
        const char* text = RuntimeOverrideRefusalText(reason);
        ASSERT_NE(text, nullptr) << reason;
        EXPECT_GT(std::string(text).size(), 20u) << reason;
    }
    // An unknown reason must read as "nothing to report" rather than fabricate
    // an explanation.
    EXPECT_STREQ(RuntimeOverrideRefusalText(9999u), "");
}

}  // namespace
