#pragma once

#include "sensor_snapshot_cache.h"
#include "common/logging/log_meter.h"
#include "common/logging/logging.h"
#include <iterator>

namespace ce::hardware_sensors {

class SensorSnapshotDiagnostics {
public:
    struct Observation {
        SnapshotStatus status = SnapshotStatus::Unavailable;
        uint64_t ageMs = 0;
        uint32_t readableMask = 0;
        ce::log_meter::ChangeGate::Verdict verdict;
    };

    Observation Observe(const SensorSnapshotCache& cache, bool active, uint64_t nowMs, uint32_t pollMs) {
        Observation observation;
        observation.status = cache.Status(active, nowMs, pollMs);
        const auto& snapshot = cache.Current();
        if (observation.status != SnapshotStatus::Unavailable) {
            observation.ageMs = nowMs - snapshot.receivedTickMs;
            const SensorValue* values[] = {&snapshot.cpuTemperature,  &snapshot.gpuTemperature,
                                           &snapshot.cpuPackagePower, &snapshot.gpuPackagePower,
                                           &snapshot.gpuFan,          &snapshot.cpuCoreClock,
                                           &snapshot.gpuCoreClock,    &snapshot.gpuMemoryClock,
                                           &snapshot.gpuVoltage,      &snapshot.cpuMaxCoreClock};
            for (size_t index = 0; index < std::size(values); ++index) {
                if (values[index]->valid)
                    observation.readableMask |= 1u << index;
            }
        }
        observation.verdict =
            gate_.Observe(ce::log_meter::FieldKey(active, observation.status, observation.readableMask));
        return observation;
    }

    void Report(const SensorSnapshotCache& cache, bool active, uint64_t nowMs, uint32_t pollMs, const char* backend) {
        const auto observation = Observe(cache, active, nowMs, pollMs);
        if (!observation.verdict)
            return;
        const char* status = observation.status == SnapshotStatus::Delayed   ? "delayed"
                             : observation.status == SnapshotStatus::Current ? "current"
                                                                             : "unavailable";
        LogInfo(
            "[Sensors:LHM] Sample state: backend=%s active=%u status=%s sequence=%u ageMs=%llu pollMs=%u "
            "readable=0x%X retained=%u%s",
            backend, active ? 1u : 0u, status, cache.Current().sequence,
            static_cast<unsigned long long>(observation.ageMs), pollMs, observation.readableMask,
            observation.status == SnapshotStatus::Delayed ? 1u : 0u,
            ce::log_meter::SuppressedNote(observation.verdict.suppressed).text);
    }

private:
    ce::log_meter::ChangeGate gate_;
};

}  // namespace ce::hardware_sensors
