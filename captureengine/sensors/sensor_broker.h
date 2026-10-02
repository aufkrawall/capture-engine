#pragma once

#include "captureengine/elevation/elevation_client.h"
#include "sensor_plugin.h"
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
    HardwareSensorsConfig config_;
    std::function<void()> stopLocal_;
    ce::elevation::Client client_;
    HardwareSensorSnapshot snapshot_;
    uint64_t nextAttempt_ = 0;
    bool driverPresent_ = false;
    bool enabled_ = false;
    bool failureLogged_ = false;
};
}  // namespace ce::hardware_sensors
