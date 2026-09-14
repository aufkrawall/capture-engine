#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../hook/apis/ffx_hook.h"
#include "../hook/common/module_export_resolver.h"
#include "../hook/common/streamline_runtime_policy.h"
#include "../hook/wrappers/iat_import_table.h"
#include "../hook/wrappers/iat_hook.h"

namespace {

bool StreamlineCoreModuleFilter(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleBaseName);
}

bool StreamlineReflexModuleFilter(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);
}

struct SyntheticImportImage {
    static constexpr DWORD kDirectoryRva = 0x100;
    static constexpr DWORD kModuleNameRva = 0x200;
    static constexpr DWORD kIatRva = 0x300;
    static constexpr DWORD kNameTableRva = 0x400;
    static constexpr DWORD kFunctionNameRva = 0x500;

    SyntheticImportImage() : bytes(0x1000, 0) {
        auto* descriptor = At<IMAGE_IMPORT_DESCRIPTOR>(kDirectoryRva);
        descriptor->Name = kModuleNameRva;
        descriptor->FirstThunk = kIatRva;
        WriteString(kModuleNameRva, "KERNEL32.dll");
    }

    template <typename T>
    T* At(DWORD rva) {
        return reinterpret_cast<T*>(bytes.data() + rva);
    }

    void WriteString(DWORD rva, const char* value) {
        std::memcpy(bytes.data() + rva, value, std::strlen(value) + 1);
    }

    IATHook::detail::ImportEntryLookup Find(void* expectedFunction, void* hookFunction,
                                            IATHook::detail::TrackedImportLookup trackedLookup = nullptr,
                                            void* trackedContext = nullptr) {
        return IATHook::detail::FindImportTableEntry(
            bytes.data(), bytes.size(), kDirectoryRva, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR), "kernel32.dll",
            "LoadLibraryA", expectedFunction, hookFunction, nullptr, trackedLookup, trackedContext);
    }

    std::vector<BYTE> bytes;
};

struct TrackedImportFixture {
    void** expectedEntry = nullptr;
    void* originalFunction = nullptr;
};

bool LookupSyntheticTrackedImport(void* opaqueContext, void** iatEntry, void** originalFunction) {
    const auto* context = static_cast<const TrackedImportFixture*>(opaqueContext);
    if (!context || context->expectedEntry != iatEntry)
        return false;
    *originalFunction = context->originalFunction;
    return true;
}

}  // namespace

TEST(IATHookImportTableTest, NameLessOldLinkerImportMatchesResolvedAddressWithoutReadingItAsAnRva) {
    SyntheticImportImage image;
    auto* iat = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kIatRva);
    void* expectedFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x766E1F70));
    void* hookFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
    iat[0].u1.Function = reinterpret_cast<ULONG_PTR>(expectedFunction);

    const auto result = image.Find(expectedFunction, hookFunction);

    EXPECT_EQ(result.issue, IATHook::detail::ImportTableIssue::None);
    EXPECT_TRUE(result.sourceModuleFound);
    EXPECT_TRUE(result.usedResolvedAddress);
    EXPECT_EQ(result.iatEntry, &iat[0]);
}

TEST(IATHookImportTableTest, NameLessImportPreservesAnUnidentifiedForeignReplacement) {
    SyntheticImportImage image;
    auto* iat = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kIatRva);
    void* expectedFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x766E1F70));
    void* foreignFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x34567890));
    void* hookFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
    iat[0].u1.Function = reinterpret_cast<ULONG_PTR>(foreignFunction);

    const auto result = image.Find(expectedFunction, hookFunction);

    EXPECT_EQ(result.issue, IATHook::detail::ImportTableIssue::None);
    EXPECT_TRUE(result.sourceModuleFound);
    EXPECT_EQ(result.iatEntry, nullptr);
}

