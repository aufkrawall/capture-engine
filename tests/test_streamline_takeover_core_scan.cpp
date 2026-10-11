#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hook/streamline/streamline_interposer_takeover.h"
#include "hook/streamline/streamline_takeover_core_scan.h"
#include "source_fragment_reader.h"

namespace {
using ce::streamline_interposer_takeover::CanActivateTakeoverForExport;
using ce::streamline_interposer_takeover::CorePresence;
using ce::streamline_interposer_takeover::IsStreamlineCoreModulePath;
using ce::streamline_interposer_takeover::IsTakeoverEntryExportIndex;
using ce::streamline_interposer_takeover::kFirstNonSlTakeoverExportIndex;
using ce::streamline_interposer_takeover::kSlInitTakeoverExportIndex;
using ce::streamline_interposer_takeover::kTakeoverExportCount;
using ce::streamline_interposer_takeover::kTakeoverExportNames;
using ce::streamline_interposer_takeover::ScanLoadedStreamlineCore;

CorePresence ScanPaths(const std::vector<std::string>& paths) {
    return ScanLoadedStreamlineCore([&](std::vector<HMODULE>& modules) {
        for (size_t i = 0; i < paths.size(); ++i)
            modules.push_back(reinterpret_cast<HMODULE>(i + 1));
        return true;
    }, [&](HMODULE module, char* out, DWORD capacity) -> DWORD {
        const auto& path = paths[reinterpret_cast<uintptr_t>(module) - 1];
        if (path.size() >= capacity)
            return capacity;
        std::memcpy(out, path.c_str(), path.size() + 1);
        return static_cast<DWORD>(path.size());
    });
}

TEST(StreamlineTakeoverCoreScan, CanonicalAndCachedCoresBothPreventTakeover) {
    for (const char* path : {
        "C:\\game\\SL.Common.DLL",
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\1B0_E658703.dll",
        "C:/cache/NVIDIA/NGX/models/SL_COMMON_0/versions/another_hash.dll"}) {
        const auto presence = ScanPaths({"C:\\game\\sl.interposer.dll", path});
        EXPECT_EQ(presence, CorePresence::Loaded) << path;
        EXPECT_FALSE(ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer(
            true, true, presence != CorePresence::Absent, true, 2, 2));
    }
}

TEST(StreamlineTakeoverCoreScan, DormantInterposerAndUnrelatedCachedPluginsPermitTakeover) {
    const auto presence = ScanPaths({"C:\\game\\sl.interposer.dll", "C:\\Windows\\System32\\kernel32.dll",
        "C:\\cache\\NVIDIA\\NGX\\models\\sl_dlss_0\\versions\\hash.dll",
        "C:\\cache\\NVIDIA\\NGX\\models\\nvngx_dlss_0\\versions\\hash.dll"});
    EXPECT_EQ(presence, CorePresence::Absent);
    EXPECT_TRUE(ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer(
        true, true, presence != CorePresence::Absent, true, 2, 2));
    EXPECT_EQ(ScanPaths({}), CorePresence::Absent);
}

TEST(StreamlineTakeoverCoreScan, IncompleteModuleInventoryCannotEstablishDormancy) {
    EXPECT_EQ(ScanLoadedStreamlineCore([](std::vector<HMODULE>&) { return false; },
        [](HMODULE, char*, DWORD) -> DWORD { return 0; }), CorePresence::Unresolved);
    for (const auto& paths : std::vector<std::vector<std::string>>{{""}, {std::string(MAX_PATH, 'a')}}) {
        const auto presence = ScanPaths(paths);
        EXPECT_EQ(presence, CorePresence::Unresolved);
        EXPECT_FALSE(ce::graphics_runtime::CanTakeOverForeignStreamlineInterposer(
            true, true, presence != CorePresence::Absent, true, 1, 1));
    }
    EXPECT_EQ(ScanPaths({"", "C:\\game\\sl.common.dll"}), CorePresence::Loaded);
}

TEST(StreamlineTakeoverCoreScan, ProductionArmingUsesCompleteSemanticInventory) {
    const auto source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "hook/streamline/streamline_interposer_takeover.cpp");
    const auto scan = source.find("const CorePresence corePresence = ScanLoadedStreamlineCore(");
    const auto policy = source.find("CanTakeOverForeignStreamlineInterposer(", scan);
    const auto arm = source.find("g_TakeoverArmed.store(true", policy);
    ASSERT_NE(scan, std::string::npos);
    ASSERT_NE(policy, std::string::npos);
    ASSERT_NE(arm, std::string::npos);
    EXPECT_LT(scan, policy);
    EXPECT_LT(policy, arm);
    EXPECT_NE(source.find("corePresence != CorePresence::Absent", scan), std::string::npos);
    EXPECT_NE(source.find("ce::EnumerateProcessModules(::GetCurrentProcess(), modules)", scan), std::string::npos);
    EXPECT_EQ(source.find("::GetModuleHandleA(\"sl.common.dll\")"), std::string::npos);
}

