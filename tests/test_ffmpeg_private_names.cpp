#include <gtest/gtest.h>

#include "common/platform/ffmpeg_runtime_names.h"

#include <windows.h>
#include <psapi.h>

#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

// CaptureEngine's FFmpeg carries private DLL names (libcengine 11, part 4): a client process that runs the engine may
// already have loaded its own avcodec-63.dll, and Windows binds an import to an already loaded DLL of the same base
// name. These tests run against the FFmpeg this test binary links, which is the product's.
namespace {

// Every loaded module whose file name starts with `prefix` (case-insensitive).
std::vector<std::pair<HMODULE, std::wstring>> LoadedModulesStartingWith(const std::wstring& prefix) {
    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    std::vector<std::pair<HMODULE, std::wstring>> found;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
                               static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed))
        return found;
    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));
    for (HMODULE module : modules) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module, path, MAX_PATH))
            continue;
        const std::wstring name = std::filesystem::path(path).filename().wstring();
        if (_wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0)
            found.emplace_back(module, name);
    }
    return found;
}

TEST(FfmpegPrivateNamesTest, TheLinkedFfmpegHasThePrivateName) {
    EXPECT_GT(avcodec_version(), 0u);  // makes sure the import is resolved
    const auto avcodec = LoadedModulesStartingWith(L"avcodec");
    ASSERT_FALSE(avcodec.empty());
    for (const auto& [module, name] : avcodec)
        EXPECT_EQ(name.rfind(ce::ffmpeg_runtime::kAvcodecPrefix, 0), 0u) << std::filesystem::path(name).string();
}

// A client's own FFmpeg under the standard name, loaded first: CaptureEngine's calls still reach its own copy.
TEST(FfmpegPrivateNamesTest, AClientsFfmpegUnderTheStandardNameDoesNotTakeOver) {
    wchar_t system[MAX_PATH]{};
    ASSERT_GT(GetSystemDirectoryW(system, MAX_PATH), 0u);
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / (L"ffmpeg_decoy_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    // Any valid DLL serves as the decoy; what matters is its base name.
    const std::filesystem::path decoy = directory / L"avcodec-63.dll";
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", decoy,
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    HMODULE loaded = LoadLibraryExW(decoy.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(GetModuleHandleW(L"avcodec-63.dll"), loaded);
    EXPECT_EQ(avcodec_version(), static_cast<unsigned>(LIBAVCODEC_VERSION_INT));
    for (const auto& [module, name] : LoadedModulesStartingWith(ce::ffmpeg_runtime::kAvcodecPrefix))
        EXPECT_NE(module, loaded) << "CaptureEngine's FFmpeg is a module of its own";
    FreeLibrary(loaded);
    std::filesystem::remove_all(directory, ec);
}

}  // namespace
