#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../common/secure_dll_loading.h"

namespace {

std::string ReadSource(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

}  // namespace

TEST(SecureDllLoadingTest, RejectsRelativePrivateLibraryPaths) {
    DWORD error = ERROR_SUCCESS;
    EXPECT_EQ(ce::security::LoadLibraryFromSecurePath(L"mediaengine.dll", &error), nullptr);
    EXPECT_EQ(error, ERROR_BAD_PATHNAME);
}

TEST(SecureDllLoadingTest, LoadsNamedSystemLibrariesOnlyFromSystem32) {
    DWORD error = ERROR_SUCCESS;
    HMODULE module = ce::security::LoadSystemLibrary(L"kernel32.dll", &error);
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(error, ERROR_SUCCESS);
    FreeLibrary(module);

    EXPECT_EQ(ce::security::LoadSystemLibrary(L"..\\kernel32.dll", &error), nullptr);
    EXPECT_EQ(error, ERROR_INVALID_NAME);
}

TEST(SecureDllLoadingSourceTest, PrivateRuntimeLoadersNeverMutateTheLegacyDllDirectory) {
    const std::filesystem::path root = std::filesystem::current_path();
    const std::string loader = ReadSource(root / "captureengine" / "mediaengine_loader.cpp");
    const std::string screenshot = ReadSource(root / "captureengine" / "screenshot.cpp");
    const std::string workerHost = ReadSource(root / "captureengine" / "process_loopback_worker_host.cpp");

    for (const std::string* source : {&loader, &screenshot, &workerHost}) {
        ASSERT_FALSE(source->empty());
        EXPECT_EQ(source->find("SetDllDirectory"), std::string::npos);
        EXPECT_NE(source->find("EnsureSecureDllSearchDirectory"), std::string::npos);
    }
    EXPECT_NE(loader.find("LoadLibraryFromSecurePath"), std::string::npos);
    EXPECT_NE(workerHost.find("LoadLibraryFromSecurePath"), std::string::npos);
    EXPECT_NE(workerHost.find("mediaengine.dll"), std::string::npos);
}
