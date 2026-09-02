#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ce::overlay_layout {

enum class MemoryValueMode : uint8_t {
    Unavailable,
    UsedOnly,
    UsedAndTotal,
};

inline MemoryValueMode SelectMemoryValueMode(bool usageValid, uint64_t usedBytes, uint64_t totalBytes) {
    if (!usageValid && usedBytes == 0)
        return MemoryValueMode::Unavailable;
    return totalBytes != 0 ? MemoryValueMode::UsedAndTotal : MemoryValueMode::UsedOnly;
}

// The leading load percentage carries a load-derived color, but the appended
// sensor readings must not inherit it: drawing the whole composite in one color
// turns temperature, power and fan red the moment GPU load reaches the high
// threshold, which reads as though those sensors were themselves critical.
// sensorOffset reports where the appended readings start so a caller can draw
// the two spans in different colors; it equals the load length when nothing
// was appended.
inline void FormatCpuMetricsValue(char* output, size_t outputSize, float usage, float maxCoreUsage,
                                  bool temperatureValid, float temperatureC, bool powerValid, float powerW,
                                  size_t* sensorOffset = nullptr) {
    if (!output || outputSize == 0)
        return;
    int length = snprintf(output, outputSize, "%.0f%% (%.0f%%)", usage, maxCoreUsage);
    if (length < 0 || static_cast<size_t>(length) >= outputSize)
        return;
    if (sensorOffset)
        *sensorOffset = static_cast<size_t>(length);
    if (temperatureValid) {
        const size_t remaining = outputSize - static_cast<size_t>(length);
        const int appended = snprintf(output + length, remaining, "  %.0f C", temperatureC);
        if (appended < 0 || static_cast<size_t>(appended) >= remaining)
            return;
        length += appended;
    }
    if (powerValid)
        snprintf(output + length, outputSize - static_cast<size_t>(length), "  %.0f W", powerW);
}

inline void FormatGpuMetricsValue(char* output, size_t outputSize, bool usageValid, float usage,
                                  bool temperatureValid, float temperatureC, bool powerValid, float powerW,
                                  bool fanValid, float fanRpm, size_t* sensorOffset = nullptr) {
    if (!output || outputSize == 0)
        return;
    int length = usageValid ? snprintf(output, outputSize, "%.0f%%", usage) : snprintf(output, outputSize, "--");
    if (length < 0 || static_cast<size_t>(length) >= outputSize)
        return;
    if (sensorOffset)
        *sensorOffset = static_cast<size_t>(length);
    if (temperatureValid) {
        const size_t remaining = outputSize - static_cast<size_t>(length);
        const int appended = snprintf(output + length, remaining, "  %.0f C", temperatureC);
        if (appended < 0 || static_cast<size_t>(appended) >= remaining)
            return;
        length += appended;
    }
    if (powerValid) {
        const size_t remaining = outputSize - static_cast<size_t>(length);
        const int appended = snprintf(output + length, remaining, "  %.0f W", powerW);
        if (appended < 0 || static_cast<size_t>(appended) >= remaining)
            return;
        length += appended;
    }
    if (fanValid)
        snprintf(output + length, outputSize - static_cast<size_t>(length), "  %.0f RPM", fanRpm);
}

inline constexpr const char* kGpuClocksLabel = "GPU Clocks";
inline constexpr const char* kCpuClocksLabel = "CPU Clocks";

// Clocks and voltage occupy their own row instead of extending the usage rows:
// appending them there would push the widest row - and with it the whole
// adaptive overlay width - well past the memory rows.
inline void FormatGpuClocksValue(char* output, size_t outputSize, bool coreClockValid, float coreClockMhz,
                                 bool memoryClockValid, float memoryClockMhz, bool voltageValid, float voltageV) {
    if (!output || outputSize == 0)
        return;
    output[0] = 0;
    int length = 0;
    const char* separator = "";
    if (coreClockValid) {
        length = snprintf(output, outputSize, "%.0f MHz", coreClockMhz);
        if (length < 0 || static_cast<size_t>(length) >= outputSize)
            return;
        separator = "  ";
    }
    if (memoryClockValid) {
        const size_t remaining = outputSize - static_cast<size_t>(length);
        const int appended = snprintf(output + length, remaining, "%s%.0f MHz", separator, memoryClockMhz);
        if (appended < 0 || static_cast<size_t>(appended) >= remaining)
            return;
        length += appended;
        separator = "  ";
    }
    if (voltageValid)
        snprintf(output + length, outputSize - static_cast<size_t>(length), "%s%.3f V", separator, voltageV);
}

inline void FormatCpuClocksValue(char* output, size_t outputSize, bool coreClockValid, float coreClockMhz) {
    if (!output || outputSize == 0)
        return;
    output[0] = 0;
    if (coreClockValid)
        snprintf(output, outputSize, "%.0f MHz", coreClockMhz);
}

enum OverlayRow : uint32_t {
    kRowGPU = 1u << 0,
    kRowCPU = 1u << 1,
    kRowVRAM = 1u << 2,
    kRowRAM = 1u << 3,
    kRowFPS = 1u << 4,
    kRowFGRates = 1u << 5,
    kRowFPSAverages = 1u << 6,
    kRowFGStatus = 1u << 7,
    kRowRecording = 1u << 8,
    kRowNotification = 1u << 9,
    kRowSystemLatency = 1u << 10,
    kRowGPUClocks = 1u << 11,
    kRowCPUClocks = 1u << 12,
};

struct RowInputs {
    bool showGPU = false;
    bool showCPU = false;
    // Set only when at least one of the row's own sensors is readable, so an
    // unelevated run does not reserve an empty clock row.
    bool showGPUClocks = false;
    bool showCPUClocks = false;
    bool showVRAM = false;
    bool showRAM = false;
    bool showFPS = false;
    bool showFPSAverages = false;
    bool showSystemLatency = false;
    bool showFG = false;
    bool fgActive = false;
    bool reserveFGSpace = false;
    bool showRecording = false;
    bool recordingActive = false;
    bool recordingStarting = false;
    bool notificationVisible = false;
};

inline uint32_t BuildOverlayRowMask(const RowInputs& input) {
    uint32_t mask = 0;
    if (input.showGPU)
        mask |= kRowGPU;
    if (input.showGPU && input.showGPUClocks)
        mask |= kRowGPUClocks;
    if (input.showCPU)
        mask |= kRowCPU;
    if (input.showCPU && input.showCPUClocks)
        mask |= kRowCPUClocks;
    if (input.showVRAM)
        mask |= kRowVRAM;
    if (input.showRAM)
        mask |= kRowRAM;
    if (input.showFPS) {
        mask |= kRowFPS;
        if (input.showFPSAverages)
            mask |= kRowFPSAverages;
        if (input.showFG && (input.fgActive || input.reserveFGSpace))
            mask |= kRowFGRates;
    }
    if (input.showSystemLatency)
        mask |= kRowSystemLatency;
    if (input.showFG && (input.fgActive || input.reserveFGSpace))
        mask |= kRowFGStatus;
    if (input.showRecording && (input.recordingActive || input.recordingStarting))
        mask |= kRowRecording;
    if (input.notificationVisible)
        mask |= kRowNotification;
    return mask;
}

inline uint32_t CountOverlayRows(uint32_t mask) {
    uint32_t count = 0;
    while (mask) {
        count += mask & 1u;
        mask >>= 1;
    }
    return count;
}

}  // namespace ce::overlay_layout
