#include <gtest/gtest.h>
#include <windows.h>
#include <winver.h>

#include <array>
#include <filesystem>
#include <fstream>

namespace {
unsigned versionApiCalls = 0;
DWORD WINAPI CountVersionApi(LPCWSTR, LPDWORD) { ++versionApiCalls; return 0; }
}  // namespace

// The helper is TU-local. Count loader-dependent fallback calls without installing a process-wide hook.
#define GetFileVersionInfoSizeW CountVersionApi
#include "hook/runtime/dll_utils.h"
#undef GetFileVersionInfoSizeW
#include "source_fragment_reader.h"

namespace {
class DllVersionReadPolicyTest : public testing::Test {
protected:
    void SetUp() override {
        versionApiCalls = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("ce_versionless_" + std::to_string(GetCurrentProcessId()) + ".dll");
        alignas(IMAGE_NT_HEADERS) std::array<char, 512> image{};
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 128;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image.data() + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        std::ofstream file(path_, std::ios::binary);
        file.write(image.data(), image.size());
        ASSERT_TRUE(file.good());
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove(path_, error);
        EXPECT_FALSE(error);
    }
    std::filesystem::path path_;
};

TEST_F(DllVersionReadPolicyTest, DirectPeOnlyRejectsVersionlessAndMissingFilesWithoutLoaderFallback) {
    EXPECT_TRUE(DllReadVersionResourceW(path_.c_str(), DllVersionReadPolicy::DirectPeOnly).empty());
    EXPECT_EQ(DllFileMajorVersion(path_.string().c_str(), DllVersionReadPolicy::DirectPeOnly), 0u);
    EXPECT_TRUE(DllReadVersionResourceW((path_.wstring() + L".missing").c_str(),
                                      DllVersionReadPolicy::DirectPeOnly).empty());
    EXPECT_EQ(versionApiCalls, 0u);
}

TEST_F(DllVersionReadPolicyTest, OrdinaryFileQueriesRetainTheirFallback) {
    EXPECT_TRUE(DllReadVersionResourceW(path_.c_str()).empty());
    EXPECT_EQ(versionApiCalls, 1u);
}

TEST_F(DllVersionReadPolicyTest, DirectPeOnlyReadsAnActualVersionResource) {
    const auto path = DllModulePathW(GetModuleHandleW(L"kernel32.dll"));
    ASSERT_FALSE(path.empty());
    uint32_t major = 0;
    EXPECT_TRUE(DllFileVersionPartsW(path.c_str(), &major, nullptr, nullptr, DllVersionReadPolicy::DirectPeOnly));
    EXPECT_GT(major, 0u);
    EXPECT_EQ(versionApiCalls, 0u);
}

TEST_F(DllVersionReadPolicyTest, EarlyTakeoverRequiresTheStrictReadPolicy) {
    const auto source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "hook/streamline/streamline_interposer_takeover.cpp");
    EXPECT_NE(source.find("tl_InEarlyArm ? DllVersionReadPolicy::DirectPeOnly"), std::string::npos);
    EXPECT_NE(source.find("DllFileMajorVersion(overridePath.c_str(), versionPolicy)"), std::string::npos);
}
}  // namespace