TEST(IATHookImportTableTest, NameLessImportRecognizesOnlyItsTrackedExistingHook) {
    SyntheticImportImage image;
    auto* iat = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kIatRva);
    void* expectedFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x766E1F70));
    void* hookFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
    iat[0].u1.Function = reinterpret_cast<ULONG_PTR>(hookFunction);
    TrackedImportFixture tracking{reinterpret_cast<void**>(&iat[0].u1.Function), expectedFunction};

    const auto result = image.Find(expectedFunction, hookFunction, &LookupSyntheticTrackedImport, &tracking);

    EXPECT_EQ(result.issue, IATHook::detail::ImportTableIssue::None);
    EXPECT_TRUE(result.usedResolvedAddress);
    EXPECT_EQ(result.iatEntry, &iat[0]);
    EXPECT_EQ(result.trackedOriginal, expectedFunction);
}

TEST(IATHookImportTableTest, NormalImportStillMatchesBoundedNameMetadata) {
    SyntheticImportImage image;
    auto* descriptor = image.At<IMAGE_IMPORT_DESCRIPTOR>(SyntheticImportImage::kDirectoryRva);
    descriptor->OriginalFirstThunk = SyntheticImportImage::kNameTableRva;
    auto* names = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kNameTableRva);
    names[0].u1.AddressOfData = SyntheticImportImage::kFunctionNameRva;
    image.WriteString(SyntheticImportImage::kFunctionNameRva + offsetof(IMAGE_IMPORT_BY_NAME, Name), "LoadLibraryA");
    auto* iat = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kIatRva);

    const auto result = image.Find(reinterpret_cast<void*>(static_cast<uintptr_t>(0x766E1F70)),
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678)));

    EXPECT_EQ(result.issue, IATHook::detail::ImportTableIssue::None);
    EXPECT_FALSE(result.usedResolvedAddress);
    EXPECT_EQ(result.iatEntry, &iat[0]);
}

TEST(IATHookImportTableTest, OutOfImageNameRvaFailsClosed) {
    SyntheticImportImage image;
    auto* descriptor = image.At<IMAGE_IMPORT_DESCRIPTOR>(SyntheticImportImage::kDirectoryRva);
    descriptor->OriginalFirstThunk = SyntheticImportImage::kNameTableRva;
    auto* names = image.At<IMAGE_THUNK_DATA>(SyntheticImportImage::kNameTableRva);
    names[0].u1.AddressOfData = static_cast<ULONG_PTR>(image.bytes.size() - 1);

    const auto result = image.Find(reinterpret_cast<void*>(static_cast<uintptr_t>(0x766E1F70)),
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678)));

    EXPECT_EQ(result.issue, IATHook::detail::ImportTableIssue::InvalidImportName);
    EXPECT_EQ(result.iatEntry, nullptr);
}

TEST(IATHookImportTableTest, RangeValidationRejectsOverflowAndEndOverrun) {
    EXPECT_TRUE(IATHook::detail::ImageRangeContains(0x1000, 0xF00, 0x100));
    EXPECT_FALSE(IATHook::detail::ImageRangeContains(0x1000, 0xF01, 0x100));
    EXPECT_FALSE(IATHook::detail::ImageRangeContains(0x1000, UINTPTR_MAX, 2));
}

TEST(IATHookDynamicFilterTest, UnfilteredDynamicHookStillRoutesAllModules) {
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(nullptr, "kernel32.dll", nullptr));
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(nullptr, "sl.reflex.dll", nullptr));
}

