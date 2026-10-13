#include <gtest/gtest.h>

#include "captureengine/sensors/sensor_snapshot_cache.h"
#include "elevationservice/service_internal.h"
#include "hook/overlay/overlay_layout_policy.h"

using namespace ce::hardware_sensors;

namespace {
HardwareSensorSnapshot Sample(uint32_t sequence = 7, uint64_t tickMs = 1000) {
    HardwareSensorSnapshot snapshot;
    snapshot.sequence = sequence;
    snapshot.receivedTickMs = tickMs;
    snapshot.cpuTemperature = {75.0f, true, "/cpu/0/temperature/0"};
    snapshot.gpuTemperature = {65.0f, true, "/gpu/0/temperature/0"};
    snapshot.cpuPackagePower = {70.0f, true, "/cpu/0/power/0"};
    snapshot.gpuPackagePower = {180.0f, true, "/gpu/0/power/0"};
    snapshot.gpuFan = {0.0f, true, "/gpu/0/fan/0"};
    snapshot.cpuCoreClock = {4400.0f, true, "/cpu/0/clock/0"};
    snapshot.gpuCoreClock = {2400.0f, true, "/gpu/0/clock/0"};
    snapshot.gpuMemoryClock = {17000.0f, true, "/gpu/0/clock/1"};
    snapshot.gpuVoltage = {0.89f, true, "/gpu/0/voltage/0"};
    snapshot.cpuMaxCoreClock = {4800.0f, true, "/cpu/0/clock/1"};
    return snapshot;
}
}  // namespace

TEST(SensorSnapshotCacheTest, DelayedPollingKeepsEveryValidatedReadingAndItsAcquisitionTime) {
    SensorSnapshotCache cache;
    cache.Store(Sample());
    EXPECT_EQ(cache.Status(true, 6000, 1000), SnapshotStatus::Current);
    for (const uint64_t now : {6001ull, 10000ull, 61000ull}) {
        ASSERT_EQ(cache.Status(true, now, 1000), SnapshotStatus::Delayed);
        const auto snapshot = cache.Read(true, now);
        EXPECT_EQ(snapshot.sequence, 7u);
        EXPECT_EQ(snapshot.receivedTickMs, 1000u);
        EXPECT_TRUE(snapshot.cpuTemperature.valid);
        EXPECT_FLOAT_EQ(snapshot.cpuTemperature.value, 75.0f);
        EXPECT_TRUE(snapshot.gpuTemperature.valid);
        EXPECT_TRUE(snapshot.cpuPackagePower.valid);
        EXPECT_TRUE(snapshot.gpuPackagePower.valid);
        EXPECT_TRUE(snapshot.gpuFan.valid);
        EXPECT_FLOAT_EQ(snapshot.gpuFan.value, 0.0f);
        EXPECT_TRUE(snapshot.cpuCoreClock.valid);
        EXPECT_TRUE(snapshot.gpuCoreClock.valid);
        EXPECT_TRUE(snapshot.gpuMemoryClock.valid);
        EXPECT_TRUE(snapshot.gpuVoltage.valid);
        EXPECT_TRUE(snapshot.cpuMaxCoreClock.valid);
    }
    EXPECT_EQ(cache.Status(true, 31000, 10000), SnapshotStatus::Current);
    EXPECT_EQ(cache.Status(true, 31001, 10000), SnapshotStatus::Delayed);
    EXPECT_TRUE(cache.Read(true, 31001).cpuTemperature.valid);
}

TEST(SensorSnapshotCacheTest, StartupDisconnectAndResetNeverInventOrResurrectValues) {
    SensorSnapshotCache cache;
    EXPECT_EQ(cache.Read(true, 1000).sequence, 0u);
    EXPECT_EQ(cache.Status(true, 1000, 1000), SnapshotStatus::Unavailable);
    cache.Store(Sample());
    EXPECT_EQ(cache.Read(false, 61000).sequence, 0u);
    EXPECT_EQ(cache.Status(false, 61000, 1000), SnapshotStatus::Unavailable);
    cache.Clear();
    EXPECT_EQ(cache.Read(true, 61000).sequence, 0u);
    EXPECT_FALSE(cache.Read(true, 61000).cpuTemperature.valid);
    cache.Store(Sample(8, 61000));
    EXPECT_EQ(cache.Read(true, 61000).sequence, 8u);
    EXPECT_EQ(cache.Status(true, 61000, 1000), SnapshotStatus::Current);
}

