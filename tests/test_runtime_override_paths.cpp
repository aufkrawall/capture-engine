#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "hook/overrides/published_graphics_config.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/runtime_override_paths.h"

namespace {

namespace path = ce::runtime_override_path;
using ce::published_config::CodePagePathToUtf8;

// config.ini values are active-code-page text, the loader redirect is UTF-8.
// U+00FC is 0xFC in cp1252 and 0xC3 0xBC in UTF-8; the redirect used to decode
// the cp1252 byte as UTF-8 and loaded a file that does not exist.
TEST(RuntimeOverridePaths, CodePageConfigTextBecomesUtf8) {
    char utf8[ce::published_config::RuntimeOverridePaths::kCapacity] = {};
    ASSERT_TRUE(CodePagePathToUtf8("C:\\Users\\J\xFCrgen\\sl", 1252, utf8, sizeof(utf8)));
    EXPECT_STREQ(utf8, "C:\\Users\\J\xC3\xBCrgen\\sl");
    EXPECT_EQ(path::CodePageToUtf8("C:\\Users\\J\xFCrgen\\sl", 1252), "C:\\Users\\J\xC3\xBCrgen\\sl");

    ASSERT_TRUE(CodePagePathToUtf8("", 1252, utf8, sizeof(utf8)));
    EXPECT_STREQ(utf8, "");
    char tiny[4] = {};
    EXPECT_FALSE(CodePagePathToUtf8("C:\\long\\path", 1252, tiny, sizeof(tiny)));
    EXPECT_STREQ(tiny, "");
}

// Invalid UTF-8 never decodes to U+FFFD: two different malformed paths must not
// name (or compare as) the same file.
TEST(RuntimeOverridePaths, MalformedUtf8NamesNoFile) {
    EXPECT_TRUE(path::Utf8ToWide("C:\\Users\\J\xFCrgen\\sl").empty());
    EXPECT_FALSE(path::Exists("C:\\Users\\J\xFCrgen\\sl"));
    EXPECT_EQ(path::Utf8ToWide("C:\\a\xC3\xBC"), std::wstring(L"C:\\a\u00FC"));
}

class RuntimeOverridePathFileTest : public testing::Test {
protected:
    void SetUp() override {
        // U+00FC plus a CJK character: outside cp1252, so no ANSI API can name it.
        dir_ = std::filesystem::temp_directory_path() /
               (L"ce_override_\u00FC\u65E5_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::create_directories(dir_, error);
        ASSERT_FALSE(error) << error.message();
        wchar_t system[MAX_PATH] = {};
        ASSERT_GT(GetSystemDirectoryW(system, MAX_PATH), 0u);
        std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", dir_ / L"sl.interposer.dll",
                                   std::filesystem::copy_options::overwrite_existing, error);
        ASSERT_FALSE(error) << error.message();
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(dir_, error);
    }
    std::filesystem::path dir_;
};

// The redirect's existence check, version probe and load name the same file
// for a folder outside the code page.
TEST_F(RuntimeOverridePathFileTest, NonAsciiOverrideFolderResolvesOneFile) {
    const std::string directory = path::WideToUtf8(dir_.wstring());
    const std::string interposer = BuildOverridePath(directory, "sl.interposer.dll");
    EXPECT_TRUE(path::Exists(interposer));
    EXPECT_FALSE(path::Exists(BuildOverridePath(directory, "sl.common.dll")));
    EXPECT_GE(DllFileMajorVersionW(path::Utf8ToWide(interposer).c_str(), DllVersionReadPolicy::DirectPeOnly), 6u);

    // Relative spellings canonicalize to the loader's form.
    const std::string dotted = directory + "\\sub\\..\\.\\";
    EXPECT_EQ(CanonicalOverridePath(dotted, "sl.interposer.dll"), path::Canonical(interposer));

    // What the old code did: the UTF-8 bytes through an ANSI API name nothing.
    if (GetACP() != CP_UTF8) {
        EXPECT_EQ(GetFileAttributesA(interposer.c_str()), INVALID_FILE_ATTRIBUTES);
    }

    HMODULE module = LoadLibraryExW(path::Utf8ToWide(interposer).c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    ASSERT_NE(module, nullptr);
    FreeLibrary(module);
}

TEST(RuntimeOverridePaths, ModulePathIsUtf8AndUncapped) {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    ASSERT_NE(kernel32, nullptr);
    const std::string utf8 = path::ModulePath(kernel32);
    ASSERT_FALSE(utf8.empty());
    EXPECT_TRUE(path::Exists(utf8));
    EXPECT_TRUE(ce::graphics_runtime::EqualsIgnoreCase(ce::graphics_runtime::ModuleFileName(utf8.c_str()),
                                                       "kernel32.dll"));
    EXPECT_TRUE(path::ModulePath(nullptr).empty());
}

// NGX maps its OTA / NVIDIA-app-overridden DLSS snippets from the model
// repository under hashed .bin names (layout observed on this machine,
// 2026-10-14: models\dlss|dlssg|dlssd\versions\20318464\files\160_E658703.bin),
// so only the folder says which snippet it is.
TEST(RuntimeOverridePaths, DlssSnippetsAreRecognizedByNameAndModelFolder) {
    using ce::graphics_runtime::ResolveDlssSnippetName;
    bool repository = true;
    EXPECT_STREQ(ResolveDlssSnippetName("D:\\Game\\NVNGX_DLSS.dll", &repository), "nvngx_dlss.dll");
    EXPECT_FALSE(repository);
    EXPECT_STREQ(ResolveDlssSnippetName("C:\\npi\\nvngx_dlssg.dll"), "nvngx_dlssg.dll");
    EXPECT_STREQ(ResolveDlssSnippetName("C:\\npi\\nvngx_dlssd.dll"), "nvngx_dlssd.dll");
    const char* ota = "C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss\\versions\\20318464\\files\\160_E658703.bin";
    EXPECT_STREQ(ResolveDlssSnippetName(ota, &repository), "nvngx_dlss.dll");
    EXPECT_TRUE(repository);
    EXPECT_STREQ(ResolveDlssSnippetName("C:/ProgramData/NVIDIA/NGX/models/DLSSG/versions/1/files/x.bin"),
                 "nvngx_dlssg.dll");
    EXPECT_STREQ(ResolveDlssSnippetName("C:\\ProgramData\\NVIDIA\\NGX\\models\\dlssd\\versions\\1\\files\\x.bin"),
                 "nvngx_dlssd.dll");
    // Neither the Streamline model folders, the override marker folder nor the core are snippets.
    EXPECT_EQ(ResolveDlssSnippetName("C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_dlss_0\\versions\\1\\files\\x.dll"),
              nullptr);
    EXPECT_EQ(ResolveDlssSnippetName("C:\\ProgramData\\NVIDIA\\NGX\\models\\dlss_override\\versions\\1\\files\\x"),
              nullptr);
    EXPECT_EQ(ResolveDlssSnippetName("C:\\Windows\\System32\\DriverStore\\_nvngx.dll"), nullptr);
    EXPECT_EQ(ResolveDlssSnippetName(nullptr), nullptr);

    using ce::graphics_runtime::ClassifyDlssOverrideEffect;
    using ce::graphics_runtime::DlssOverrideEffect;
    EXPECT_EQ(ClassifyDlssOverrideEffect("C:/npi/NVNGX_DLSS.dll", false, "C:\\npi\\nvngx_dlss.dll"),
              DlssOverrideEffect::InEffect);
    EXPECT_EQ(ClassifyDlssOverrideEffect(ota, true, "C:\\npi\\nvngx_dlss.dll"), DlssOverrideEffect::ModelRepository);
    EXPECT_EQ(ClassifyDlssOverrideEffect("D:\\Game\\nvngx_dlss.dll", false, "C:\\npi\\nvngx_dlss.dll"),
              DlssOverrideEffect::OtherCopy);
}

// Loader-lock readers hold the snapshot without a lock: a republication must
// never mutate a published snapshot, and an unchanged one is not republished.
TEST(RuntimeOverridePaths, PublishedSnapshotsAreImmutable) {
    ce::published_config::PublishLocalRuntimeOverridePaths("C:\\sr", "", "C:\\fg", "C:\\sl", "test first");
    const auto& first = ce::published_config::CurrentRuntimeOverridePaths();
    EXPECT_STREQ(first.dlssSr, "C:\\sr");
    EXPECT_STREQ(first.dlssRr, "");
    EXPECT_STREQ(first.dlssFg, "C:\\fg");
    EXPECT_STREQ(first.streamline, "C:\\sl");

    ce::published_config::PublishLocalRuntimeOverridePaths("C:\\sr", "", "C:\\fg", "C:\\sl", "test same");
    EXPECT_EQ(&ce::published_config::CurrentRuntimeOverridePaths(), &first);

    ce::published_config::PublishLocalRuntimeOverridePaths("C:\\sr2", "", "", "", "test changed");
    const auto& second = ce::published_config::CurrentRuntimeOverridePaths();
    EXPECT_NE(&second, &first);
    EXPECT_STREQ(second.dlssSr, "C:\\sr2");
    EXPECT_STREQ(second.streamline, "");
    EXPECT_STREQ(first.dlssSr, "C:\\sr");
    EXPECT_STREQ(first.streamline, "C:\\sl");

    ce::published_config::PublishLocalRuntimeOverridePaths("", "", "", "", "test reset");
}

}  // namespace
