#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "common/platform/process_identity.h"

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    std::ifstream stream(std::filesystem::current_path() / relativePath, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(ProcessIdentityTest, ResolvesCurrentExecutableThroughLimitedQueryInterface) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(GetCurrentProcessId());
    ASSERT_TRUE(identity) << identity.error;
    EXPECT_NE(identity.imageName.find("unit_tests"), std::string::npos);
    EXPECT_FALSE(identity.imagePath.empty());
    EXPECT_EQ(identity.imageName, std::filesystem::path(identity.imagePath).filename().string());
    EXPECT_EQ(identity.error, ERROR_SUCCESS);

    const ce::process::ProcessIdentityResult fallback =
        ce::process::QueryProcessIdentityBySystemProcessId(GetCurrentProcessId());
    ASSERT_TRUE(fallback) << fallback.error;
    EXPECT_EQ(fallback.imageName, identity.imageName);
    EXPECT_EQ(fallback.imagePath, identity.imagePath);
    EXPECT_EQ(fallback.error, ERROR_SUCCESS);
}

TEST(ProcessIdentityTest, NtDevicePathTranslationRespectsVolumeBoundariesAndMupShares) {
    const std::vector<std::pair<std::wstring, std::wstring>> mappings = {
        {L"C:", LR"(\Device\HarddiskVolume1)"},
        {L"D:", LR"(\Device\HarddiskVolume10)"},
    };
    EXPECT_EQ(ce::process::TranslateNtDevicePathToWin32(LR"(\Device\HarddiskVolume10\Modded\game.exe)", mappings),
              LR"(D:\Modded\game.exe)");
    EXPECT_EQ(ce::process::TranslateNtDevicePathToWin32(LR"(\device\harddiskvolume1\Vanilla\game.exe)", mappings),
              LR"(C:\Vanilla\game.exe)");
    EXPECT_EQ(ce::process::TranslateNtDevicePathToWin32(LR"(\Device\Mup\server\share\Modded\game.exe)", mappings),
              LR"(\\server\share\Modded\game.exe)");
    EXPECT_EQ(ce::process::TranslateNtDevicePathToWin32(LR"(\Device\HarddiskVolume99\game.exe)", mappings),
              LR"(\Device\HarddiskVolume99\game.exe)");
}

TEST(ProcessIdentityTest, FailureCannotReturnAPartiallyOverwrittenSentinel) {
    const ce::process::ProcessIdentityResult identity = ce::process::QueryProcessIdentity(UINT32_MAX);
    EXPECT_FALSE(identity);
    EXPECT_TRUE(identity.imageName.empty());
    EXPECT_TRUE(identity.imagePath.empty());
    EXPECT_NE(identity.error, ERROR_SUCCESS);

    const ce::process::ProcessIdentityResult fallback = ce::process::QueryProcessIdentityBySystemProcessId(UINT32_MAX);
    EXPECT_FALSE(fallback);
    EXPECT_TRUE(fallback.imageName.empty());
    EXPECT_TRUE(fallback.imagePath.empty());
    EXPECT_NE(fallback.error, ERROR_SUCCESS);
}

TEST(ProcessIdentityTest, ProductionIdentityQueryDoesNotRequestTargetMemoryAccess) {
    const std::string source = ReadSource("common/platform/process_identity.cpp");
    const std::string inject = ReadSource("captureengine/injection/inject_main.cpp");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(inject.empty());
    EXPECT_NE(source.find("PROCESS_QUERY_LIMITED_INFORMATION"), std::string::npos);
    EXPECT_NE(source.find("QueryFullProcessImageNameW"), std::string::npos);
    EXPECT_NE(source.find("ce::utf8::FromWide"), std::string::npos);
    EXPECT_EQ(source.find("PROCESS_VM_READ"), std::string::npos);
    EXPECT_EQ(inject.find("GetModuleBaseNameA"), std::string::npos);
    EXPECT_EQ(inject.find("PROCESS_VM_READ"), std::string::npos);
}
