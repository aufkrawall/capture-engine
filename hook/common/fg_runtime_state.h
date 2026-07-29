#pragma once

#include <cstdint>

namespace ce::fg_runtime {

enum class RuntimeMode {
    kOff,
    kNvidiaSmoothMotion,
    kStreamlineNoFG,
    kDLSSFG,
    kFSRFG,
    kUnknown,
};

struct DetectionSnapshot {
    bool dormant = true;
    bool nvPresentLoaded = false;
    bool streamlineLoaded = false;
    bool streamlineFGSignaled = false;
    bool dlssFGApiActive = false;
    bool fsrFGApiActive = false;
    bool heuristicFSRFGActive = false;
    bool nvidiaSmoothMotionDetected = false;
    int dlssFGMultiplier = 0;
};

inline RuntimeMode ClassifyRuntimeMode(const DetectionSnapshot& snapshot) {
    const bool dlssFGConfirmed = snapshot.dlssFGApiActive && snapshot.dlssFGMultiplier >= 2;
    if (snapshot.fsrFGApiActive) {
        return RuntimeMode::kFSRFG;
    }
    if (dlssFGConfirmed) {
        return RuntimeMode::kDLSSFG;
    }
    if (!snapshot.dormant && snapshot.heuristicFSRFGActive && !snapshot.streamlineFGSignaled && !dlssFGConfirmed) {
        return RuntimeMode::kFSRFG;
    }
    if (snapshot.nvidiaSmoothMotionDetected && snapshot.nvPresentLoaded && !snapshot.streamlineFGSignaled &&
        !snapshot.dlssFGApiActive && !snapshot.fsrFGApiActive) {
        return RuntimeMode::kNvidiaSmoothMotion;
    }
    if (snapshot.streamlineLoaded || snapshot.streamlineFGSignaled) {
        return RuntimeMode::kStreamlineNoFG;
    }
    return RuntimeMode::kOff;
}

inline bool CanEvaluateNvidiaSmoothMotionPattern(const DetectionSnapshot& snapshot) {
    return snapshot.nvPresentLoaded && !snapshot.streamlineFGSignaled && !snapshot.dlssFGApiActive &&
           !snapshot.fsrFGApiActive && !snapshot.heuristicFSRFGActive;
}

inline bool HasNvidiaSmoothMotion2xPopulation(bool nvPresentLoaded, int totalFrames, int highWorkFrames) {
    const int lowWorkFrames = totalFrames - highWorkFrames;
    if (!nvPresentLoaded || totalFrames < 30 || highWorkFrames < 10 || lowWorkFrames < 10) {
        return false;
    }

    // NvPresent-generated DX12 frames still submit a small amount of driver
    // work, so they are not necessarily zero-command-list frames.  Require two
    // substantial work populations whose ratio is conservatively near 2x.
    return totalFrames * 5 >= highWorkFrames * 8 && totalFrames * 5 <= highWorkFrames * 12;
}

inline bool IsRuntimeFGActive(RuntimeMode mode) {
    return mode == RuntimeMode::kDLSSFG || mode == RuntimeMode::kFSRFG || mode == RuntimeMode::kNvidiaSmoothMotion;
}

inline bool IsActualGeneratedFrameMode(RuntimeMode mode) {
    return mode == RuntimeMode::kDLSSFG || mode == RuntimeMode::kFSRFG;
}

inline bool RuntimeModeUsesStreamline(RuntimeMode mode) {
    return mode == RuntimeMode::kStreamlineNoFG || mode == RuntimeMode::kDLSSFG;
}

inline bool RuntimeModeUsesFSR(RuntimeMode mode) {
    return mode == RuntimeMode::kFSRFG;
}

inline const char* GetRuntimeModeName(RuntimeMode mode) {
    switch (mode) {
        case RuntimeMode::kOff:
            return "Off";
        case RuntimeMode::kNvidiaSmoothMotion:
            return "NVIDIA_SM";
        case RuntimeMode::kStreamlineNoFG:
            return "STREAMLINE_NO_FG";
        case RuntimeMode::kDLSSFG:
            return "DLSS_FG";
        case RuntimeMode::kFSRFG:
            return "FSR_FG";
        case RuntimeMode::kUnknown:
        default:
            return "UNKNOWN";
    }
}

}  // namespace ce::fg_runtime