// 20261011_113848 (The Witcher 3 DX12): when the main thread entered the resident
// sl.interposer.dll!slInit before TryArmTakeover finished patching the IAT, HookThread
// eagerly mapped a second custom sl.interposer.dll while the resident slInit was still
// inside WinVerifyTrust (before sl.common.dll mapped) and rebound the game's IAT to that
// uninitialized duplicate, causing every subsequent slGetFeatureFunction /
// slIsFeatureSupported / slSetD3DDevice call to fail with sl::Result::eErrorNotInitialized (21).
TEST(StreamlineTakeoverCoreScan, EntryThunkActivationRequiresEntryExportAndAbsentCore) {
    ASSERT_STREQ(kTakeoverExportNames[kSlInitTakeoverExportIndex], "slInit");
    ASSERT_STREQ(kTakeoverExportNames[kFirstNonSlTakeoverExportIndex - 1], "slGetNumHooks");
    ASSERT_STREQ(kTakeoverExportNames[kFirstNonSlTakeoverExportIndex], "CreateDXGIFactory");

    EXPECT_TRUE(IsTakeoverEntryExportIndex(kSlInitTakeoverExportIndex));
    EXPECT_TRUE(CanActivateTakeoverForExport(kSlInitTakeoverExportIndex, CorePresence::Absent));
    EXPECT_FALSE(CanActivateTakeoverForExport(kSlInitTakeoverExportIndex, CorePresence::Loaded));
    EXPECT_FALSE(CanActivateTakeoverForExport(kSlInitTakeoverExportIndex, CorePresence::Unresolved));

    for (size_t i = kSlInitTakeoverExportIndex + 1; i < kFirstNonSlTakeoverExportIndex; ++i) {
        EXPECT_FALSE(IsTakeoverEntryExportIndex(i)) << kTakeoverExportNames[i];
        EXPECT_FALSE(CanActivateTakeoverForExport(i, CorePresence::Absent)) << kTakeoverExportNames[i];
        EXPECT_FALSE(CanActivateTakeoverForExport(i, CorePresence::Loaded)) << kTakeoverExportNames[i];
        EXPECT_FALSE(CanActivateTakeoverForExport(i, CorePresence::Unresolved)) << kTakeoverExportNames[i];
    }

    for (size_t i = kFirstNonSlTakeoverExportIndex; i < kTakeoverExportCount; ++i) {
        EXPECT_TRUE(IsTakeoverEntryExportIndex(i)) << kTakeoverExportNames[i];
        EXPECT_TRUE(CanActivateTakeoverForExport(i, CorePresence::Absent)) << kTakeoverExportNames[i];
        EXPECT_FALSE(CanActivateTakeoverForExport(i, CorePresence::Loaded)) << kTakeoverExportNames[i];
        EXPECT_FALSE(CanActivateTakeoverForExport(i, CorePresence::Unresolved)) << kTakeoverExportNames[i];
    }

    EXPECT_TRUE(IsStreamlineCoreModulePath("C:\\npi\\sl\\sl.common.dll"));
    EXPECT_TRUE(IsStreamlineCoreModulePath(
        "C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\133888\\files\\1B0_E658703.dll"));
    EXPECT_FALSE(IsStreamlineCoreModulePath("C:\\npi\\sl\\sl.interposer.dll"));
    EXPECT_FALSE(IsStreamlineCoreModulePath("C:\\npi\\sl\\sl.dlss_g.dll"));
}

TEST(StreamlineTakeoverCoreScan, TakeoverDefersCustomLoadUntilEntryThunkAndRollsBackWhenResidentWinsSlInit) {
    const auto root = std::filesystem::current_path();
    const auto takeover = ce::test_source::ReadLogicalSource(root / "hook/streamline/streamline_interposer_takeover.cpp");
    const auto injection = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_injection.cpp");
    const auto detect = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_overlay_detect.cpp");
    const auto redirect = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_redirect.cpp");
    ASSERT_FALSE(takeover.empty());
    ASSERT_FALSE(injection.empty());
    ASSERT_FALSE(detect.empty());
    ASSERT_FALSE(redirect.empty());

    // DllMain's InstallKernel32LoaderHooks arms takeover before the multi-module kernel32 IAT scan.
    const auto loaderFn = injection.find("void InstallKernel32LoaderHooks(");
    const auto earlyArmInLoader = injection.find("ce::streamline_interposer_takeover::ArmEarlyFromDllMain()", loaderFn);
    const auto kernel32Init = injection.find("IATHook::InitializeKernel32Hooks(", loaderFn);
    ASSERT_NE(loaderFn, std::string::npos);
    ASSERT_NE(earlyArmInLoader, std::string::npos);
    ASSERT_NE(kernel32Init, std::string::npos);
    EXPECT_LT(earlyArmInLoader, kernel32Init);

    // EnsureCustomInterposerReady must not map the custom interposer until an entry thunk activates it.
    const auto ensureBegin = takeover.find("bool EnsureCustomInterposerReady() {");
    const auto entryGate = takeover.find("if (!tl_InEntryThunkActivation)", ensureBegin);
    const auto loadAndPublish = takeover.find("g_RuntimePublication.LoadAndPublish(", ensureBegin);
    ASSERT_NE(ensureBegin, std::string::npos);
    ASSERT_NE(entryGate, std::string::npos);
    ASSERT_NE(loadAndPublish, std::string::npos);
    EXPECT_LT(entryGate, loadAndPublish);

    // DispatchTakeoverSlot and NoteTakeoverInterposerMapped must roll back to the resident interposer
    // if a post-slInit export or a Streamline core load arrives before takeover activates.
    EXPECT_NE(takeover.find("CanActivateTakeoverForExport(index, corePresence)"), std::string::npos);
    EXPECT_NE(takeover.find("AbandonArmedTakeover("), std::string::npos);
    EXPECT_NE(takeover.find("IsStreamlineCoreModulePath(resolvedPath)"), std::string::npos);
    EXPECT_NE(takeover.find("StreamlineHook::OnModuleLoaded("), std::string::npos);
    EXPECT_NE(detect.find("NoteTakeoverInterposerMapped("), std::string::npos);
    EXPECT_NE(redirect.find("!ce::streamline_interposer_takeover::IsTakeoverActive()"), std::string::npos);
}
}  // namespace
