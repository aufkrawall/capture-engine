#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <string>

#include "../hook/common/overlay_compat.h"
#include "source_fragment_reader.h"

// GTA session 20260925_225006: CE classified Present/ECL callers with GetModuleHandleExA(FROM_ADDRESS) and
// GetModuleFileNameA several times per call. GetModuleFileNameA takes the loader lock, so a DLL load on any
// other thread stalled AMD's presenter thread for the whole load. Module identity is now served from a table
// invalidated by the loader's unload notification.

namespace {

namespace cache = ce::overlay_compat::module_address_cache;

cache::Entry MakeEntry(std::uintptr_t begin, std::uintptr_t end, std::uint64_t generation, const char* path) {
    cache::Entry entry;
    entry.begin = begin;
    entry.end = end;
    entry.module = reinterpret_cast<HMODULE>(begin);
    entry.generation = generation;
    strncpy_s(entry.path, path, _TRUNCATE);
    return entry;
}

TEST(ModuleAddressCacheTable, FindsAddressesInsideTheImageRangeOnly) {
    cache::Table<4> table;
    ASSERT_TRUE(table.Insert(MakeEntry(0x10000, 0x20000, 7, "C:\\a.dll"), 7));

    EXPECT_NE(table.Find(0x10000, 7), nullptr);
    EXPECT_NE(table.Find(0x1FFFF, 7), nullptr);
    EXPECT_EQ(table.Find(0x20000, 7), nullptr) << "end is exclusive";
    EXPECT_EQ(table.Find(0x0FFFF, 7), nullptr);
}

TEST(ModuleAddressCacheTable, AnyUnloadInvalidatesEveryEntry) {
    cache::Table<4> table;
    ASSERT_TRUE(table.Insert(MakeEntry(0x10000, 0x20000, 3, "C:\\a.dll"), 3));
    EXPECT_NE(table.Find(0x15000, 3), nullptr);
    // Another image mapping at the old range after an unload must never be answered with the stale path.
    EXPECT_EQ(table.Find(0x15000, 4), nullptr);
}

TEST(ModuleAddressCacheTable, ResolutionThatRacedAnUnloadIsNotStored) {
    cache::Table<4> table;
    EXPECT_FALSE(table.Insert(MakeEntry(0x10000, 0x20000, 3, "C:\\a.dll"), 4));
    EXPECT_EQ(table.Find(0x15000, 3), nullptr);
    EXPECT_EQ(table.Find(0x15000, 4), nullptr);
}

TEST(ModuleAddressCacheTable, RejectsEmptyRangesAndReplacesRoundRobin) {
    cache::Table<2> table;
    EXPECT_FALSE(table.Insert(MakeEntry(0x10000, 0x10000, 1, "C:\\empty.dll"), 1));
    ASSERT_TRUE(table.Insert(MakeEntry(0x10000, 0x20000, 1, "C:\\a.dll"), 1));
    ASSERT_TRUE(table.Insert(MakeEntry(0x30000, 0x40000, 1, "C:\\b.dll"), 1));
    ASSERT_TRUE(table.Insert(MakeEntry(0x50000, 0x60000, 1, "C:\\c.dll"), 1));
    EXPECT_EQ(table.Find(0x15000, 1), nullptr) << "oldest entry evicted";
    EXPECT_NE(table.Find(0x35000, 1), nullptr);
    EXPECT_NE(table.Find(0x55000, 1), nullptr);
}

TEST(ModuleAddressCacheTable, ReinsertingTheSameModuleUpdatesInPlace) {
    cache::Table<2> table;
    ASSERT_TRUE(table.Insert(MakeEntry(0x10000, 0x20000, 1, "C:\\a.dll"), 1));
    ASSERT_TRUE(table.Insert(MakeEntry(0x30000, 0x40000, 1, "C:\\b.dll"), 1));
    ASSERT_TRUE(table.Insert(MakeEntry(0x10000, 0x20000, 2, "C:\\a.dll"), 2));
    EXPECT_NE(table.Find(0x35000, 1), nullptr) << "an update must not evict another module";
    EXPECT_NE(table.Find(0x15000, 2), nullptr);
}

TEST(ModuleAddressCacheLive, ResolvesTheSameModuleAndPathAsTheLoaderAndServesRepeatsFromTheTable) {
    cache::Enable();
    const void* address = reinterpret_cast<const void*>(&MakeEntry);
    HMODULE expectedModule = nullptr;
    ASSERT_TRUE(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(address), &expectedModule));
    char expectedPath[MAX_PATH] = {};
    ASSERT_GT(GetModuleFileNameA(expectedModule, expectedPath, MAX_PATH), 0u);

