#include <gtest/gtest.h>

#include <windows.h>

#include <fstream>

#include "scoped_test_directory.h"

TEST(ScopedTestDirectoryTest, EveryLifetimeOwnsADistinctDirectoryAndPreservesOtherLifetimes) {
    std::filesystem::path firstPath;
    std::filesystem::path secondPath;
    {
        ce::test_support::ScopedTestDirectory first("ce_scoped_directory_test");
        firstPath = first.path();
        std::ofstream(firstPath / "owned.txt") << "first";
        {
            ce::test_support::ScopedTestDirectory second("ce_scoped_directory_test");
            secondPath = second.path();
            EXPECT_NE(firstPath, secondPath);
            EXPECT_TRUE(std::filesystem::is_directory(secondPath));
        }
        EXPECT_FALSE(std::filesystem::exists(secondPath));
        EXPECT_TRUE(std::filesystem::exists(firstPath / "owned.txt"));
    }
    EXPECT_FALSE(std::filesystem::exists(firstPath));
}

// Regression: fixtures that deleted and recreated one fixed directory per test failed with ACCESS_DENIED
// (CrashSymbolStoreTest, gate 0.1.7517) while another process still held a handle on the deleted directory.
// A handle held across the teardown must never block the next test's directory.
TEST(ScopedTestDirectoryTest, AHandleHeldOnADeletedDirectoryNeverBlocksTheNextOne) {
    std::filesystem::path firstPath;
    HANDLE held = INVALID_HANDLE_VALUE;
    {
        ce::test_support::ScopedTestDirectory first("ce_scoped_directory_held");
        firstPath = first.path();
        std::ofstream(firstPath / "scanned.bin") << "contents";
        held = CreateFileW(firstPath.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        ASSERT_NE(held, INVALID_HANDLE_VALUE);
    }
    // The old fixtures' next step. libc++'s remove_all deletes a directory with the legacy delete-on-close
    // disposition, so while another handle is open the name stays delete-pending and cannot be created again.
    std::error_code recreate;
    const bool recreated = std::filesystem::create_directory(firstPath, recreate);
    EXPECT_FALSE(recreated) << "the held directory was no longer delete-pending";
    EXPECT_TRUE(recreate == std::errc::permission_denied) << recreate.message();
    for (int test = 0; test < 4; ++test) {
        ce::test_support::ScopedTestDirectory next("ce_scoped_directory_held");
        EXPECT_NE(next.path(), firstPath);
        EXPECT_TRUE(std::filesystem::is_directory(next.path()));
        std::ofstream(next.path() / "scanned.bin") << "contents";
    }
    CloseHandle(held);
}
