#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../common/av_sync_latency_channel.h"

// The session A/V latency channel replaces a per-recording ~3.2 s render->loopback probe with a
// controller-lifetime table the disposable media children share. These cover the pure table logic
// that decides whether a later recording may reuse an earlier measurement; the Win32 mapping is
// exercised by the product build.

namespace {

using ce::av_sync::InitLatencyChannel;
using ce::av_sync::IsLatencyChannelCompatible;
using ce::av_sync::kLatencyChannelKeyCapacity;
using ce::av_sync::kLatencyChannelMagic;
using ce::av_sync::kLatencyChannelMaxEntries;
using ce::av_sync::kLatencyChannelVersion;
using ce::av_sync::LatencyChannelBlock;
using ce::av_sync::LatencyChannelKeyFits;
using ce::av_sync::LookupLatencyChannel;
using ce::av_sync::UpsertLatencyChannel;

// The block carries a std::atomic, so it is neither copyable nor safe to place on the stack of a
// test that wants a pristine one each time; allocate and initialize it explicitly.
std::unique_ptr<LatencyChannelBlock> MakeChannel() {
    auto block = std::make_unique<LatencyChannelBlock>();
    InitLatencyChannel(*block);
    return block;
}

// The real key shape: device id plus the format/period metadata that make a reconfigured endpoint
// a different entry.
std::string RealisticKey(int sampleRate = 192000) {
    return "{0.0.0.00000000}.{07565be3-1fc4-4d0b-88d0-0c9ad7273321}|sr" + std::to_string(sampleRate) +
           "|ch2|bits32|align8|mask3|period100000|min30000";
}

}  // namespace

TEST(AvSyncLatencyChannelTest, FreshChannelIsCompatibleAndEmpty) {
    const auto block = MakeChannel();
    EXPECT_TRUE(IsLatencyChannelCompatible(*block));
    EXPECT_EQ(block->magic, kLatencyChannelMagic);
    EXPECT_EQ(block->version, kLatencyChannelVersion);
    EXPECT_EQ(block->structSize, static_cast<uint32_t>(sizeof(LatencyChannelBlock)));
    EXPECT_EQ(block->entryCount, 0u);
    EXPECT_EQ(block->sequence.load(), 0u);

    double latencyMs = -1.0;
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), &latencyMs));
    EXPECT_DOUBLE_EQ(latencyMs, -1.0);
}

// The regression this whole channel exists for: the second media process must not re-probe.
TEST(AvSyncLatencyChannelTest, MeasurementSurvivesForTheNextMediaProcess) {
    const auto block = MakeChannel();
    const std::string key = RealisticKey();

    ASSERT_TRUE(UpsertLatencyChannel(*block, key, 28.597));

    double latencyMs = 0.0;
    ASSERT_TRUE(LookupLatencyChannel(*block, key, &latencyMs));
    EXPECT_DOUBLE_EQ(latencyMs, 28.597);
    EXPECT_EQ(block->entryCount, 1u);
    EXPECT_EQ(block->sequence.load() % 2, 0u) << "the channel must be left in a stable (even) state";
}

TEST(AvSyncLatencyChannelTest, UpsertReplacesTheSameKeyInPlace) {
    const auto block = MakeChannel();
    const std::string key = RealisticKey();

    ASSERT_TRUE(UpsertLatencyChannel(*block, key, 28.597));
    ASSERT_TRUE(UpsertLatencyChannel(*block, key, 30.399));

    double latencyMs = 0.0;
    ASSERT_TRUE(LookupLatencyChannel(*block, key, &latencyMs));
    EXPECT_DOUBLE_EQ(latencyMs, 30.399);
    EXPECT_EQ(block->entryCount, 1u) << "a re-measurement of one endpoint must not grow the table";
}

// A reconfigured endpoint is a different key, so it must miss rather than inherit a latency that
// was measured for another format.
TEST(AvSyncLatencyChannelTest, DifferentEndpointConfigurationMisses) {
    const auto block = MakeChannel();
    ASSERT_TRUE(UpsertLatencyChannel(*block, RealisticKey(192000), 28.597));

    double latencyMs = -1.0;
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(48000), &latencyMs));
    EXPECT_DOUBLE_EQ(latencyMs, -1.0);
}

TEST(AvSyncLatencyChannelTest, DistinctEndpointsCoexistUpToCapacity) {
    const auto block = MakeChannel();
    for (uint32_t i = 0; i < kLatencyChannelMaxEntries; ++i) {
        ASSERT_TRUE(UpsertLatencyChannel(*block, "endpoint" + std::to_string(i), 10.0 + i));
    }
    EXPECT_EQ(block->entryCount, kLatencyChannelMaxEntries);
    for (uint32_t i = 0; i < kLatencyChannelMaxEntries; ++i) {
        double latencyMs = 0.0;
        ASSERT_TRUE(LookupLatencyChannel(*block, "endpoint" + std::to_string(i), &latencyMs)) << i;
        EXPECT_DOUBLE_EQ(latencyMs, 10.0 + i);
    }
}

