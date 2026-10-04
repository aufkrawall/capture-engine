#include <gtest/gtest.h>

#include "common/logging/log_meter.h"
#include "hook/streamline/streamline_ui_tag_log.h"
#include <barrier>
#include <thread>

namespace log_meter = ce::log_meter;

TEST(LogMeterTest, FirstBurstIsAlwaysLogged) {
    EXPECT_TRUE(log_meter::ShouldLogCadence(1, 5, 100));
    EXPECT_TRUE(log_meter::ShouldLogCadence(2, 5, 100));
    EXPECT_TRUE(log_meter::ShouldLogCadence(5, 5, 100));
    EXPECT_FALSE(log_meter::ShouldLogCadence(6, 5, 100));
}

TEST(LogMeterTest, ZeroFirstBurstSkipsEarlyCalls) {
    EXPECT_FALSE(log_meter::ShouldLogCadence(1, 0, 100));
    EXPECT_TRUE(log_meter::ShouldLogCadence(100, 0, 100));
    EXPECT_FALSE(log_meter::ShouldLogCadence(101, 0, 100));
}

TEST(LogMeterTest, StrideHeartbeatCadence) {
    EXPECT_TRUE(log_meter::ShouldLogCadence(10, 3, 10));
    EXPECT_TRUE(log_meter::ShouldLogCadence(20, 3, 10));
    EXPECT_FALSE(log_meter::ShouldLogCadence(15, 3, 10));
    EXPECT_FALSE(log_meter::ShouldLogCadence(19, 3, 10));
}

TEST(LogMeterTest, StrideOneAndZero) {
    // Stride 1 logs every call after the burst.
    EXPECT_TRUE(log_meter::ShouldLogCadence(1, 2, 1));
    EXPECT_TRUE(log_meter::ShouldLogCadence(7, 2, 1));
    // Stride 0 logs every call.
    EXPECT_TRUE(log_meter::ShouldLogCadence(1, 0, 0));
    EXPECT_TRUE(log_meter::ShouldLogCadence(42, 0, 0));
}

TEST(LogMeterTest, CallIndexZeroBehavesAsFirstCall) {
    EXPECT_TRUE(log_meter::ShouldLogCadence(0, 3, 100));
}

TEST(LogMeterChangeGateTest, LogsFirstLineAndEveryChangeWithTheSwallowedRepeatCount) {
    log_meter::ChangeGate gate;
    const uint64_t a = log_meter::FieldKey(1, true);
    const uint64_t b = log_meter::FieldKey(2, true);

    auto first = gate.Observe(a);
    EXPECT_TRUE(first.log);
    EXPECT_EQ(first.suppressed, 0u);
    EXPECT_FALSE(gate.Observe(a).log);
    EXPECT_FALSE(gate.Observe(a).log);
    auto changed = gate.Observe(b);
    EXPECT_TRUE(changed.log);
    EXPECT_EQ(changed.suppressed, 2u) << "the two identical repeats are reported, not lost";
    auto back = gate.Observe(a);
    EXPECT_TRUE(back.log);
    EXPECT_EQ(back.suppressed, 0u);
}

TEST(LogMeterChangeGateTest, HeartbeatLogsAnUnchangedLineOncePerInterval) {
    log_meter::ChangeGate gate(1000);
    const uint64_t key = log_meter::FieldKey(7u);
    EXPECT_TRUE(gate.Observe(key, 0).log);
    EXPECT_FALSE(gate.Observe(key, 500).log);
    EXPECT_FALSE(gate.Observe(key, 999).log);
    auto beat = gate.Observe(key, 1000);
    EXPECT_TRUE(beat.log);
    EXPECT_EQ(beat.suppressed, 2u);
    EXPECT_FALSE(gate.Observe(key, 1500).log);
}

TEST(LogMeterChangeGateTest, FieldKeyDistinguishesFieldValuesAndOrder) {
    EXPECT_NE(log_meter::FieldKey(1, 2), log_meter::FieldKey(2, 1));
    EXPECT_NE(log_meter::FieldKey(false), log_meter::FieldKey(true));
    int value = 0;
    EXPECT_NE(log_meter::FieldKey(&value), log_meter::FieldKey(static_cast<int*>(nullptr)));
    EXPECT_EQ(log_meter::FieldKey(3u, 4.5), log_meter::FieldKey(3u, 4.5));
}

