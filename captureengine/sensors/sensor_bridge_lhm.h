#pragma once

// Drives LibreHardwareMonitor's managed object model from native code, with no
// PowerShell and no first-party managed assembly. See clr_interop.h for the COM
// contracts this relies on.

#include <filesystem>
#include <memory>
#include <string>

#include "sensor_selection_policy.h"

namespace ce::hardware_sensors {

// One selector per metric, in kMetrics order. Each is "off", "auto", or an
// exact LibreHardwareMonitor sensor identifier.
struct BridgeSelectors {
    std::string values[policy::kMetricCount];
};

struct MetricReading {
    bool available = false;
    float value = 0.0f;
    std::string identifier;
};

class LibreHardwareMonitorSession {
public:
    LibreHardwareMonitorSession();
    ~LibreHardwareMonitorSession();

    LibreHardwareMonitorSession(const LibreHardwareMonitorSession&) = delete;
    LibreHardwareMonitorSession& operator=(const LibreHardwareMonitorSession&) = delete;

    // Loads the runtime and the library, subscribes to the hardware lifecycle
    // and opens the CPU/GPU visitors the selectors actually ask for. On failure
    // `failureToken` receives a stable ASCII reason for the bridge protocol.
    bool Start(const std::filesystem::path& pluginDirectory, const BridgeSelectors& selectors,
               std::string& failureToken);

    // Refreshes the hardware tree and resolves every metric. `readings` must
    // hold policy::kMetricCount entries.
    bool Sample(MetricReading* readings, std::string& failureToken);

    void Close();

    // Assembly version of the loaded LibreHardwareMonitorLib, for the ready
    // handshake. Empty until Start succeeded.
    const std::string& LibraryVersion() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ce::hardware_sensors