// Overflow costs one re-probe; it must never corrupt the table or resurrect a dropped key.
TEST(AvSyncLatencyChannelTest, OverflowDropsTheOldestEntryAndKeepsTheRest) {
    const auto block = MakeChannel();
    for (uint32_t i = 0; i < kLatencyChannelMaxEntries; ++i) {
        ASSERT_TRUE(UpsertLatencyChannel(*block, "endpoint" + std::to_string(i), 10.0 + i));
    }
    ASSERT_TRUE(UpsertLatencyChannel(*block, "newest", 99.5));

    EXPECT_EQ(block->entryCount, kLatencyChannelMaxEntries);
    EXPECT_FALSE(LookupLatencyChannel(*block, "endpoint0", nullptr));
    for (uint32_t i = 1; i < kLatencyChannelMaxEntries; ++i) {
        double latencyMs = 0.0;
        ASSERT_TRUE(LookupLatencyChannel(*block, "endpoint" + std::to_string(i), &latencyMs)) << i;
        EXPECT_DOUBLE_EQ(latencyMs, 10.0 + i);
    }
    double newest = 0.0;
    ASSERT_TRUE(LookupLatencyChannel(*block, "newest", &newest));
    EXPECT_DOUBLE_EQ(newest, 99.5);
}

// Truncating a key could hand a different endpoint's latency back, which would silently
// mis-correct A/V sync. Refusing the key only costs a re-probe.
TEST(AvSyncLatencyChannelTest, OversizedKeyIsRefusedNotTruncated) {
    const auto block = MakeChannel();
    const std::string oversized(kLatencyChannelKeyCapacity, 'k');
    const std::string maximum(kLatencyChannelKeyCapacity - 1, 'k');

    EXPECT_FALSE(LatencyChannelKeyFits(oversized));
    EXPECT_FALSE(UpsertLatencyChannel(*block, oversized, 25.0));
    EXPECT_EQ(block->entryCount, 0u);
    EXPECT_FALSE(LookupLatencyChannel(*block, oversized, nullptr));

    EXPECT_TRUE(LatencyChannelKeyFits(maximum));
    EXPECT_TRUE(UpsertLatencyChannel(*block, maximum, 25.0));
    double latencyMs = 0.0;
    ASSERT_TRUE(LookupLatencyChannel(*block, maximum, &latencyMs));
    EXPECT_DOUBLE_EQ(latencyMs, 25.0);
}

TEST(AvSyncLatencyChannelTest, EmptyKeyIsRefused) {
    const auto block = MakeChannel();
    EXPECT_FALSE(LatencyChannelKeyFits(""));
    EXPECT_FALSE(UpsertLatencyChannel(*block, "", 25.0));
    EXPECT_FALSE(LookupLatencyChannel(*block, "", nullptr));
    EXPECT_EQ(block->entryCount, 0u);
}

// An incompatible or uninitialized block must fail closed in both directions: the child probes,
// which is always correct, only slower.
TEST(AvSyncLatencyChannelTest, IncompatibleBlockIsNeverTrusted) {
    const auto block = MakeChannel();
    ASSERT_TRUE(UpsertLatencyChannel(*block, RealisticKey(), 28.597));

    block->magic = kLatencyChannelMagic ^ 0xFFu;
    EXPECT_FALSE(IsLatencyChannelCompatible(*block));
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), nullptr));
    EXPECT_FALSE(UpsertLatencyChannel(*block, RealisticKey(), 30.0));

    block->magic = kLatencyChannelMagic;
    block->version = kLatencyChannelVersion + 1;
    EXPECT_FALSE(IsLatencyChannelCompatible(*block));
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), nullptr));

    block->version = kLatencyChannelVersion;
    block->structSize = static_cast<uint32_t>(sizeof(LatencyChannelBlock)) + 1;
    EXPECT_FALSE(IsLatencyChannelCompatible(*block));

    block->structSize = static_cast<uint32_t>(sizeof(LatencyChannelBlock));
    block->entryCount = static_cast<uint32_t>(kLatencyChannelMaxEntries) + 1;
    EXPECT_FALSE(IsLatencyChannelCompatible(*block)) << "an out-of-range count must not be walked";
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), nullptr));
}

// A read that observes a write in progress must report a miss rather than partial data.
TEST(AvSyncLatencyChannelTest, WriteInProgressReadsAsAMiss) {
    const auto block = MakeChannel();
    ASSERT_TRUE(UpsertLatencyChannel(*block, RealisticKey(), 28.597));
    ASSERT_TRUE(LookupLatencyChannel(*block, RealisticKey(), nullptr));

    block->sequence.fetch_add(1);  // odd: a writer is mid-update
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), nullptr));

    block->sequence.fetch_add(1);  // even again
    EXPECT_TRUE(LookupLatencyChannel(*block, RealisticKey(), nullptr));
}

// A corrupt entry whose key ran past the buffer must be skipped, not read off the end.
TEST(AvSyncLatencyChannelTest, UnterminatedEntryKeyIsSkipped) {
    const auto block = MakeChannel();
    ASSERT_TRUE(UpsertLatencyChannel(*block, RealisticKey(), 28.597));
    block->entries[0].key[kLatencyChannelKeyCapacity - 1] = 'x';
    EXPECT_FALSE(LookupLatencyChannel(*block, RealisticKey(), nullptr));
}
