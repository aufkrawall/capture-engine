#include <gtest/gtest.h>

#include "captureengine/sensors/sensor_snapshot_diagnostics.h"

using namespace ce::hardware_sensors;

TEST(SensorSnapshotDiagnosticsTest, LogsDelayAndRecoveryWithoutRotatingValueOrSequenceNoise) {
    SensorSnapshotCache cache;
    SensorSnapshotDiagnostics diagnostics;
    auto observation = diagnostics.Observe(cache, true, 1000, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.status, SnapshotStatus::Unavailable);
    EXPECT_FALSE(diagnostics.Observe(cache, true, 2000, 1000).verdict.log);

    HardwareSensorSnapshot snapshot;
    snapshot.sequence = 1;
    snapshot.receivedTickMs = 1000;
    snapshot.cpuTemperature = {75.0f, true, "/cpu/0/temperature/0"};
    cache.Store(snapshot);
    observation = diagnostics.Observe(cache, true, 6000, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.verdict.suppressed, 1u);
    EXPECT_EQ(observation.status, SnapshotStatus::Current);
    EXPECT_EQ(observation.readableMask, 1u);
    observation = diagnostics.Observe(cache, true, 6001, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.status, SnapshotStatus::Delayed);
    EXPECT_EQ(observation.ageMs, 5001u);
    EXPECT_EQ(observation.readableMask, 1u);
    EXPECT_FALSE(diagnostics.Observe(cache, true, 7000, 1000).verdict.log);
    EXPECT_FALSE(diagnostics.Observe(cache, true, 61000, 1000).verdict.log);

    snapshot.sequence = 2;
    snapshot.receivedTickMs = 61000;
    snapshot.cpuTemperature.value = 72.0f;
    cache.Store(snapshot);
    observation = diagnostics.Observe(cache, true, 61001, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.status, SnapshotStatus::Current);
    EXPECT_EQ(observation.ageMs, 1u);
    EXPECT_EQ(observation.verdict.suppressed, 2u);
    snapshot.sequence = 3;
    snapshot.receivedTickMs = 62000;
    cache.Store(snapshot);
    EXPECT_FALSE(diagnostics.Observe(cache, true, 62001, 1000).verdict.log);

    snapshot.cpuTemperature = {};
    cache.Store(snapshot);
    observation = diagnostics.Observe(cache, true, 62001, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.readableMask, 0u);
    EXPECT_EQ(observation.status, SnapshotStatus::Current);
    cache.Clear();
    observation = diagnostics.Observe(cache, true, 62001, 1000);
    EXPECT_TRUE(observation.verdict.log);
    EXPECT_EQ(observation.status, SnapshotStatus::Unavailable);
    EXPECT_EQ(observation.ageMs, 0u);
    EXPECT_TRUE(diagnostics.Observe(cache, false, 62001, 1000).verdict.log);
    EXPECT_FALSE(diagnostics.Observe(cache, false, 62002, 1000).verdict.log);
}
