#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hook/hooking/iat_import_table.h"
#include "hook/runtime/pe_file_exports.h"
#include "hook/streamline/streamline_interposer_takeover.h"
#include "hook/streamline/streamline_takeover_core_scan.h"
#include "hook/streamline/streamline_takeover_import_coverage.h"
#include "source_fragment_reader.h"

namespace {

using IATHook::detail::ForEachImportOf;
using IATHook::detail::ImportNameKind;
using IATHook::detail::ImportTableIssue;
using namespace ce::streamline_interposer_takeover;

// A PE32+ image whose file layout equals its mapped layout (one section, raw
// offset == RVA), so the same bytes exercise the on-disk export parser and the
// in-memory import walk.
class SyntheticImage {
public:
    static constexpr DWORD kSectionRva = 0x1000;
    static constexpr DWORD kImageSize = 0x3000;

    SyntheticImage() : bytes_(kImageSize, 0) {
        auto* dos = At<IMAGE_DOS_HEADER>(0);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x80;
        auto* nt = At<IMAGE_NT_HEADERS64>(0x80);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.NumberOfSections = 1;
        nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.SizeOfImage = kImageSize;
        auto* section = At<IMAGE_SECTION_HEADER>(0x80 + sizeof(IMAGE_NT_HEADERS64));
        std::memcpy(section->Name, ".rdata", 6);
        section->VirtualAddress = kSectionRva;
        section->Misc.VirtualSize = kImageSize - kSectionRva;
        section->PointerToRawData = kSectionRva;
        section->SizeOfRawData = kImageSize - kSectionRva;
    }

    void SetExports(const std::vector<std::string>& names) {
        auto* nt = At<IMAGE_NT_HEADERS64>(0x80);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {0x1000, 0x400};
        auto* directory = At<IMAGE_EXPORT_DIRECTORY>(0x1000);
        directory->NumberOfNames = static_cast<DWORD>(names.size());
        directory->AddressOfNames = 0x1080;
        DWORD stringRva = 0x1200;
        for (size_t i = 0; i < names.size(); ++i) {
            *At<DWORD>(0x1080 + i * sizeof(DWORD)) = stringRva;
            stringRva = PutString(stringRva, names[i]);
        }
    }

    struct Import {
        std::string name;  // empty: ordinal import
    };

