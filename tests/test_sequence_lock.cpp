#include <gtest/gtest.h>
#include "../common/sequence_lock.h"

namespace {
struct TestSequenceConfig {
    uint32_t frameLimit;
    uint32_t mode;
};
}  // namespace

TEST(SequenceLockTest, ReadReturnsLatestWrittenValue) {
    ce::SequenceLock<TestSequenceConfig> lock;

    TestSequenceConfig input{144, 2};
    lock.Write(input);

    TestSequenceConfig output{};
    ASSERT_TRUE(lock.Read(output));
    EXPECT_EQ(output.frameLimit, 144u);
    EXPECT_EQ(output.mode, 2u);
}

TEST(SequenceLockTest, ReadIfChangedTracksStableVersionTransitions) {
    ce::VersionedConfig<TestSequenceConfig> config;

    TestSequenceConfig output{};
    uint32_t version = 0;

    EXPECT_FALSE(config.ReadIfChanged(output, version));

    config.Write(TestSequenceConfig{60, 1});
    ASSERT_TRUE(config.ReadIfChanged(output, version));
    EXPECT_EQ(output.frameLimit, 60u);
    EXPECT_EQ(output.mode, 1u);
    EXPECT_NE(version, 0u);

    EXPECT_FALSE(config.ReadIfChanged(output, version));

    config.Write(TestSequenceConfig{120, 3});
    ASSERT_TRUE(config.ReadIfChanged(output, version));
    EXPECT_EQ(output.frameLimit, 120u);
    EXPECT_EQ(output.mode, 3u);
}
