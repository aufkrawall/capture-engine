#include <gtest/gtest.h>

#include "common/logging/log_meter.h"

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
