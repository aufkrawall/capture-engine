#include <gtest/gtest.h>
#include <windows.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "../common/config.h"

namespace {

static std::string TempPath(const char* suffix) {
    char buf[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, buf);
    EXPECT_GT(len, 0u);
    EXPECT_LT(len, MAX_PATH);
    std::string path(buf);
    path += "ce_config_fuzz_";
    path += suffix;
    return path;
}

static void WriteFuzzFile(const std::string& path, const std::vector<char>& data) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    if (!data.empty())
        WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
}

TEST(ConfigFuzzTest, RandomByteSequencesDoNotCrash) {
    const std::string path = TempPath("random.ini");
    constexpr int kIterations = 100;
    for (int i = 0; i < kIterations; ++i) {
        std::vector<char> buf(256 + (i * 37) % 512);
        for (size_t j = 0; j < buf.size(); ++j)
            buf[j] = static_cast<char>((i * 13 + j * 7) % 256);
        WriteFuzzFile(path, buf);

        AppConfig config;
        EXPECT_NO_THROW(LoadConfig(path, config)) << "Failed on iteration " << i;
    }
    DeleteFileA(path.c_str());
}

TEST(ConfigFuzzTest, EmptyAndMinimalFilesDoNotCrash) {
    const std::string emptyPath = TempPath("empty.ini");
    WriteFuzzFile(emptyPath, {});
    AppConfig config;
    EXPECT_NO_THROW(LoadConfig(emptyPath, config));
    DeleteFileA(emptyPath.c_str());

    const std::string newlinePath = TempPath("newline.ini");
    WriteFuzzFile(newlinePath, {'\n'});
    EXPECT_NO_THROW(LoadConfig(newlinePath, config));
    DeleteFileA(newlinePath.c_str());
}

TEST(ConfigFuzzTest, LargeBogusInputDoesNotCrash) {
    const std::string path = TempPath("large.ini");
    std::vector<char> big(10000 + 1);
    for (size_t j = 0; j < big.size(); ++j)
        big[j] = static_cast<char>((j * 31 + 7) % 256);
    big[0] = '[';
    big[1] = 'X';
    big[2] = ']';
    big[big.size() - 1] = '\n';
    WriteFuzzFile(path, big);

    AppConfig config;
    EXPECT_NO_THROW(LoadConfig(path, config));
    DeleteFileA(path.c_str());
}

TEST(ConfigFuzzTest, RepeatedSectionKeysDoNotCrash) {
    const std::string path = TempPath("repeated.ini");
    std::string content;
    for (int i = 0; i < 50; ++i) {
        content += "[Section]\nkey" + std::to_string(i) + "=value" + std::to_string(i) + "\n";
    }
    WriteFuzzFile(path, std::vector<char>(content.begin(), content.end()));

    AppConfig config;
    EXPECT_NO_THROW(LoadConfig(path, config));
    DeleteFileA(path.c_str());
}

}  // namespace
