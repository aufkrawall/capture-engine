#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "hook/streamline/streamline_takeover_core_scan.h"
#include "source_fragment_reader.h"

namespace {
using ce::streamline_interposer_takeover::CorePresence;
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
}  // namespace
