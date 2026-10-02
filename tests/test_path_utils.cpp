#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>

#include "../common/path_utils.h"

TEST(PathUtilsTest, DetectsDriveAbsolutePaths) {
    EXPECT_TRUE(ce::path::IsDriveAbsolutePath(std::filesystem::path(L"Z:\\Captures")));
    EXPECT_TRUE(ce::path::IsDriveAbsolutePath(std::filesystem::path(L"z:/Captures")));
    EXPECT_FALSE(ce::path::IsDriveAbsolutePath(std::filesystem::path(L"Z:Captures")));
    EXPECT_FALSE(ce::path::IsDriveAbsolutePath(std::filesystem::path(L"\\\\server\\share\\Captures")));
    EXPECT_FALSE(ce::path::IsDriveAbsolutePath(std::filesystem::path(L"Captures")));
}

TEST(PathUtilsTest, ReplacesMappedDriveRootWithUncRoot) {
    const std::filesystem::path resolved = ce::path::ReplaceDriveRootWithRemotePath(
        std::filesystem::path(L"Z:\\Captures\\Game\\file.mkv"), L"\\\\nas\\recordings");

    EXPECT_EQ(resolved.wstring(), L"\\\\nas\\recordings\\Captures\\Game\\file.mkv");
}

TEST(PathUtilsTest, ReplacesMappedDriveRootWithoutDuplicatingSlashes) {
    const std::filesystem::path resolved =
        ce::path::ReplaceDriveRootWithRemotePath(std::filesystem::path(L"Z:/Captures"), L"\\\\nas\\recordings\\");

    EXPECT_EQ(resolved.wstring(), L"\\\\nas\\recordings\\Captures");
}

TEST(PathUtilsTest, ExpandsEnvironmentReferences) {
    ASSERT_TRUE(SetEnvironmentVariableW(L"CE_PATH_UTILS_TEST_ROOT", L"C:\\Users\\Someone"));
    EXPECT_EQ(ce::path::ExpandEnvironmentReferences(L"%CE_PATH_UTILS_TEST_ROOT%\\Videos\\Capture Engine"),
              L"C:\\Users\\Someone\\Videos\\Capture Engine");
    SetEnvironmentVariableW(L"CE_PATH_UTILS_TEST_ROOT", nullptr);
}

TEST(PathUtilsTest, EnvironmentExpansionLeavesPlainAndUndefinedTextAlone) {
    EXPECT_EQ(ce::path::ExpandEnvironmentReferences(L"D:\\Captures"), L"D:\\Captures");
    EXPECT_EQ(ce::path::ExpandEnvironmentReferences(L""), L"");
    SetEnvironmentVariableW(L"CE_PATH_UTILS_TEST_UNDEFINED", nullptr);
    EXPECT_EQ(ce::path::ExpandEnvironmentReferences(L"%CE_PATH_UTILS_TEST_UNDEFINED%\\x"),
              L"%CE_PATH_UTILS_TEST_UNDEFINED%\\x");
}

TEST(PathUtilsTest, LeavesNonDriveAbsolutePathsUnchanged) {
    const std::filesystem::path unc(L"\\\\nas\\recordings\\Captures");
    EXPECT_EQ(ce::path::ReplaceDriveRootWithRemotePath(unc, L"\\\\other\\share"), unc);

    const std::filesystem::path relative(L"Captures\\Game");
    EXPECT_EQ(ce::path::ReplaceDriveRootWithRemotePath(relative, L"\\\\nas\\recordings"), relative);
}
