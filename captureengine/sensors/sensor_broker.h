#pragma once

#include "captureengine/elevation/elevation_client.h"
#include "sensor_plugin.h"
#include "sensor_snapshot_cache.h"
#include "sensor_snapshot_diagnostics.h"
#include "common/config/config.h"
#include <functional>

namespace ce::hardware_sensors {
class SensorBrokerBackend {
public:
    SensorBrokerBackend(const HardwareSensorsConfig& config, std::function<void()> stopLocal)
        : config_(config),
          stopLocal_(std::move(stopLocal)) {}
    bool Start();
    void Poll();
    bool Connected() const {
        return client_.Connected();
    }
    HardwareSensorSnapshot Snapshot() const;

private:
    void PollSample();
    HardwareSensorsConfig config_;
    std::function<void()> stopLocal_;
    ce::elevation::Client client_;
    SensorSnapshotCache snapshot_;
    SensorSnapshotDiagnostics snapshotDiagnostics_;
    uint64_t nextAttempt_ = 0;
    bool driverPresent_ = false;
    bool enabled_ = false;
    bool failureLogged_ = false;
};
}  // namespace ce::hardware_sensors
