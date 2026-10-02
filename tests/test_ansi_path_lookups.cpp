#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string>

#include "common/platform/ansi_path.h"
#include "hook/runtime/dll_utils.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

// The text between `begin` and the next `end` after it ("" when either is missing).
std::string Between(const std::string& source, const std::string& begin, const std::string& end) {
    const size_t start = source.find(begin);
    if (start == std::string::npos) {
        return {};
    }
    const size_t stop = source.find(end, start + begin.size());
    return stop == std::string::npos ? std::string() : source.substr(start, stop - start);
}

// A folder whose name no Western code page expresses ("Проверка"), or on a
// Cyrillic system one the Cyrillic code page cannot ("テスト").
std::filesystem::path MakeForeignFolder() {
    const std::wstring name = GetACP() == 1251 ? L"ce_ansi_\u30C6\u30B9\u30C8_" : L"ce_ansi_\u041F\u0440\u043E_";
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / (name + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    return folder;
}

}  // namespace

// The drop-in for GetModuleFileNameA must name a file the ANSI APIs can open.
TEST(AnsiPathLookupTest, ModulePathsAreDerivedInUtf16AndStayOpenable) {
    const std::wstring exe = ce::ansi_path::ModulePathW(nullptr);
    ASSERT_FALSE(exe.empty());
    char ansi[MAX_PATH] = {};
    bool exact = false;
    const DWORD length = ce::ansi_path::ModuleFileNameAnsi(nullptr, ansi, MAX_PATH, &exact);
    ASSERT_GT(length, 0u);
    EXPECT_TRUE(exact);
    EXPECT_NE(GetFileAttributesA(ansi), INVALID_FILE_ATTRIBUTES);

    const std::string directory = ce::ansi_path::ModuleDirectoryAnsi(nullptr, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(std::string(ansi).rfind(directory + "\\", 0), 0u) << "the directory is the image path's parent";
    EXPECT_EQ(ce::ansi_path::ParentDirectoryW(L"C:\\a\\b.dll"), L"C:\\a");
    EXPECT_EQ(ce::ansi_path::ParentDirectoryW(L"b.dll"), L"");

    char tooSmall[4] = {};
    EXPECT_EQ(ce::ansi_path::ModuleFileNameAnsi(nullptr, tooSmall, sizeof(tooSmall)), 0u);
    EXPECT_EQ(tooSmall[0], '\0');
}

// A DLL below a folder the code page cannot express: the version resource is
// still read through the wide path, while the '?'-mangled narrow path that
// GetModuleFileNameA used to supply names no file at all.
TEST(AnsiPathLookupTest, VersionResourcesResolveBelowAForeignFolder) {
    wchar_t systemDir[MAX_PATH] = {};
    ASSERT_GT(GetSystemDirectoryW(systemDir, MAX_PATH), 0u);
    const std::filesystem::path source = std::filesystem::path(systemDir) / L"version.dll";
    const std::filesystem::path folder = MakeForeignFolder();
    const std::filesystem::path copy = folder / L"version.dll";
    std::error_code error;
    std::filesystem::copy_file(source, copy, std::filesystem::copy_options::overwrite_existing, error);
    ASSERT_FALSE(error) << error.message();

    uint32_t major = 0;
    EXPECT_TRUE(DllFileVersionPartsW(copy.c_str(), &major, nullptr, nullptr));
    EXPECT_GE(major, 6u);
    EXPECT_TRUE(DllVersionStringContainsW(copy.c_str(), "windows"));

    // What GetModuleFileNameA returned for it.
    std::string mangled;
    const std::wstring wide = copy.wstring();
    const int needed = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    ASSERT_GT(needed, 1);
    mangled.assign(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, mangled.data(), needed, nullptr, nullptr);
    EXPECT_FALSE(DllVersionStringContains(mangled.c_str(), "windows")) << "the old narrow lookup's failure mode";

    // The ANSI-compatible form opens it when the volume keeps 8.3 names.
    bool exact = false;
    const std::string compatible = ce::ansi_path::CompatiblePath(wide, &exact);
    if (exact) {
        EXPECT_NE(GetFileAttributesA(compatible.c_str()), INVALID_FILE_ATTRIBUTES);
        EXPECT_EQ(DllFileMajorVersion(compatible.c_str()), major);
    } else {
        EXPECT_EQ(GetFileAttributesA(compatible.c_str()), INVALID_FILE_ATTRIBUTES) << "inexact is never silently wrong";
    }
    std::filesystem::remove_all(folder, error);
}

TEST(AnsiPathLookupTest, LoadedModuleProbesUseTheModuleHandle) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);
    EXPECT_GE(ModuleFileMajorVersion(kernel32), 6u);
    EXPECT_TRUE(ModuleVersionStringContains(kernel32, "WINDOWS"));
    EXPECT_FALSE(ModuleVersionStringContains(kernel32, "dxvk"));
    EXPECT_TRUE(IsModuleInSystem32(kernel32));
    EXPECT_FALSE(IsModuleInSystem32(GetModuleHandleW(nullptr)));
    EXPECT_FALSE(IsDllFromProject("kernel32.dll", "windows")) << "a System32 module is never a replacement";
    EXPECT_EQ(ModuleFileMajorVersion(nullptr), ModuleFileMajorVersion(GetModuleHandleW(nullptr)));
}

