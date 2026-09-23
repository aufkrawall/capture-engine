#include <gtest/gtest.h>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../common/crash_symbol_store.h"

namespace fs = std::filesystem;

namespace {

class CrashSymbolStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        wchar_t tempPath[MAX_PATH] = {};
        ASSERT_GT(GetTempPathW(MAX_PATH, tempPath), 0u);
        root_ = fs::path(tempPath) / (L"ce_symbol_store_test_" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(root_, ec);
        ASSERT_TRUE(fs::create_directories(root_ / L"installed"));
        store_ = ce::crash_symbols::StoreDirForLogsRoot(root_ / L"logs");
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path WriteInstalled(const wchar_t* name, const std::string& contents) {
        const fs::path path = root_ / L"installed" / name;
        std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
        return path;
    }

    static std::string ReadAll(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    fs::path root_;
    fs::path store_;
};

}  // namespace

// Regression: every CaptureEngine start copied the installed PDBs and binaries
// (~180 MB) into the new session twice, and twenty sessions are retained. Two
// sessions of the same build must now share one stored copy.
TEST_F(CrashSymbolStoreTest, SessionsOfOneBuildShareOneStoredCopy) {
    const fs::path pdb = WriteInstalled(L"capture_hook_x64.pdb", std::string(4096, 'a'));
    const fs::path sessionA = root_ / L"logs" / L"20260923_100000" / L"symbols" / L"capture_hook_x64.pdb";
    const fs::path sessionAMirror =
        root_ / L"logs" / L"20260923_100000" / L"symbols" / L"captureengine" / L"capture_hook_x64.pdb";
    const fs::path sessionB = root_ / L"logs" / L"20260923_110000" / L"symbols" / L"capture_hook_x64.pdb";

    const auto first = ce::crash_symbols::PlaceArtifact(pdb, sessionA, store_);
    const auto mirror = ce::crash_symbols::PlaceArtifact(pdb, sessionAMirror, store_);
    const auto second = ce::crash_symbols::PlaceArtifact(pdb, sessionB, store_);
    ASSERT_TRUE(first.placed);
    ASSERT_TRUE(mirror.placed);
    ASSERT_TRUE(second.placed);
    EXPECT_EQ(ReadAll(sessionA), std::string(4096, 'a'));
    EXPECT_EQ(ReadAll(sessionB), std::string(4096, 'a'));

    if (!first.linked) {
        GTEST_SKIP() << "temp volume has no hard links; the copy fallback was exercised instead";
    }
    EXPECT_TRUE(mirror.linked);
    EXPECT_TRUE(second.linked);
    // Store + three session entries, one set of bytes on disk.
    EXPECT_EQ(ce::crash_symbols::QueryLinkCount(store_ / L"capture_hook_x64.pdb"), 4u);

    const auto again = ce::crash_symbols::PlaceArtifact(pdb, sessionA, store_);
    EXPECT_TRUE(again.alreadyPresent);
}

// A newer build must never change what an older session archived: the store is
// refreshed by rename, so the older links keep the older bytes.
TEST_F(CrashSymbolStoreTest, NewBuildLeavesOlderSessionBytesUntouched) {
    const fs::path pdb = WriteInstalled(L"captureengine.pdb", "build-1");
    const fs::path oldSession = root_ / L"logs" / L"20260923_100000" / L"symbols" / L"captureengine.pdb";
    ASSERT_TRUE(ce::crash_symbols::PlaceArtifact(pdb, oldSession, store_).placed);

    WriteInstalled(L"captureengine.pdb", "build-2-with-different-size");
    const fs::path newSession = root_ / L"logs" / L"20260923_120000" / L"symbols" / L"captureengine.pdb";
    ASSERT_TRUE(ce::crash_symbols::PlaceArtifact(pdb, newSession, store_).placed);

    EXPECT_EQ(ReadAll(oldSession), "build-1");
    EXPECT_EQ(ReadAll(newSession), "build-2-with-different-size");
}

// Deleting old sessions (the 20-session retention) must eventually free the
// stored copies they alone referenced, while a still-linked file survives.
TEST_F(CrashSymbolStoreTest, PruneRemovesOnlyUnreferencedStoreFiles) {
    const fs::path kept = WriteInstalled(L"kept.pdb", "kept");
    const fs::path dropped = WriteInstalled(L"dropped.pdb", "dropped");
    const fs::path liveSession = root_ / L"logs" / L"20260923_130000" / L"symbols";
    const fs::path oldSession = root_ / L"logs" / L"20260923_090000" / L"symbols";
    const auto keptResult = ce::crash_symbols::PlaceArtifact(kept, liveSession / L"kept.pdb", store_);
    const auto droppedResult = ce::crash_symbols::PlaceArtifact(dropped, oldSession / L"dropped.pdb", store_);
    ASSERT_TRUE(keptResult.placed);
    ASSERT_TRUE(droppedResult.placed);
    if (!keptResult.linked || !droppedResult.linked) {
        GTEST_SKIP() << "temp volume has no hard links";
    }

    fs::remove_all(root_ / L"logs" / L"20260923_090000");
    EXPECT_EQ(ce::crash_symbols::PruneUnreferencedStoreFiles(store_), 1u);
    EXPECT_TRUE(fs::exists(store_ / L"kept.pdb"));
    EXPECT_FALSE(fs::exists(store_ / L"dropped.pdb"));
    EXPECT_EQ(ReadAll(liveSession / L"kept.pdb"), "kept");
}

TEST_F(CrashSymbolStoreTest, WithoutAStoreArtifactsAreCopied) {
    const fs::path pdb = WriteInstalled(L"plain.pdb", "plain");
    const fs::path destination = root_ / L"logs" / L"session" / L"plain.pdb";
    const auto result = ce::crash_symbols::PlaceArtifact(pdb, destination, fs::path());
    EXPECT_TRUE(result.placed);
    EXPECT_FALSE(result.linked);
    EXPECT_EQ(ReadAll(destination), "plain");
    EXPECT_EQ(ce::crash_symbols::PruneUnreferencedStoreFiles(fs::path()), 0u);
}