    // One descriptor per module; `withNameTable=false` leaves OriginalFirstThunk 0.
    void SetImports(const std::vector<std::pair<std::string, std::vector<Import>>>& modules, bool withNameTable = true) {
        auto* nt = At<IMAGE_NT_HEADERS64>(0x80);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {
            0x1800, static_cast<DWORD>((modules.size() + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR))};
        DWORD cursor = 0x1A00;
        for (size_t m = 0; m < modules.size(); ++m) {
            auto* descriptor = At<IMAGE_IMPORT_DESCRIPTOR>(0x1800 + m * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            descriptor->Name = cursor;
            cursor = PutString(cursor, modules[m].first);
            const DWORD lookup = Align(cursor);
            const DWORD iat = lookup + static_cast<DWORD>((modules[m].second.size() + 1) * 8);
            cursor = iat + static_cast<DWORD>((modules[m].second.size() + 1) * 8);
            descriptor->OriginalFirstThunk = withNameTable ? lookup : 0;
            descriptor->FirstThunk = iat;
            for (size_t i = 0; i < modules[m].second.size(); ++i) {
                uint64_t value = IMAGE_ORDINAL_FLAG64 | (i + 1);
                if (!modules[m].second[i].name.empty()) {
                    value = cursor;
                    cursor = PutString(cursor + 2, modules[m].second[i].name);
                    cursor = Align(cursor);
                }
                *At<uint64_t>(lookup + i * 8) = value;
                *At<uint64_t>(iat + i * 8) = withNameTable ? value : 0x7FF000000000ull + i;
            }
        }
    }

    ImportTableIssue Walk(const char* module, std::vector<std::string>* named, size_t* ordinal, size_t* unnamed) {
        const auto& dir = At<IMAGE_NT_HEADERS64>(0x80)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        return ForEachImportOf(
            bytes_.data(), bytes_.size(), dir.VirtualAddress, dir.Size, module,
            [&](ImportNameKind kind, const char* name, IMAGE_THUNK_DATA*) {
                if (kind == ImportNameKind::Named) named->emplace_back(name);
                if (kind == ImportNameKind::Ordinal) ++*ordinal;
                if (kind == ImportNameKind::Unnamed) ++*unnamed;
            },
            nullptr);
    }

    template <typename T>
    T* At(size_t offset) {
        return reinterpret_cast<T*>(bytes_.data() + offset);
    }
    std::vector<uint8_t>& Bytes() { return bytes_; }

private:
    DWORD PutString(DWORD rva, const std::string& text) {
        std::memcpy(bytes_.data() + rva, text.c_str(), text.size() + 1);
        return rva + static_cast<DWORD>(text.size() + 1);
    }
    static DWORD Align(DWORD value) { return (value + 7u) & ~7u; }
    std::vector<uint8_t> bytes_;
};

TEST(StreamlineTakeoverImports, ExportParserReadsNamesAndRejectsTruncation) {
    SyntheticImage image;
    image.SetExports({"slInit", "vkQueueSubmit", "D3D11CreateDevice"});
    std::vector<std::string> names;
    ASSERT_TRUE(ce::pe_file_exports::ParseExportNames(image.Bytes().data(), image.Bytes().size(), &names));
    EXPECT_EQ(names, (std::vector<std::string>{"slInit", "vkQueueSubmit", "D3D11CreateDevice"}));

    // Every truncation point fails cleanly; none reads out of bounds.
    for (size_t size : {size_t{0}, size_t{2}, size_t{0x80}, size_t{0x200}, size_t{0x1004}, size_t{0x1082},
                        size_t{0x1203}}) {
        EXPECT_FALSE(ce::pe_file_exports::ParseExportNames(image.Bytes().data(), size, &names)) << size;
        EXPECT_TRUE(names.empty()) << size;
    }
    // A name count no table can hold is rejected rather than walked.
    image.At<IMAGE_EXPORT_DIRECTORY>(0x1000)->NumberOfNames = 0x7FFFFFFF;
    EXPECT_FALSE(ce::pe_file_exports::ParseExportNames(image.Bytes().data(), image.Bytes().size(), &names));
}

TEST(StreamlineTakeoverImports, ExportParserReadsARealDllFromDisk) {
    wchar_t system[MAX_PATH] = {};
    ASSERT_GT(GetSystemDirectoryW(system, MAX_PATH), 0u);
    std::vector<std::string> names;
    ASSERT_TRUE(ce::pe_file_exports::ReadFileExportNames(
        (std::filesystem::path(system) / L"version.dll").c_str(), &names));
    EXPECT_NE(std::find(names.begin(), names.end(), "GetFileVersionInfoW"), names.end());
    EXPECT_FALSE(ce::pe_file_exports::ReadFileExportNames(L"Z:\\does\\not\\exist.dll", &names));
    EXPECT_TRUE(names.empty());
}

TEST(StreamlineTakeoverImports, ImportWalkSeparatesNamedOrdinalAndUnnamedImports) {
    SyntheticImage image;
    image.SetImports({{"KERNEL32.dll", {{"GetModuleHandleW"}}},
                      {"sl.interposer.dll", {{"slInit"}, {"vkQueueSubmit"}, {""}}}});
    std::vector<std::string> named;
    size_t ordinal = 0;
    size_t unnamed = 0;
    EXPECT_EQ(image.Walk("sl.interposer.dll", &named, &ordinal, &unnamed), ImportTableIssue::None);
    EXPECT_EQ(named, (std::vector<std::string>{"slInit", "vkQueueSubmit"}));
    EXPECT_EQ(ordinal, 1u);
    EXPECT_EQ(unnamed, 0u);

    SyntheticImage bound;
    bound.SetImports({{"sl.interposer.dll", {{"slInit"}, {"slShutdown"}}}}, false);
    named.clear();
    ordinal = 0;
    EXPECT_EQ(bound.Walk("SL.INTERPOSER.DLL", &named, &ordinal, &unnamed), ImportTableIssue::None);
    EXPECT_TRUE(named.empty());
    EXPECT_EQ(unnamed, 2u);
}

TEST(StreamlineTakeoverImports, ImportWalkReadsTheRunningExecutable) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t named = 0;
    bool sawGetModuleHandleW = false;
    const auto issue = ForEachImportOf(base, nt->OptionalHeader.SizeOfImage, dir.VirtualAddress, dir.Size,
                                       "kernel32.dll",
                                       [&](ImportNameKind kind, const char* name, IMAGE_THUNK_DATA*) {
                                           if (kind == ImportNameKind::Named) {
                                               ++named;
                                               sawGetModuleHandleW |= std::strcmp(name, "GetModuleHandleW") == 0;
                                           }
                                       },
                                       &IATHook::detail::IsCommittedReadableMemory);
    EXPECT_TRUE(issue == ImportTableIssue::None || issue == ImportTableIssue::UnterminatedDescriptors);
    EXPECT_GT(named, 0u);
    EXPECT_TRUE(sawGetModuleHandleW);
}

// sl.interposer.dll exports some 260 names; only the gated ones have thunks.
// The rest move at activation, which needs every imported name to exist in the
// configured copy and to be a name at all.
TEST(StreamlineTakeoverImports, CoverageDeclinesAnythingThatCannotMoveAtOnce) {
    std::vector<std::string> overrideExports = {"slInit", "slSetTag", "vkQueueSubmit", "vkCmdSetLineWidth"};
    std::sort(overrideExports.begin(), overrideExports.end());

    ImportCoverage complete;
    complete.overrideExportsKnown = true;
    RecordNamedImport(&complete, "slInit", true, overrideExports);
    RecordNamedImport(&complete, "vkCmdSetLineWidth", false, overrideExports);
    EXPECT_EQ(DecideImportCoverage(complete), ImportCoverageVerdict::Complete);
    EXPECT_EQ(complete.named, 2u);
    EXPECT_EQ(complete.thunked, 1u);

    // An older same-major override that lacks an export the game imports.
    ImportCoverage downgrade = complete;
    RecordNamedImport(&downgrade, "slSetTagForFrame", true, overrideExports);
    RecordNamedImport(&downgrade, "vkGetSwapchainImagesKHR", false, overrideExports);
    EXPECT_EQ(DecideImportCoverage(downgrade), ImportCoverageVerdict::MissingOverrideExport);
    EXPECT_EQ(downgrade.missingFromOverride, 2u);
    EXPECT_EQ(downgrade.firstMissing, "slSetTagForFrame");

    ImportCoverage ordinal = complete;
    ordinal.ordinal = 1;
    EXPECT_EQ(DecideImportCoverage(ordinal), ImportCoverageVerdict::OrdinalImports);
    ImportCoverage unnamed = complete;
    unnamed.unnamed = 3;
    EXPECT_EQ(DecideImportCoverage(unnamed), ImportCoverageVerdict::UnnamedImports);
    ImportCoverage malformed = complete;
    malformed.malformed = true;
    EXPECT_EQ(DecideImportCoverage(malformed), ImportCoverageVerdict::MalformedImports);
    ImportCoverage unreadable;
    RecordNamedImport(&unreadable, "slInit", true, overrideExports);
    EXPECT_EQ(DecideImportCoverage(unreadable), ImportCoverageVerdict::OverrideExportsUnreadable);
}

// D3D11 device creation can create runtime state before slInit, like the D3D12
// and DXGI proxies, so it must be a thunked entry export; the 12-argument
// D3D11CreateDeviceAndSwapChain sets the thunk width.
TEST(StreamlineTakeoverImports, D3D11DeviceCreationIsAThunkedEntryExport) {
    for (const char* name : {"D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"}) {
        const auto it = std::find_if(std::begin(kTakeoverExportNames), std::end(kTakeoverExportNames),
                                     [&](const char* entry) { return std::strcmp(entry, name) == 0; });
        ASSERT_NE(it, std::end(kTakeoverExportNames)) << name;
        EXPECT_TRUE(IsTakeoverEntryExportIndex(static_cast<size_t>(it - std::begin(kTakeoverExportNames)))) << name;
    }
    EXPECT_GE(kTakeoverThunkArgumentCount, 12u);
}

TEST(StreamlineTakeoverImports, ProductionGatesArmingAndMovesUnthunkedImports) {
    const auto root = std::filesystem::current_path();
    const auto takeover =
        ce::test_source::ReadLogicalSource(root / "hook/streamline/streamline_interposer_takeover.cpp");
    ASSERT_FALSE(takeover.empty());
    const auto coverage = takeover.find("DecideImportCoverage(coverage)");
    const auto arm = takeover.find("g_TakeoverArmed.store(true", coverage);
    ASSERT_NE(coverage, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    const auto ensure = takeover.find("bool EnsureCustomInterposerReady() {");
    const auto direct = takeover.find("RebindUnthunkedImports(mod, superseded, customModule)", ensure);
    const auto active = takeover.find("g_TakeoverActive.store(true", ensure);
    ASSERT_NE(direct, std::string::npos);
    EXPECT_LT(direct, active);
    // Once the override owns the runtime, a missing export fails the call
    // instead of reaching the uninitialized resident interposer.
    const auto dispatch = takeover.find("uint64_t DispatchTakeoverSlot(");
    const auto noFallback = takeover.find("g_TakeoverDecision.IsCustom() && g_RuntimePublication.Query()", dispatch);
    const auto fallback = takeover.find("target = g_FallbackTargets[index]", dispatch);
    ASSERT_NE(noFallback, std::string::npos);
    EXPECT_LT(noFallback, fallback);
}

}  // namespace
