#pragma once

#include <cstdint>

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
};

struct RowInputs {
    bool showGPU = false;
    bool showCPU = false;
    bool showVRAM = false;
    bool showRAM = false;
    bool showFPS = false;
    bool showFPSAverages = false;
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
    if (input.showCPU)
        mask |= kRowCPU;
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
