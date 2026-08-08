#include <gtest/gtest.h>

#include <windows.h>

#include <cstdio>
#include <string>

#include "../common/config.h"
#include "../hook/common/dlss_indicator_spoof.h"

namespace {

using ce::dlss_indicator::Answer;
using ce::dlss_indicator::kIndicatorOn;
using ce::dlss_indicator::kValueName;
using ce::dlss_indicator::Mode;
using ce::dlss_indicator::ParseMode;
using ce::dlss_indicator::ResolveGetValue;
using ce::dlss_indicator::ResolveQueryValue;

TEST(DlssIndicatorSpoof, ParseModeAcceptsConfigSpellings) {
    EXPECT_EQ(ParseMode("on"), Mode::kForceOn);
    EXPECT_EQ(ParseMode("ON"), Mode::kForceOn);
    EXPECT_EQ(ParseMode(" On "), Mode::kForceOn);
    EXPECT_EQ(ParseMode("1"), Mode::kForceOn);
    EXPECT_EQ(ParseMode("true"), Mode::kForceOn);

    EXPECT_EQ(ParseMode("off"), Mode::kForceOff);
    EXPECT_EQ(ParseMode("OFF"), Mode::kForceOff);
    EXPECT_EQ(ParseMode("0"), Mode::kForceOff);
    EXPECT_EQ(ParseMode("false"), Mode::kForceOff);
}

TEST(DlssIndicatorSpoof, ParseModeLeavesUnsetAndTyposAlone) {
    EXPECT_EQ(ParseMode(""), Mode::kPassthrough);
    EXPECT_EQ(ParseMode("default"), Mode::kPassthrough);
    EXPECT_EQ(ParseMode("onn"), Mode::kPassthrough);
    EXPECT_EQ(ParseMode("enable-please"), Mode::kPassthrough);
}

TEST(DlssIndicatorSpoof, PassthroughModeNeverAnswers) {
    const Answer query = ResolveQueryValue(Mode::kPassthrough, kValueName, true, sizeof(DWORD));
    EXPECT_FALSE(query.handled);

    const Answer get = ResolveGetValue(Mode::kPassthrough, kValueName, RRF_RT_REG_DWORD, true, sizeof(DWORD));
    EXPECT_FALSE(get.handled);
}

TEST(DlssIndicatorSpoof, UnrelatedValueNamesAreForwarded) {
    EXPECT_FALSE(ResolveQueryValue(Mode::kForceOn, L"FullPath", true, sizeof(DWORD)).handled);
    EXPECT_FALSE(ResolveQueryValue(Mode::kForceOn, L"ShowDlssIndicatorExtra", true, sizeof(DWORD)).handled);
    EXPECT_FALSE(ResolveQueryValue(Mode::kForceOn, nullptr, true, sizeof(DWORD)).handled);
    EXPECT_FALSE(ResolveGetValue(Mode::kForceOff, L"Installed", RRF_RT_REG_DWORD, true, sizeof(DWORD)).handled);
}

TEST(DlssIndicatorSpoof, ValueNameMatchIsCaseInsensitive) {
    const Answer answer = ResolveQueryValue(Mode::kForceOn, L"showdlssindicator", true, sizeof(DWORD));
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.value, kIndicatorOn);
}

// The regression this whole unit exists for: the machine has no
// ShowDlssIndicator value, so the answer must be synthesized complete - type,
// payload and size - rather than only writing into the caller's buffer.
TEST(DlssIndicatorSpoof, ForceOnReportsDwordTypeAndSizeWithPayload) {
    const Answer answer = ResolveQueryValue(Mode::kForceOn, kValueName, true, sizeof(DWORD));
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, ERROR_SUCCESS);
    EXPECT_EQ(answer.type, static_cast<DWORD>(REG_DWORD));
    EXPECT_EQ(answer.value, kIndicatorOn);
    EXPECT_EQ(answer.requiredBytes, sizeof(DWORD));
    EXPECT_TRUE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, ForceOffReportsZeroPayload) {
    const Answer answer = ResolveQueryValue(Mode::kForceOff, kValueName, true, sizeof(DWORD));
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, ERROR_SUCCESS);
    EXPECT_EQ(answer.value, 0u);
    EXPECT_TRUE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, SizeProbeSucceedsWithoutWritingData) {
    const Answer answer = ResolveQueryValue(Mode::kForceOn, kValueName, false, 0);
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, ERROR_SUCCESS);
    EXPECT_EQ(answer.type, static_cast<DWORD>(REG_DWORD));
    EXPECT_EQ(answer.requiredBytes, sizeof(DWORD));
    EXPECT_FALSE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, UndersizedBufferAsksForMoreDataWithoutWriting) {
    const Answer answer = ResolveQueryValue(Mode::kForceOn, kValueName, true, sizeof(DWORD) - 1);
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, static_cast<LSTATUS>(ERROR_MORE_DATA));
    EXPECT_EQ(answer.requiredBytes, sizeof(DWORD));
    EXPECT_FALSE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, OversizedBufferStillReportsFourBytes) {
    const Answer answer = ResolveQueryValue(Mode::kForceOn, kValueName, true, 64);
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, ERROR_SUCCESS);
    EXPECT_EQ(answer.requiredBytes, sizeof(DWORD));
    EXPECT_TRUE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, GetValueHonorsDwordTypeRestrictions) {
    for (DWORD flags : {static_cast<DWORD>(RRF_RT_REG_DWORD), static_cast<DWORD>(RRF_RT_DWORD),
                        static_cast<DWORD>(RRF_RT_ANY), static_cast<DWORD>(0)}) {
        const Answer answer = ResolveGetValue(Mode::kForceOn, kValueName, flags, true, sizeof(DWORD));
        ASSERT_TRUE(answer.handled) << "flags=" << flags;
        EXPECT_EQ(answer.status, ERROR_SUCCESS) << "flags=" << flags;
        EXPECT_EQ(answer.value, kIndicatorOn) << "flags=" << flags;
    }
}

