#include <gtest/gtest.h>

#include "../common/log_meter.h"

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