TEST(LogMeterChangeGateTest, SuppressedNoteIsEmptyWithoutRepeats) {
    EXPECT_STREQ(log_meter::SuppressedNote(0).c_str(), "");
    EXPECT_STREQ(log_meter::SuppressedNote(12).c_str(), " (+12 unchanged)");
}

TEST(LogMeterKeyedOnceTest, ReportsEachKeyOnceAndOverflowsTowardLogging) {
    log_meter::KeyedOnce<2> once;
    EXPECT_TRUE(once.FirstTime(10));
    EXPECT_FALSE(once.FirstTime(10));
    EXPECT_TRUE(once.FirstTime(0));
    EXPECT_FALSE(once.FirstTime(0));
    EXPECT_TRUE(once.FirstTime(11)) << "a full set must not hide an unseen key";
    EXPECT_TRUE(once.FirstTime(11));
    EXPECT_FALSE(once.FirstTime(10));
}

// 0.1.6951 GTA session: slSetTagForFrame and slEvaluateFeature alternated through one ChangeGate keyed by
// api+tags, so every call was a "change" - 102k UI-tag lines in three minutes and 1412 lines lost.
TEST(LogMeterStreamChangeGateTest, InterleavedStreamsAreMeteredPerStream) {
    const uint64_t setTagForFrame = log_meter::FieldKey(1);
    const uint64_t evaluateFeature = log_meter::FieldKey(2);
    const uint64_t tagsA = log_meter::FieldKey(10);
    const uint64_t tagsB = log_meter::FieldKey(20);

    log_meter::ChangeGate shared;
    int sharedLogged = 0;
    log_meter::StreamChangeGate<16> perStream;
    int perStreamLogged = 0;
    for (int frame = 0; frame < 100; ++frame) {
        sharedLogged += shared.Observe(log_meter::FieldKey(setTagForFrame, tagsA)).log ? 1 : 0;
        sharedLogged += shared.Observe(log_meter::FieldKey(evaluateFeature, tagsB)).log ? 1 : 0;
        perStreamLogged += perStream.Observe(setTagForFrame, tagsA).log ? 1 : 0;
        perStreamLogged += perStream.Observe(evaluateFeature, tagsB).log ? 1 : 0;
    }
    EXPECT_EQ(sharedLogged, 200) << "one gate for two interleaved streams logs every call";
    EXPECT_EQ(perStreamLogged, 2);

    const auto changed = perStream.Observe(setTagForFrame, tagsB);
    EXPECT_TRUE(changed.log);
    EXPECT_EQ(changed.suppressed, 99u) << "a stream reports only its own swallowed repeats";
    EXPECT_FALSE(perStream.Observe(evaluateFeature, tagsB).log);
}

TEST(LogMeterStreamChangeGateTest, SaturationLogsUnknownStreamsWithoutEvictingOwners) {
    log_meter::StreamChangeGate<1> gate;
    const uint64_t key = log_meter::FieldKey(7);
    EXPECT_TRUE(gate.Observe(100, key).log);
    EXPECT_FALSE(gate.Observe(100, key).log);
    EXPECT_TRUE(gate.Observe(200, key).log) << "an overflow stream must always log";
    EXPECT_TRUE(gate.Observe(200, key).log);
    EXPECT_FALSE(gate.Observe(100, key).log) << "the original owner's gate must remain intact";
    EXPECT_TRUE(gate.Observe(0, key).log) << "stream 0 is a stream, not an unowned slot";
    EXPECT_TRUE(gate.Observe(0, key).log);
    const auto changed = gate.Observe(100, key + 1);
    EXPECT_TRUE(changed.log);
    EXPECT_EQ(changed.suppressed, 2u);
}

TEST(LogMeterStreamChangeGateTest, TwoCollidingStreamsKeepIndependentRepeatCounts) {
    log_meter::StreamChangeGate<2> gate;
    // Both raw stream keys hash to slot 0 with this capacity.
    const uint64_t streams[] = {0, 2};
    for (uint64_t stream : streams)
        EXPECT_TRUE(gate.Observe(stream, 7).log);
    for (int repeat = 0; repeat < 100; ++repeat) {
        for (uint64_t stream : streams)
            EXPECT_FALSE(gate.Observe(stream, 7).log);
    }
    for (uint64_t stream : streams) {
        const auto changed = gate.Observe(stream, 8);
        EXPECT_TRUE(changed.log);
        EXPECT_EQ(changed.suppressed, 100u);
    }
}

