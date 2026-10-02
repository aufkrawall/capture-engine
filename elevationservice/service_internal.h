#pragma once

#include "common/ipc/elevation_protocol.h"
#include "common/ipc/elevation_windows.h"
#include "captureengine/sensors/sensor_plugin.h"
#include "common/config/config.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>

namespace ce::elevation {
std::wstring InstalledOwner();
bool AuthenticateClient(HANDLE pipe, const Hello& hello, const std::wstring& owner, Handle& controller);
HardwareSensorsConfig MakeSensorConfig(const SensorRequest& request);
SensorSample MakeSensorSample(const ce::hardware_sensors::HardwareSensorSnapshot& snapshot);

class TraceManager {
public:
    explicit TraceManager(std::wstring owner) : owner_(std::move(owner)) {}
    ~TraceManager();
    DWORD Acquire();
    void Release();

private:
    void Stop();
    std::wstring owner_;
    std::mutex mutex_;
    uint32_t users_ = 0;
    uint64_t session_ = 0;
    Handle stop_;
    std::thread flush_;
};
}  // namespace ce::elevation
