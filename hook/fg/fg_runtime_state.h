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

// Which stream a present on a present interposer's private OUTPUT chain belongs to.
//
// DX12 answers this from the command-list population the frame submitted. DX11 had no
// classifier at all - `RecordPresentForNvidiaSmoothMotion` recorded a constant 1 for every
// present - so every output present counted as an application frame and the overlay reported
// the interposer's OUTPUT rate as the game's frame rate (Witcher 3, session 20260919_160555:
// 6962 presents in 40.8 s, 3231 groups of exactly two, i.e. ~85 application fps shown as ~171).
//
// The discriminator is not timing and not a threshold. A generated frame is produced entirely
// inside the interposer, so the APPLICATION's own immediate context is idle across it: it
// submits no draw, dispatch or clear between the real present and its generated partner. One
// or more application submissions since the previous present therefore means this present
// carries an application frame, and none means the interposer generated it.
//
// Counting is on the game's device only. CE's overlay and the interposer's interpolation both
// run on the interposer's own device under Smooth Motion, so neither can inflate this.
inline bool IsApplicationSourcedInterposerPresent(uint64_t applicationSubmissionsSincePreviousPresent) {
    return applicationSubmissionsSincePreviousPresent > 0;
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

inline bool HasContrastingPresentGaps(int64_t previousGapUs, int64_t currentGapUs) {
    if (previousGapUs <= 0 || currentGapUs <= 0) {
        return false;
    }

    const int64_t shorterGapUs = previousGapUs < currentGapUs ? previousGapUs : currentGapUs;
    const int64_t longerGapUs = previousGapUs < currentGapUs ? currentGapUs : previousGapUs;
    return longerGapUs / shorterGapUs >= 3;
}

inline bool HasNvidiaSmoothMotionPairedPresentCadence(bool nvPresentLoaded, int presentGapCount,
                                                      int contrastingGapTransitions) {
    if (!nvPresentLoaded || presentGapCount < 20 || contrastingGapTransitions < 0 ||
        contrastingGapTransitions >= presentGapCount) {
        return false;
    }

    // Recent NvPresent drivers can pace generated frames below Present, making
    // callback start times alternate between a short and long gap. Require this
    // strong pairing in at least 80% of adjacent gap transitions.
    return contrastingGapTransitions * 5 >= (presentGapCount - 1) * 4;
}

inline bool ShouldRetainConfirmedNvidiaSmoothMotion(const DetectionSnapshot& snapshot) {
    return snapshot.nvidiaSmoothMotionDetected && CanEvaluateNvidiaSmoothMotionPattern(snapshot);
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