TEST(SensorSnapshotCacheTest, ExplicitUnreadableSamplesReplacePreviousValuesWithoutPerMetricHoldover) {
    SensorSnapshotCache cache;
    cache.Store(Sample());
    auto next = Sample(8, 10000);
    next.cpuTemperature = {};
    next.gpuFan = {};
    next.cpuCoreClock.value = 4600.0f;
    cache.Store(next);
    const auto snapshot = cache.Read(true, 16001);
    EXPECT_FALSE(snapshot.cpuTemperature.valid);
    EXPECT_FALSE(snapshot.gpuFan.valid);
    EXPECT_TRUE(snapshot.gpuTemperature.valid);
    EXPECT_FLOAT_EQ(snapshot.cpuCoreClock.value, 4600.0f);
    cache.Store({});  // Service reports no sample after a bridge error/restart.
    EXPECT_EQ(cache.Read(true, 16001).sequence, 0u);
}

TEST(SensorSnapshotCacheTest, RejectsMissingSequenceAndInvalidAcquisitionTimes) {
    SensorSnapshotCache cache;
    for (const auto& snapshot : {Sample(0), Sample(7, 0), Sample(7, 1001)}) {
        cache.Store(snapshot);
        EXPECT_EQ(cache.Read(true, 1000).sequence, 0u);
        EXPECT_EQ(cache.Status(true, 1000, 1000), SnapshotStatus::Unavailable);
    }
}

TEST(SensorSnapshotCacheTest, PrivilegedRoundTripDoesNotExpireOrFreshenAnOverdueSample) {
    SensorSnapshotCache service;
    SensorSnapshotCache broker;
    service.Store(Sample());
    for (const uint64_t now : {6001ull, 10000ull, 61000ull}) {
        const auto wire = ce::elevation::MakeSensorSample(service.Read(true, now));
        BridgeMessage message;
        ASSERT_TRUE(ParseBridgeMessage(wire.line.data(), message));
        message.snapshot.receivedTickMs = wire.sampledTickMs;
        broker.Store(std::move(message.snapshot));
        const auto snapshot = broker.Read(true, now);
        EXPECT_EQ(snapshot.sequence, 7u);
        EXPECT_EQ(snapshot.receivedTickMs, 1000u);
        EXPECT_EQ(broker.Status(true, now, 1000), SnapshotStatus::Delayed);
        char cpu[80]{};
        char gpu[80]{};
        ce::overlay_layout::FormatCpuMetricsValue(cpu, sizeof(cpu), 10, 100, snapshot.cpuTemperature.valid,
                                                  snapshot.cpuTemperature.value, snapshot.cpuPackagePower.valid,
                                                  snapshot.cpuPackagePower.value);
        ce::overlay_layout::FormatGpuMetricsValue(gpu, sizeof(gpu), true, 4, snapshot.gpuTemperature.valid,
                                                  snapshot.gpuTemperature.value, snapshot.gpuPackagePower.valid,
                                                  snapshot.gpuPackagePower.value, snapshot.gpuFan.valid,
                                                  snapshot.gpuFan.value);
        EXPECT_STREQ(cpu, "10% (100%)  75 C  70 W");
        EXPECT_STREQ(gpu, "4%  65 C  180 W  0 RPM");
        ce::overlay_layout::FormatCpuClocksValue(cpu, sizeof(cpu), snapshot.cpuCoreClock.valid,
                                                 snapshot.cpuCoreClock.value, snapshot.cpuMaxCoreClock.valid,
                                                 snapshot.cpuMaxCoreClock.value);
        ce::overlay_layout::FormatGpuClocksValue(
            gpu, sizeof(gpu), snapshot.gpuCoreClock.valid, snapshot.gpuCoreClock.value, snapshot.gpuMemoryClock.valid,
            snapshot.gpuMemoryClock.value, snapshot.gpuVoltage.valid, snapshot.gpuVoltage.value);
        EXPECT_STREQ(cpu, "4400 MHz (4800 MHz)");
        EXPECT_STREQ(gpu, "2400 MHz  17000 MHz  0.890 V");
    }
    auto recovered = Sample(8, 61001);
    recovered.cpuTemperature.value = 72.0f;
    service.Store(recovered);
    const auto wire = ce::elevation::MakeSensorSample(service.Read(true, 61001));
    BridgeMessage message;
    ASSERT_TRUE(ParseBridgeMessage(wire.line.data(), message));
    message.snapshot.receivedTickMs = wire.sampledTickMs;
    broker.Store(std::move(message.snapshot));
    EXPECT_EQ(broker.Status(true, 61001, 1000), SnapshotStatus::Current);
    EXPECT_FLOAT_EQ(broker.Read(true, 61001).cpuTemperature.value, 72.0f);
}
