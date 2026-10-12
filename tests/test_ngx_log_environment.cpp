#include <gtest/gtest.h>
#include <windows.h>
#include <array>
#include <optional>
#include <string>
#include "hook/ngx/ngx_ota_runtime.h"
#include "hook/ngx/ngx_ota_policy.h"
#include "common/platform/owned_environment_value.h"
#include "source_fragment_reader.h"

namespace {
constexpr std::array keys{L"__NGX_LOG_LEVEL", L"__NGX_LOG_PATH_OVERRIDE", L"__NGX_ENABLE_OVERRIDE_LOG_PATH"};
std::optional<std::wstring> ReadVariable(const wchar_t* key) {
    const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return std::nullopt;
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(key, value.data(), size));
    return value;
}
class NgxLogEnvironmentTest : public testing::Test {
protected:
    void SetUp() override {
        ce::ngx_ota::RestoreLogEnvironment();
        for (size_t i = 0; i < keys.size(); ++i) {
            original_[i] = ReadVariable(keys[i]);
            ASSERT_TRUE(SetEnvironmentVariableW(keys[i], L"fixture-original"));
        }
    }
    void TearDown() override {
        ce::ngx_ota::RestoreLogEnvironment();
        for (size_t i = 0; i < keys.size(); ++i)
            EXPECT_TRUE(SetEnvironmentVariableW(keys[i], original_[i] ? original_[i]->c_str() : nullptr));
    }
    std::array<std::optional<std::wstring>, 3> original_;
};
}

TEST_F(NgxLogEnvironmentTest, OffAndDefaultRestoreOwnedPathsAndDeactivationPreservesApplicationWrites) {
    ce::ngx_ota::ApplyLogEnvironment(kNgxLogLevelOn, "fixture-session");
    EXPECT_EQ(ReadVariable(keys[0]), L"1");
    EXPECT_EQ(ReadVariable(keys[1]), L"fixture-session");
    EXPECT_EQ(ReadVariable(keys[2]), L"1");
    ce::ngx_ota::ApplyLogEnvironment(kNgxLogLevelOff, nullptr);
    EXPECT_EQ(ReadVariable(keys[0]), L"0");
    EXPECT_EQ(ReadVariable(keys[1]), L"fixture-original");
    ce::ngx_ota::ApplyLogEnvironment(kNgxLogLevelDefault, nullptr);
    EXPECT_EQ(ReadVariable(keys[0]), L"fixture-original");
    ce::ngx_ota::ApplyLogEnvironment(kNgxLogLevelOn, "next-session");
    ASSERT_TRUE(SetEnvironmentVariableW(keys[1], L"application-owned"));
    ce::ngx_ota::RestoreLogEnvironment();
    EXPECT_EQ(ReadVariable(keys[1]), L"application-owned");
    EXPECT_EQ(ReadVariable(keys[0]), L"fixture-original");
    EXPECT_FALSE(ce::ngx_ota::IsLogEnvironmentApplied());
}

TEST(OwnedEnvironmentValueTest, AbsenceWriteFailureAndExternalReplacementRemainDistinct) {
    ce::OwnedEnvironmentValue<std::string> owner;
    std::optional<std::string> current;
    const auto read = [&] { return current; };
    const auto write = [&](const auto& value) { current = value; return true; };
    EXPECT_FALSE(owner.Apply("first", read, [](const auto&) { return false; }));
    EXPECT_FALSE(current);
    ASSERT_TRUE(owner.Apply("first", read, write));
    ASSERT_TRUE(owner.Restore(read, write));
    EXPECT_FALSE(current);
    ASSERT_TRUE(owner.Apply("first", read, write));
    current = "external";
    ASSERT_TRUE(owner.Apply("second", read, write));
    ASSERT_TRUE(owner.Restore(read, write));
    EXPECT_EQ(current, "external");
}

TEST(NgxLogPolicySourceTest, CreatorRolesSkipLogPolicyAndDormancyRestoresEnvironment) {
    const auto hook = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "main_hookthread.cpp"));
    const auto guard = hook.find("if (main_g_ProcessCategory != ProcessCategory::Launcher && main_g_ProcessCategory != ProcessCategory::Blacklisted)");
    const auto publish = hook.find("ce::ngx_ota::PublishPolicy");
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(guard, publish);
    const auto lifecycle = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "main_host_lifecycle.cpp"));
    EXPECT_NE(lifecycle.find("ce::ngx_ota::RestoreLogEnvironment()"), std::string::npos);
    const auto runtime = ce::test_source::ReadFile(ce::test_source::FindSource("hook", "ngx_ota_runtime.cpp"));
    EXPECT_NE(runtime.find("NGX OTA: PEB lock unavailable"), std::string::npos);
}
