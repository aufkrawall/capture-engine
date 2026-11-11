#pragma once

#include "common/ipc/elevation_protocol.h"
#include "common/ipc/elevation_trace_lease.h"
#include "common/ipc/elevation_windows.h"
#include "captureengine/sensors/sensor_plugin.h"
#include "common/config/config.h"
#include "common/platform/display_flight_recorder.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

namespace ce::elevation {
std::wstring InstalledOwner();
bool AuthenticateClient(HANDLE pipe, const Hello& hello, const std::wstring& owner, Handle& controller);
HardwareSensorsConfig MakeSensorConfig(const SensorRequest& request);
// Operation::RestoreConfig: ERROR_SUCCESS, ERROR_FILE_EXISTS, or why the installation's config.ini was not created.
DWORD RestoreConfig();
SensorSample MakeSensorSample(const ce::hardware_sensors::HardwareSensorSnapshot& snapshot);

// What a client leases: Display adds the display-timing providers to the shared
// session; ProcessStart needs only the process provider every lease keeps on.
enum class TraceKind { Display, ProcessStart };

class TraceManager {
public:
    explicit TraceManager(std::wstring owner) : owner_(std::move(owner)) {}
    ~TraceManager();
    DWORD Acquire(TraceKind kind);
    void Release(TraceKind kind);
    bool ProcessStartAvailable();
    // The display flight recorder (service_flight_trace.cpp), leased by connections that hold the display lease and
    // asked for it; the last such lease, or the display lease's end, stops it.
    DWORD AcquireFlight();
    void ReleaseFlight();
    SealedDisplayTrace SealFlight();

private:
    DWORD StartSessionLocked();
    DWORD EnableDisplayLocked();
    void StopFlightLocked();
    void Stop();
    std::wstring owner_;
    std::mutex mutex_;
    TraceLeaseCounter leases_;
    uint64_t session_ = 0;
    bool processStartAvailable_ = false;
    Handle stop_;
    std::thread flush_;
    display_flight_recorder::Recorder flight_;
    uint32_t flightUsers_ = 0;
    uint32_t flightSeals_ = 0;
    uint64_t lastSealTickMs_ = 0;
};
}  // namespace ce::elevation
