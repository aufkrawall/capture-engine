#include <gtest/gtest.h>

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
