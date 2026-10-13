#pragma once

#include "sensor_plugin.h"
#include <utility>

namespace ce::hardware_sensors {

enum class SnapshotStatus { Unavailable, Current, Delayed };

// Acquisition age describes a polling delay, not whether a validated reading
// exists. Only the backend lifecycle or an explicit new sample invalidates it.
class SensorSnapshotCache {
public:
    void Store(HardwareSensorSnapshot snapshot) {
        snapshot_ = std::move(snapshot);
    }
    void Clear() {
        snapshot_ = {};
    }
    const HardwareSensorSnapshot& Current() const {
        return snapshot_;
    }

    HardwareSensorSnapshot Read(bool backendActive, uint64_t nowTickMs) const {
        return Available(backendActive, nowTickMs) ? snapshot_ : HardwareSensorSnapshot{};
    }

    SnapshotStatus Status(bool backendActive, uint64_t nowTickMs, uint32_t pollIntervalMs) const {
        if (!Available(backendActive, nowTickMs))
            return SnapshotStatus::Unavailable;
        return IsSnapshotFresh(snapshot_, nowTickMs, pollIntervalMs) ? SnapshotStatus::Current
                                                                     : SnapshotStatus::Delayed;
    }

private:
    bool Available(bool backendActive, uint64_t nowTickMs) const {
        return backendActive && snapshot_.sequence != 0 && snapshot_.receivedTickMs != 0 &&
               snapshot_.receivedTickMs <= nowTickMs;
    }

    HardwareSensorSnapshot snapshot_;
};

}  // namespace ce::hardware_sensors
