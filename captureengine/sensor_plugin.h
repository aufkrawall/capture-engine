#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct HardwareSensorsConfig;

namespace ce::hardware_sensors {

struct SensorValue {
    float value = 0.0f;
    bool valid = false;
    std::string identifier;
};

struct HardwareSensorSnapshot {
    SensorValue cpuTemperature;
    SensorValue gpuTemperature;
    SensorValue cpuPackagePower;
    SensorValue gpuPackagePower;
    SensorValue gpuFan;
    uint32_t sequence = 0;
    uint64_t receivedTickMs = 0;
};

enum class BridgeMessageKind : uint8_t {
    Invalid = 0,
    Ready,
    Sample,
    Error,
};

struct BridgeMessage {
    BridgeMessageKind kind = BridgeMessageKind::Invalid;
    std::string detail;
    HardwareSensorSnapshot snapshot;
};

// Parses the deliberately small, tab-delimited protocol emitted by the
// first-party PowerShell bridge. Every field is bounded before it reaches IPC.
bool ParseBridgeMessage(std::string_view line, BridgeMessage& message);
bool IsSnapshotFresh(const HardwareSensorSnapshot& snapshot, uint64_t nowTickMs, uint32_t pollIntervalMs);

class LibreHardwareMonitorPlugin {
public:
    explicit LibreHardwareMonitorPlugin(const HardwareSensorsConfig& config);
    ~LibreHardwareMonitorPlugin();

    LibreHardwareMonitorPlugin(const LibreHardwareMonitorPlugin&) = delete;
    LibreHardwareMonitorPlugin& operator=(const LibreHardwareMonitorPlugin&) = delete;

    bool Start();
    void Poll();
    HardwareSensorSnapshot GetSnapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ce::hardware_sensors