TEST(IATHookTargetFilterTest, WindowsRuntimeDirectoriesAreExcludedCaseInsensitively) {
    EXPECT_TRUE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Windows\\System32\\KERNELBASE.dll", L"C:\\Windows"));
    EXPECT_TRUE(IATHook::IsWindowsSystemModulePathUnderRoot(L"c:\\windows\\SYSWOW64\\kernel32.dll", L"C:\\WINDOWS\\"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Games\\System32\\mod.dll", L"C:\\Windows"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Games\\test.exe", L"C:\\Windows"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(nullptr, L"C:\\Windows"));
    EXPECT_TRUE(IATHook::IsPathUnderDirectoryRoot(L"C:\\Windows\\WinSxS\\runtime.dll", L"c:\\WINDOWS\\"));
    EXPECT_FALSE(IATHook::IsPathUnderDirectoryRoot(L"C:\\WindowsOld\\System32\\kernel32.dll", L"C:\\Windows"));
}

TEST(IATHookTargetFilterTest, NonSystemGraphicsProxyModulesAreKeptOutOfCEsIAT) {
    EXPECT_TRUE(IATHook::IsGraphicsProxyModuleBaseName(L"C:\\Games\\dxgi.dll"));
    EXPECT_TRUE(IATHook::IsGraphicsProxyModuleBaseName(L"C:\\Mods\\VERSION.DLL"));
    EXPECT_TRUE(IATHook::IsGraphicsProxyModuleBaseName(L"C:\\Mods\\dinput8.dll"));
    EXPECT_TRUE(IATHook::IsGraphicsProxyModuleBaseName(L"C:\\Mods\\opengl32.dll"));
    EXPECT_TRUE(IATHook::IsGraphicsProxyModuleBaseName(L"nvngx.dll"));
    EXPECT_FALSE(IATHook::IsGraphicsProxyModuleBaseName(L"C:\\Games\\game.dll"));

    EXPECT_TRUE(IATHook::IsNonSystemGraphicsProxyModulePath(L"C:\\Games\\dxgi.dll"));
    EXPECT_FALSE(IATHook::IsNonSystemGraphicsProxyModulePath(L"C:\\Windows\\System32\\dxgi.dll"));
}

TEST(IATHookDynamicFilterTest, FilteredDynamicHookRoutesOnlyMatchingModules) {
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.interposer.dll", nullptr));
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "SL.COMMON.DLL", nullptr));

    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.dlss_g.dll", nullptr));

    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.common.dll", nullptr));
}

TEST(IATHookDynamicFilterTest, FilteredHookPreservesForeignResolvedTargets) {
    EXPECT_FALSE(IATHook::ShouldPreserveFilteredForeignResolvedTarget(false, false, false));
    EXPECT_FALSE(IATHook::ShouldPreserveFilteredForeignResolvedTarget(true, true, true));
    EXPECT_TRUE(IATHook::ShouldPreserveFilteredForeignResolvedTarget(true, true, false));
    EXPECT_TRUE(IATHook::ShouldPreserveFilteredForeignResolvedTarget(true, false, false));
}

TEST(IATHookExportResolverTest, DirectResolutionBypassesGetProcAddressWithoutChangingTheOwner) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    ASSERT_NE(ntdll, nullptr);
    void* resolved = ce::module_export::ResolveAddressDirect(ntdll, "RtlAllocateHeap");
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved, reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAllocateHeap")));

    HMODULE owner = nullptr;
    ASSERT_TRUE(GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(resolved), &owner));
    EXPECT_EQ(owner, ntdll);
    EXPECT_EQ(ce::module_export::ResolveAddressDirect(ntdll, "CE_DefinitelyMissingExport"), nullptr);
}

TEST(IATHookDynamicFilterTest, OverlayCallerBypassKeepsNativeFSRApiHooksVisible) {
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, false,
                                                          "D3D12CreateDevice"));
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                          "D3D12CreateDevice"));

    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxConfigure"));
    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxCreateContext"));
    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxDestroyContext"));

    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(true, false, false, false, false, false, false, true,
                                                          "ffxConfigure"));
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, false, false, false, false, true, false, true,
                                                          "ffxConfigure"));
}

TEST(FFXHookPolicyTest, EntryBreakpointHitAcceptsExceptionAddressOrAdvancedInstructionPointer) {
    const uintptr_t target = 0x100000;

    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target), target + 1,
                                                      reinterpret_cast<const void*>(target)));
    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target + 1), target + 1,
                                                      reinterpret_cast<const void*>(target)));
    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(0x200000), target + 1,
                                                      reinterpret_cast<const void*>(target)));

    EXPECT_FALSE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(0x200000), 0x200001,
                                                       reinterpret_cast<const void*>(target)));
    EXPECT_FALSE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target), target + 1, nullptr));
}