TEST(LogMeterStreamChangeGateTest, ReservedOwnerKeyFailsTowardLogging) {
    log_meter::StreamChangeGate<1> gate;
    EXPECT_TRUE(gate.Observe(UINT64_MAX, 7).log);
    EXPECT_TRUE(gate.Observe(UINT64_MAX, 7).log);
    EXPECT_TRUE(gate.Observe(0, 7).log);
    EXPECT_FALSE(gate.Observe(0, 7).log);
    EXPECT_TRUE(gate.ObserveOrEvery(UINT64_MAX, 7, 1, 300).log);
}

TEST(LogMeterStreamChangeGateTest, ThreeCollidingStreamsUseFreeCapacityBeforeOverflowing) {
    log_meter::StreamChangeGate<4> gate;
    // All three raw stream keys hash to slot 0; a two-probe table overflows
    // the third stream even though half of its capacity remains unused.
    const uint64_t streams[] = {0, 3, 6};
    for (uint64_t stream : streams)
        EXPECT_TRUE(gate.Observe(stream, 7).log);
    for (int repeat = 0; repeat < 100; ++repeat) {
        for (uint64_t stream : streams)
            EXPECT_FALSE(gate.Observe(stream, 7).log);
    }
    for (uint64_t stream : streams)
        EXPECT_EQ(gate.Observe(stream, 8).suppressed, 100u);
}

TEST(LogMeterStreamChangeGateTest, ConcurrentCollisionsCannotSuppressOverflowOrResetOwners) {
    log_meter::StreamChangeGate<1> gate;
    ASSERT_TRUE(gate.Observe(100, 7).log);
    std::barrier start(3);
    uint32_t ownerLogged = 0;
    uint32_t overflowLogged = 0;
    std::thread owner([&] {
        start.arrive_and_wait();
        for (uint32_t i = 0; i < 10000; ++i)
            ownerLogged += gate.Observe(100, 7).log ? 1 : 0;
    });
    std::thread overflow([&] {
        start.arrive_and_wait();
        for (uint32_t i = 0; i < 10000; ++i)
            overflowLogged += gate.Observe(200, 7).log ? 1 : 0;
    });
    start.arrive_and_wait();
    owner.join();
    overflow.join();
    EXPECT_EQ(ownerLogged, 0u);
    EXPECT_EQ(overflowLogged, 10000u);
    EXPECT_EQ(gate.Observe(100, 8).suppressed, 10000u);
}

TEST(LogMeterStreamChangeGateTest, HeartbeatStrideCountsAllStreams) {
    log_meter::StreamChangeGate<16> gate;
    const uint64_t key = log_meter::FieldKey(3);
    int logged = 0;
    for (uint32_t call = 1; call <= 600; ++call) {
        logged += gate.ObserveOrEvery(call % 2, key, call, 300).log ? 1 : 0;
    }
    EXPECT_EQ(logged, 4) << "first line of each stream plus the heartbeats at calls 300 and 600";
}

TEST(LogMeterChangeGateTest, ForceLogsAnUnchangedLineWithItsSwallowedRepeats) {
    log_meter::ChangeGate gate;
    const uint64_t key = log_meter::FieldKey(5);
    EXPECT_TRUE(gate.Observe(key).log);
    EXPECT_FALSE(gate.Observe(key).log);
    EXPECT_FALSE(gate.Observe(key).log);
    auto forced = gate.Force(key);
    EXPECT_TRUE(forced.log);
    EXPECT_EQ(forced.suppressed, 2u);
    EXPECT_FALSE(gate.Observe(key).log);
}

// GTA V Enhanced tags 3, 4 and 6 resources in turn on one viewport every frame. Each call shape is its own
// stream, so the UI tag diagnostics log each shape once plus the every-300th heartbeat, not every call.
TEST(LogMeterTest, StreamlineUiTagCallShapesAreSeparateStreams) {
    ce::log_meter::StreamChangeGate<16> gate;
    const char* api = "slSetTagForFrame";
    const uint32_t shapes[] = {3, 4, 6};
    int logged = 0;
    for (uint32_t call = 1; call <= 900; ++call) {
        const uint32_t numTags = shapes[call % 3];
        const uint64_t stream = ce::streamline_ui_tag_log::Stream(api, UINT32_MAX, 0, numTags, 0);
        logged += gate.ObserveOrEvery(stream, ce::log_meter::FieldKey(numTags, 7u), call, 300) ? 1 : 0;
    }
    EXPECT_EQ(logged, 6);  // three first appearances, three heartbeats (calls 300, 600, 900)
}
