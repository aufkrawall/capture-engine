#pragma once

// Bridge role of captureengine.exe. The sensor service launches the same
// executable with kSensorBridgeCommand instead of a PowerShell interpreter, so
// the optional LibreHardwareMonitor integration needs no script and no extra
// binary while keeping the CLR and the sensor library out of every other
// CaptureEngine process.

#include <cstddef>
#include <optional>
#include <string>

#include "sensor_selection_policy.h"

namespace ce::hardware_sensors {

inline constexpr wchar_t kSensorBridgeCommand[] = L"--sensor-bridge";
inline constexpr wchar_t kSensorBridgeShutdownEventOption[] = L"--shutdown-event=";
inline constexpr wchar_t kSensorBridgePollIntervalOption[] = L"--poll-interval-ms=";

// Command-line option name for one metric selector, derived from its policy key
// so the launcher and the bridge cannot disagree about the spelling. Inline so
// the launcher does not have to link the bridge role.
inline std::wstring MetricSelectorOption(size_t metricIndex) {
    if (metricIndex >= policy::kMetricCount)
        return {};
    std::wstring option = L"--";
    for (const char* character = policy::kMetrics[metricIndex].key; *character != 0; ++character)
        option.push_back(*character == '_' ? L'-' : static_cast<wchar_t>(*character));
    option.push_back(L'=');
    return option;
}

// Returns a process exit code when this command line selects the bridge role,
// and std::nullopt for every other CaptureEngine role.
std::optional<int> TryRunSensorBridgeHost();

}  // namespace ce::hardware_sensors