// A loaded module's version comes out of its own mapping, and it must be the
// same answer the file gives: the file read goes through
// LoadLibraryEx(AS_DATAFILE), the export Steam's overlay serializes behind a
// lock, and CE's hook thread holding that lock while the game's in-process
// MiniDumpWriteDump suspended everything froze Talos Reawakened for 50 s
// (logs/20260926_094906).
TEST(AnsiPathLookupTest, LoadedModuleVersionIsReadFromTheMappingNotTheFile) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);
    const std::string mapped = ModuleReadVersionResource(kernel32);
    ASSERT_FALSE(mapped.empty());

    // The mapping carries the image's real version. The file read of kernel32
    // from this unmanifested test executable returned 6.2 - the compatibility
    // version lie - so the two are compared by strings, not by number.
    uint32_t mappedParts[3] = {};
    ASSERT_TRUE(VersionResourceFileVersionParts(mapped, &mappedParts[0], &mappedParts[1], &mappedParts[2]));
    EXPECT_GE(mappedParts[0], 10u);
    EXPECT_EQ(ModuleFileMajorVersion(kernel32), mappedParts[0]);
    EXPECT_TRUE(VersionResourceStringContains(mapped, "windows"));
    EXPECT_EQ(VersionResourceStringContains(mapped, "windows"),
              DllVersionStringContainsW(DllModulePathW(kernel32).c_str(), "windows"));
    EXPECT_FALSE(VersionResourceStringContains(mapped, "dxvk"));

    EXPECT_TRUE(ModuleReadVersionResource(nullptr).empty());
    EXPECT_FALSE(VersionResourceStringContains(std::string(), "windows"));
    EXPECT_FALSE(VersionResourceFileVersionParts(std::string(), &mappedParts[0], nullptr, nullptr));

    // The HMODULE forms never derive a path to re-open.
    const std::string dllUtils = ReadSource("hook/runtime/dll_utils.h");
    for (const char* form : {"static inline bool ModuleVersionStringContains(HMODULE module",
                             "static inline bool ModuleFileVersionParts(HMODULE module"}) {
        const std::string body = Between(dllUtils, form, "\n}\n");
        ASSERT_FALSE(body.empty()) << form;
        EXPECT_NE(body.find("ModuleReadVersionResource(module)"), std::string::npos) << form;
        EXPECT_EQ(body.find("DllModulePathW("), std::string::npos) << form;
        EXPECT_EQ(body.find("DllReadVersionResourceW("), std::string::npos) << form;
    }
    const std::string reader = Between(dllUtils, "static inline std::string ModuleReadVersionResource(", "\n}\n");
    ASSERT_FALSE(reader.empty());
    EXPECT_EQ(reader.find("GetFileVersionInfo"), std::string::npos);
    EXPECT_EQ(reader.find("LoadLibrary"), std::string::npos);
}

