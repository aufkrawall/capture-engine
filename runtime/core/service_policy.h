// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Which auxiliary helpers a configuration asks for (ported from the controller's main_internal.h).

#include "captureengine/display_timing/display_timing_policy.h"
#include "common/config/config.h"
#include "common/logging/logging.h"

namespace ce::engine {

inline bool ShouldStartLoggerProcess(const AppConfig& config) {
    return IsAnyLoggingEnabled(config.logLevel);
}

// `recordingRequested` keeps display timing alive across a config reload during an active recording.
inline bool ShouldStartSensorProcess(const AppConfig& config, bool recordingRequested) {
    return config.overlay.showCPU || config.overlay.showGPU || config.overlay.showRAM || config.overlay.showVRAM ||
           ShouldStartOverlayDisplayTiming(config.overlay.showOverlay, config.overlay.showSystemLatency) ||
           recordingRequested;
}

inline bool HardwareSensorServiceConfigEquals(const AppConfig& lhs, const AppConfig& rhs) {
    const HardwareSensorsConfig& left = lhs.hardwareSensors;
    const HardwareSensorsConfig& right = rhs.hardwareSensors;
    return lhs.overlay.showCPU == rhs.overlay.showCPU && lhs.overlay.showGPU == rhs.overlay.showGPU &&
           left.enabled == right.enabled && left.pollIntervalMs == right.pollIntervalMs &&
           left.backend == right.backend &&
           left.cpuTemperature == right.cpuTemperature && left.gpuTemperature == right.gpuTemperature &&
           left.cpuPackagePower == right.cpuPackagePower && left.gpuPackagePower == right.gpuPackagePower &&
           left.gpuFan == right.gpuFan && left.cpuCoreClock == right.cpuCoreClock &&
           left.gpuCoreClock == right.gpuCoreClock && left.gpuMemoryClock == right.gpuMemoryClock &&
           left.gpuVoltage == right.gpuVoltage;
}

}  // namespace ce::engine
