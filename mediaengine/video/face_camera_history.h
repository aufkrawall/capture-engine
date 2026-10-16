#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Face-camera frame history (FaceCameraRenderer). The screen content and the
// microphone audio of an output slot are from the slot's content time
// (scheduled - content delay, up to ~300 ms with the WGC/DXGI smoothness
// reservoir), so the camera picture must come from that time too instead of
// being the newest frame.
namespace ce::face_camera {

inline constexpr size_t kHistoryInitialFrames = 4;
inline constexpr size_t kHistoryMaxFrames = 48;

// Capture QPC of a camera frame. Media Foundation capture sources stamp samples
// on the QPC-based system clock in 100 ns units; that time is used when it is
// plausible (not after the sample arrived, at most one second before),
// otherwise the arrival QPC. Returns true when the device time was used.
inline bool ResolveCaptureQpc(int64_t sampleTime100ns, int64_t arrivalQpc, int64_t qpcFrequency, int64_t* captureQpc) {
    *captureQpc = arrivalQpc;
    if (sampleTime100ns <= 0 || qpcFrequency <= 0 || arrivalQpc <= 0)
        return false;
    constexpr int64_t k100nsPerSecond = 10000000;
    const int64_t deviceQpc = (sampleTime100ns / k100nsPerSecond) * qpcFrequency +
                              ((sampleTime100ns % k100nsPerSecond) * qpcFrequency) / k100nsPerSecond;
    if (deviceQpc <= 0 || deviceQpc > arrivalQpc || arrivalQpc - deviceQpc > qpcFrequency)
        return false;
    *captureQpc = deviceQpc;
    return true;
}

// How far the shown camera frame was captured before its output slot's
// content time (one camera interval or less when in sync), per log window.
struct LagSummary {
    int64_t minUs = 0;
    int64_t maxUs = 0;
    int64_t sumUs = 0;
    uint64_t count = 0;
    uint64_t olderThanHistory = 0;
    void Add(int64_t lagUs, bool olderThanAllFrames) {
        minUs = count == 0 || lagUs < minUs ? lagUs : minUs;
        maxUs = count == 0 || lagUs > maxUs ? lagUs : maxUs;
        sumUs += lagUs;
        ++count;
        olderThanHistory += olderThanAllFrames ? 1u : 0u;
    }
    int64_t AverageUs() const { return count ? sumUs / static_cast<int64_t>(count) : 0; }
};

struct HistorySelection {
    size_t index = SIZE_MAX;  // SIZE_MAX: no frame
    // The target is older than every retained frame: the history is too
    // short for this content delay and should grow to `wantedFrames`.
    bool targetOlderThanHistory = false;
    size_t wantedFrames = 0;
};

// captureQpc[i] == 0 marks an empty slot. targetQpc == 0 selects the newest
// frame (no slot timing known). Otherwise the newest frame captured at or
// before the target is chosen, never a frame from the slot's future.
inline HistorySelection SelectHistoryFrame(const std::vector<int64_t>& captureQpc, int64_t targetQpc,
                                           int64_t frameIntervalQpc) {
    HistorySelection out;
    size_t newest = SIZE_MAX;
    size_t oldest = SIZE_MAX;
    size_t atOrBefore = SIZE_MAX;
    size_t valid = 0;
    for (size_t i = 0; i < captureQpc.size(); ++i) {
        const int64_t qpc = captureQpc[i];
        if (qpc <= 0)
            continue;
        ++valid;
        if (newest == SIZE_MAX || qpc > captureQpc[newest])
            newest = i;
        if (oldest == SIZE_MAX || qpc < captureQpc[oldest])
            oldest = i;
        if (targetQpc > 0 && qpc <= targetQpc && (atOrBefore == SIZE_MAX || qpc > captureQpc[atOrBefore]))
            atOrBefore = i;
    }
    if (valid == 0)
        return out;
    if (targetQpc <= 0) {
        out.index = newest;
        return out;
    }
    if (atOrBefore != SIZE_MAX) {
        out.index = atOrBefore;
        return out;
    }
    // Every retained frame is newer than the slot: show the oldest one and
    // ask for enough history to cover this lag (plus one frame of margin).
    out.index = oldest;
    out.targetOlderThanHistory = true;
    if (frameIntervalQpc > 0 && valid == captureQpc.size()) {
        const int64_t lag = captureQpc[newest] - targetQpc;
        const auto frames = static_cast<size_t>(lag / frameIntervalQpc) + 2u;
        out.wantedFrames = frames < kHistoryMaxFrames ? frames : kHistoryMaxFrames;
    }
    return out;
}

}  // namespace ce::face_camera