    char path[MAX_PATH] = {};
    HMODULE module = nullptr;
    ASSERT_TRUE(ce::overlay_compat::TryGetModulePathFromCodeAddress(address, path, sizeof(path), &module));
    EXPECT_EQ(module, expectedModule);
    EXPECT_STREQ(path, expectedPath);

    const std::uint64_t hitsBefore = cache::Hits();
    const std::uint64_t missesBefore = cache::Misses();
    char again[MAX_PATH] = {};
    ASSERT_TRUE(ce::overlay_compat::TryGetModulePathFromCodeAddress(address, again, sizeof(again)));
    EXPECT_STREQ(again, expectedPath);
    EXPECT_EQ(cache::Hits(), hitsBefore + 1);
    EXPECT_EQ(cache::Misses(), missesBefore);

    cache::NoteModuleUnloaded();
    ASSERT_TRUE(ce::overlay_compat::TryGetModulePathFromCodeAddress(address, again, sizeof(again)));
    EXPECT_EQ(cache::Misses(), missesBefore + 1) << "an unload forces the next lookup back to the loader";
    EXPECT_STREQ(again, expectedPath);
}

TEST(ModuleAddressCacheLive, AddressOutsideAnyModuleIsNotResolved) {
    cache::Enable();
    int onStack = 0;
    char path[MAX_PATH] = "unchanged";
    HMODULE module = reinterpret_cast<HMODULE>(1);
    EXPECT_FALSE(ce::overlay_compat::TryGetModulePathFromCodeAddress(&onStack, path, sizeof(path), &module));
    EXPECT_EQ(module, nullptr);
    EXPECT_EQ(path[0], '\0');
}

TEST(ModuleAddressCacheLive, ClassifiersAnswerWithoutACallerBuffer) {
    cache::Enable();
    // The Streamline classifier used to classify the caller's (possibly null) output buffer.
    EXPECT_FALSE(ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(
        reinterpret_cast<const void*>(&MakeEntry)));
    EXPECT_FALSE(
        ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(reinterpret_cast<const void*>(&MakeEntry)));
}

std::string ReadSource(const char* relativePath) {
    return ce::test_source::ReadFile(std::filesystem::current_path() / relativePath);
}

TEST(ModuleAddressCacheSource, UnloadNotificationInvalidatesBeforeAnyEarlyReturnAndEnablesTheCache) {
    const std::string detect = ReadSource("hook/main_overlay_detect.cpp");
    ASSERT_FALSE(detect.empty());
    const size_t callback = detect.find("OverlayDllNotificationCallback(ULONG reason");
    const size_t invalidate = detect.find("module_address_cache::NoteModuleUnloaded()", callback);
    const size_t firstReturn = detect.find("return;", callback);
    ASSERT_NE(callback, std::string::npos);
    ASSERT_NE(invalidate, std::string::npos);
    EXPECT_LT(invalidate, firstReturn) << "a dormant or malformed notification must still invalidate";
    EXPECT_NE(detect.find("module_address_cache::NoteModuleLoaded()", callback), std::string::npos);

    const size_t registered = detect.find("if (status == 0) {");
    const size_t enable = detect.find("module_address_cache::Enable()");
    ASSERT_NE(registered, std::string::npos);
    ASSERT_NE(enable, std::string::npos);
    EXPECT_GT(enable, registered) << "the cache may only serve entries once unloads are observed";
}

TEST(ModuleAddressCacheSource, HotPathHelpersNoLongerAskTheLoaderPerCall) {
    const std::string helpers = ReadSource("hook/apis/dx12_hook_helpers.cpp");
    const std::string steam = ReadSource("hook/common/dxgi_shared_steam.cpp");
    const std::string table = ReadSource("hook/common/overlay_compat_detail/module_table.h");
    ASSERT_FALSE(helpers.empty());
    ASSERT_FALSE(steam.empty());
    ASSERT_FALSE(table.empty());

    const auto body = [](const std::string& source, const std::string& begin) {
        const size_t start = source.find(begin);
        const size_t stop = start == std::string::npos ? std::string::npos : source.find("\n}\n", start);
        return start == std::string::npos ? std::string() : source.substr(start, stop - start);
    };
    const std::string dx12 = body(helpers, "bool TryGetModulePathFromCodeAddress(");
    const std::string streamline = body(steam, "bool IsCodeAddressFromStreamlineModule(");
    const std::string shared = body(table, "inline bool TryGetModulePathFromCodeAddress(");
    const std::string ffx = body(table, "inline bool IsCodeAddressFromFFXFrameGenerationModule(");
    for (const std::string* fn : {&dx12, &streamline, &shared, &ffx}) {
        ASSERT_FALSE(fn->empty());
        EXPECT_EQ(fn->find("GetModuleFileNameA("), std::string::npos);
        EXPECT_EQ(fn->find("GetModuleHandleExA("), std::string::npos);
    }
}

}  // namespace