TEST(DlssIndicatorSpoof, GetValueRejectsIncompatibleTypeRestriction) {
    const Answer answer = ResolveGetValue(Mode::kForceOn, kValueName, RRF_RT_REG_SZ, true, sizeof(DWORD));
    ASSERT_TRUE(answer.handled);
    EXPECT_EQ(answer.status, static_cast<LSTATUS>(ERROR_UNSUPPORTED_TYPE));
    EXPECT_FALSE(answer.writeValue);
}

TEST(DlssIndicatorSpoof, GetValueSizeProbeAndUndersizedBufferBehaveLikeQueryValue) {
    const Answer probe = ResolveGetValue(Mode::kForceOn, kValueName, RRF_RT_REG_DWORD, false, 0);
    ASSERT_TRUE(probe.handled);
    EXPECT_EQ(probe.status, ERROR_SUCCESS);
    EXPECT_EQ(probe.requiredBytes, sizeof(DWORD));
    EXPECT_FALSE(probe.writeValue);

    const Answer small = ResolveGetValue(Mode::kForceOn, kValueName, RRF_RT_REG_DWORD, true, 2);
    ASSERT_TRUE(small.handled);
    EXPECT_EQ(small.status, static_cast<LSTATUS>(ERROR_MORE_DATA));
    EXPECT_FALSE(small.writeValue);
}

// End-to-end over the config layer: the setting is normally written into a
// per-process [Profile.*] section, so pin that the profile value is what the
// hook ends up parsing.
class DlssIndicatorConfigTest : public ::testing::Test {
protected:
    std::string configPath;

    void SetUp() override {
        char temp[MAX_PATH] = {};
        const DWORD length = GetTempPathA(MAX_PATH, temp);
        ASSERT_GT(length, 0u);
        configPath = std::string(temp) + "ce_dlss_indicator_" + std::to_string(GetCurrentProcessId()) + ".ini";
        remove(configPath.c_str());
    }

    void TearDown() override {
        remove(configPath.c_str());
    }

    void WriteConfig(const std::string& content) {
        HANDLE file =
            CreateFileA(configPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        DWORD written = 0;
        ASSERT_TRUE(WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
        CloseHandle(file);
        ASSERT_EQ(written, content.size());
    }
};

TEST_F(DlssIndicatorConfigTest, ProfileSectionOverridesGlobalSetting) {
    WriteConfig(
        "[DLSS]\n"
        "dlss_debug_overlay=off\n"
        "\n"
        "[Profile.gta]\n"
        "process=GTA5_Enhanced.exe\n"
        "dlss_debug_overlay=on\n");

    AppConfig matched;
    LoadConfig(configPath, matched, "GTA5_Enhanced.exe");
    EXPECT_EQ(matched.graphics.dlssDebugOverlay, "on");
    EXPECT_EQ(ParseMode(matched.graphics.dlssDebugOverlay), Mode::kForceOn);

    AppConfig other;
    LoadConfig(configPath, other, "SomeOtherGame.exe");
    EXPECT_EQ(other.graphics.dlssDebugOverlay, "off");
    EXPECT_EQ(ParseMode(other.graphics.dlssDebugOverlay), Mode::kForceOff);
}

TEST_F(DlssIndicatorConfigTest, LegacyGraphicsSectionStillWorksAndDefaultsToPassthrough) {
    WriteConfig("[Graphics]\ndlss_debug_overlay=on\n");
    AppConfig legacy;
    LoadConfig(configPath, legacy, "AnyGame.exe");
    EXPECT_EQ(ParseMode(legacy.graphics.dlssDebugOverlay), Mode::kForceOn);

    WriteConfig("[DLSS]\ndlss_sharpening=default\n");
    AppConfig unset;
    LoadConfig(configPath, unset, "AnyGame.exe");
    EXPECT_EQ(unset.graphics.dlssDebugOverlay, "default");
    EXPECT_EQ(ParseMode(unset.graphics.dlssDebugOverlay), Mode::kPassthrough);
}

}  // namespace
