#pragma once

// Shared includes and helpers for the test_config suite, which is split
// across several .cpp files to stay under the AGENTS.md size ceiling.

#include <gtest/gtest.h>
#include <windows.h>
#include "../common/config.h"
#include "../hook/common/nvngx_parameter_abi.h"

namespace {
std::string MakeTestPath(const char* filename) {
    char buffer[MAX_PATH] = {};
    DWORD length = GetFullPathNameA(filename, MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return filename;
    }
    return buffer;
}

std::string DefaultTemplatePath() {
    char modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    EXPECT_GT(length, 0u);
    EXPECT_LT(length, static_cast<DWORD>(MAX_PATH));
    std::string path(modulePath, length);
    const size_t testsSeparator = path.find_last_of("\\/");
    EXPECT_NE(testsSeparator, std::string::npos);
    if (testsSeparator == std::string::npos)
        return {};
    path.resize(testsSeparator);
    const size_t rootSeparator = path.find_last_of("\\/");
    EXPECT_NE(rootSeparator, std::string::npos);
    if (rootSeparator == std::string::npos)
        return {};
    path.resize(rootSeparator);
    return path + "\\captureengine\\config.ini.template";
}

void WriteTextFile(const std::string& path, const std::string& content) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr));
    CloseHandle(file);
    ASSERT_EQ(written, content.size());
}

std::string ReadTextFile(const std::string& path) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(file, INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size = {};
    EXPECT_TRUE(GetFileSizeEx(file, &size));
    std::string content(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!content.empty()) {
        EXPECT_TRUE(ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr));
        content.resize(read);
    }
    CloseHandle(file);
    return content;
}
}  // namespace

class ConfigTest : public ::testing::Test {
protected:
    // Use absolute path because GetPrivateProfileString requires it or checks
    // C:\Windows
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = MakeTestPath("test_config.ini");

        // Clean up before test
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        // Clean up after test
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        WriteTextFile(tempConfigFile, content);
    }
};






















































class WhitelistEntryTest : public ::testing::Test {
protected:
    std::string tempConfigFile;

    void SetUp() override {
        tempConfigFile = MakeTestPath("test_whitelist_entry.ini");
        remove(tempConfigFile.c_str());
    }

    void TearDown() override {
        remove(tempConfigFile.c_str());
    }

    void WriteConfig(const std::string& content) {
        WriteTextFile(tempConfigFile, content);
    }
};


