// Source policy: the hook-side lookups that open, probe or derive a path from
// the module/game folder stay off GetModuleFileNameA. Base-name attribution in
// log lines is deliberately left narrow (the file name itself is ASCII).
TEST(AnsiPathLookupTest, PathDerivingLookupsStayOffTheAnsiModuleName) {
    const std::string hookCommon = ReadSource("hook/runtime/hook_common.cpp");
    const std::string logsFallback =
        Between(hookCommon, "bool GetSessionLogsDirectory(", "int written = snprintf(outDir");
    ASSERT_FALSE(logsFallback.empty());
    EXPECT_EQ(logsFallback.find("GetModuleFileNameA("), std::string::npos);
    EXPECT_NE(logsFallback.find("ModuleDirectoryAnsi"), std::string::npos);

    const std::string layerMain = ReadSource("hook/vulkan_layer/layer_main.cpp");
    const std::string layerDir = Between(layerMain, "static std::string GetLayerDllDirectory()", "return \".\";");
    ASSERT_FALSE(layerDir.empty());
    EXPECT_EQ(layerDir.find("GetModuleFileNameA("), std::string::npos);

    const std::string layerIpc = ReadSource("hook/vulkan_layer/layer_ipc.cpp");
    EXPECT_NE(layerIpc.find("ce::ansi_path::ModuleDirectoryAnsi(nullptr)"), std::string::npos);
    EXPECT_EQ(layerIpc.find("GetModuleFileNameA("), std::string::npos);

    const std::string dred = ReadSource("hook/d3d12/dx12_dred.cpp");
    EXPECT_EQ(dred.find("GetModuleFileNameA("), std::string::npos);
    EXPECT_EQ(dred.find("CreateFileA("), std::string::npos);
    const std::string trace = Between(ReadSource("hook/d3d12/dx12_hook_helpers.cpp"), "bool Dx12TraceEnabled()",
                                      "return s_enabled;");
    ASSERT_FALSE(trace.empty());
    EXPECT_EQ(trace.find("GetModuleFileNameA("), std::string::npos);

    const std::string redirect = Between(ReadSource("hook/runtime/main_redirect.cpp"), "bool StreamlineShipsWithApplication()",
                                         "return shipped;");
    ASSERT_FALSE(redirect.empty());
    EXPECT_EQ(redirect.find("GetModuleFileNameA("), std::string::npos);
    EXPECT_NE(redirect.find("GetFileAttributesW"), std::string::npos);

    const std::string fatal = Between(ReadSource("hook/runtime/main_fatal_dump.cpp"),
                                      "std::filesystem::path GetInstalledCaptureEnginePath()", "\n}\n");
    ASSERT_FALSE(fatal.empty());
    EXPECT_EQ(fatal.find("GetModuleFileNameA("), std::string::npos);

    const std::string dllUtils = ReadSource("hook/runtime/dll_utils.h");
    EXPECT_EQ(dllUtils.find("GetModuleFileNameA("), std::string::npos);
    EXPECT_EQ(dllUtils.find("GetFileVersionInfoA("), std::string::npos);
    // The layer used to carry a private narrow copy of the same helpers.
    const std::string layerHeader = ReadSource("hook/vulkan_layer/layer_main.h");
    EXPECT_EQ(layerHeader.find("GetFileVersionInfoSizeA("), std::string::npos);
    EXPECT_NE(layerHeader.find("#include \"hook/runtime/dll_utils.h\""), std::string::npos);

    const std::string generation = ReadSource("hook/streamline/streamline_api_generation.h");
    EXPECT_EQ(generation.find("GetModuleFileNameA("), std::string::npos);
    EXPECT_NE(generation.find("ModuleFileMajorVersion(interposer)"), std::string::npos);
    for (const char* file : {"hook/runtime/main_overlay_detect.cpp", "hook/runtime/main_thirdparty_load.cpp"}) {
        const std::string source = ReadSource(file);
        ASSERT_FALSE(source.empty()) << file;
        EXPECT_EQ(source.find("DllVersionStringContains(path"), std::string::npos) << file;
        EXPECT_NE(source.find("ModuleVersionStringContains(retained"), std::string::npos) << file;
    }
}
