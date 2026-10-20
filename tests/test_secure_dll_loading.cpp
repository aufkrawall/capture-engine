#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/platform/secure_dll_loading.h"

namespace {

std::string ReadSource(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

// A private directory under the temp root that the test removes again. Its DLLs are copies of a system DLL under
// names the process has never loaded.
class ProbeDirectory {
public:
    ProbeDirectory() {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "ce_secure_dll_probe";
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        path_ = root / ("run_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path_, ec);
    }
    ~ProbeDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const {
        return path_;
    }

    // Copies a system DLL to `name` inside this directory.
    std::filesystem::path CopySystemLibraryAs(const std::wstring& name) const {
        wchar_t systemDirectory[MAX_PATH]{};
        GetSystemDirectoryW(systemDirectory, MAX_PATH);
        const std::filesystem::path target = path_ / name;
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::path(systemDirectory) / L"version.dll", target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return target;
    }

private:
    std::filesystem::path path_;
};

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

TEST(ScopedDllDirectoryTest, RejectsRelativeAndMissingDirectories) {
    DWORD error = ERROR_SUCCESS;
    ce::security::ScopedDllDirectory relative(std::filesystem::path(L"ffmpeg"), &error);
    EXPECT_FALSE(relative);
    EXPECT_EQ(error, ERROR_BAD_PATHNAME);

    ProbeDirectory probe;
    ce::security::ScopedDllDirectory missing(probe.path() / L"absent", &error);
    EXPECT_FALSE(missing);
    EXPECT_EQ(error, ERROR_BAD_PATHNAME);
}

TEST(ScopedDllDirectoryTest, LoadsADependencyDirectoryForOneLoadOnly) {
    ProbeDirectory probe;
    const std::filesystem::path scopedCopy = probe.CopySystemLibraryAs(L"ce_scope_probe.dll");
    ASSERT_TRUE(std::filesystem::exists(scopedCopy));

    DWORD error = ERROR_SUCCESS;
    HMODULE module = ce::security::LoadLibraryFromScopedDirectory(scopedCopy, probe.path(), &error);
    ASSERT_NE(module, nullptr) << "error " << error;
    EXPECT_EQ(error, ERROR_SUCCESS);
    FreeLibrary(module);

    // The directory left the search with the scope: a by-name load of a copy in it does not find it.
    const std::filesystem::path unscoped = probe.CopySystemLibraryAs(L"ce_scope_probe_unscoped.dll");
    ASSERT_TRUE(std::filesystem::exists(unscoped));
    EXPECT_EQ(LoadLibraryW(L"ce_scope_probe_unscoped.dll"), nullptr);
}

TEST(SecureDllLoadingTest, PreloadLoadsOnlyTheNamedRuntimeFamilyAndKeepsItLoaded) {
    ProbeDirectory probe;
    probe.CopySystemLibraryAs(L"ce_runtime_probe-1.dll");
    probe.CopySystemLibraryAs(L"other_probe-1.dll");

    DWORD error = ERROR_SUCCESS;
    const std::vector<std::wstring> prefixes{L"ce_runtime_probe-"};
    ASSERT_TRUE(ce::security::PreloadScopedRuntimeDlls(probe.path(), prefixes, &error)) << "error " << error;
    HMODULE preloaded = GetModuleHandleW(L"ce_runtime_probe-1.dll");
    ASSERT_NE(preloaded, nullptr);
    EXPECT_EQ(GetModuleHandleW(L"other_probe-1.dll"), nullptr);

    // A later call finds the module loaded and takes no further reference on it.
    EXPECT_TRUE(ce::security::PreloadScopedRuntimeDlls(probe.path(), prefixes, &error));
    EXPECT_EQ(GetModuleHandleW(L"ce_runtime_probe-1.dll"), preloaded);

    // The preload's one reference is the only one: releasing it unloads the module.
    FreeLibrary(preloaded);
    EXPECT_EQ(GetModuleHandleW(L"ce_runtime_probe-1.dll"), nullptr);
}

TEST(SecureDllLoadingTest, PreloadOfAMissingDirectoryFails) {
    ProbeDirectory probe;
    DWORD error = ERROR_SUCCESS;
    EXPECT_FALSE(ce::security::PreloadScopedRuntimeDlls(probe.path() / L"absent", {L"avcodec-"}, &error));
    EXPECT_EQ(error, ERROR_BAD_PATHNAME);
}

TEST(SecureDllLoadingSourceTest, PrivateRuntimeLoadersNeverMutateTheLegacyDllDirectory) {
    const std::filesystem::path root = std::filesystem::current_path();
    const std::string loader = ReadSource(root / "captureengine" / "app" / "mediaengine_loader.cpp");
    const std::string screenshot = ReadSource(root / "captureengine" / "media" / "screenshot.cpp");
    const std::string workerHost = ReadSource(root / "captureengine" / "media" / "process_loopback_worker_host.cpp");

    for (const std::string* source : {&loader, &screenshot, &workerHost}) {
        ASSERT_FALSE(source->empty());
        EXPECT_EQ(source->find("SetDllDirectory"), std::string::npos);
    }
    // The helper process owns its search order; the loader and the worker host set it, the screenshot path must not.
    EXPECT_NE(loader.find("EnsureSecureDllSearchDirectory"), std::string::npos);
    EXPECT_NE(loader.find("LoadLibraryFromSecurePath"), std::string::npos);
    EXPECT_NE(loader.find("LoadLibraryFromScopedDirectory"), std::string::npos);
    EXPECT_NE(workerHost.find("EnsureSecureDllSearchDirectory"), std::string::npos);
    EXPECT_NE(workerHost.find("LoadLibraryFromSecurePath"), std::string::npos);
    EXPECT_EQ(screenshot.find("EnsureSecureDllSearchDirectory"), std::string::npos);
    EXPECT_NE(screenshot.find("PreloadScopedRuntimeDlls"), std::string::npos);
}
