#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>

inline int64_t AbsoluteTimestampDistance(int64_t lhs, int64_t rhs) {
    return (lhs >= rhs) ? (lhs - rhs) : (rhs - lhs);
}

template <typename FrameContainer>
size_t SelectFrameClosestToTimestamp(const FrameContainer& frames, size_t availableCount, int64_t targetTimestampQpc) {
    if (availableCount <= 1 || targetTimestampQpc <= 0) {
        return 0;
    }

    size_t bestIndex = 0;
    int64_t bestDistance = AbsoluteTimestampDistance(frames[0].timestamp, targetTimestampQpc);

    for (size_t i = 1; i < availableCount; ++i) {
        const int64_t distance = AbsoluteTimestampDistance(frames[i].timestamp, targetTimestampQpc);
        if (distance < bestDistance ||
            (distance == bestDistance && frames[i].timestamp > frames[bestIndex].timestamp)) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

template <typename FrameContainer, typename Predicate>
size_t SelectFrameClosestToTimestampIf(const FrameContainer& frames, size_t availableCount, int64_t targetTimestampQpc,
                                       Predicate&& predicate) {
    if (availableCount == 0 || targetTimestampQpc <= 0) {
        return availableCount;
    }

    size_t bestIndex = availableCount;
    int64_t bestDistance = 0;
    for (size_t i = 0; i < availableCount; ++i) {
        if (!predicate(frames[i])) {
            continue;
        }

        const int64_t distance = AbsoluteTimestampDistance(frames[i].timestamp, targetTimestampQpc);
        if (bestIndex == availableCount || distance < bestDistance ||
            (distance == bestDistance && frames[i].timestamp > frames[bestIndex].timestamp)) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

template <typename FrameContainer, typename Predicate>
size_t FindPreviousFrameIndexIf(const FrameContainer& frames, size_t startExclusive, Predicate&& predicate) {
    const size_t searchStart = std::min(startExclusive, frames.size());
    for (size_t i = searchStart; i > 0; --i) {
        const size_t candidateIndex = i - 1;
        if (predicate(frames[candidateIndex])) {
            return candidateIndex;
        }
    }

    return frames.size();
}

template <typename FrameContainer, typename Predicate>
size_t FindNextFrameIndexIf(const FrameContainer& frames, size_t startExclusive, Predicate&& predicate) {
    const size_t searchStart = std::min(startExclusive, frames.size());
    for (size_t i = searchStart; i < frames.size(); ++i) {
        if (predicate(frames[i])) {
            return i;
        }
    }

    return frames.size();
}

inline int64_t ComputeIdealOutputQpc(int64_t gridStartQpc, int64_t gridTickCount, int64_t targetIntervalTicks) {
    if (gridStartQpc <= 0 || targetIntervalTicks <= 0 || gridTickCount <= 0) {
        return gridStartQpc;
    }

    return gridStartQpc + (gridTickCount - 1) * targetIntervalTicks;
}

inline int64_t ComputeWgcSelectionTargetQpc(int64_t scheduledSampleQpc, int64_t gridStartQpc, int64_t gridTickCount,
                                            int64_t targetIntervalTicks) {
    if (scheduledSampleQpc > 0) {
        return scheduledSampleQpc;
    }

    return ComputeIdealOutputQpc(gridStartQpc, gridTickCount, targetIntervalTicks);
}

inline int64_t ComputeCfrFrameIndexForElapsedUs(int64_t elapsedUs, int fps, int64_t lastAssignedFrameIndex) {
    if (fps <= 0) {
        fps = 60;
    }

    if (elapsedUs < 0) {
        elapsedUs = 0;
    }

    const int64_t roundedFrameIndex = (elapsedUs * static_cast<int64_t>(fps) + 500000) / 1000000;
    if (lastAssignedFrameIndex < 0) {
        return roundedFrameIndex;
    }

    return std::max<int64_t>(roundedFrameIndex, lastAssignedFrameIndex + 1);
}

inline int64_t ComputeNextCfrFrameIndex(int64_t lastAssignedFrameIndex) {
    return lastAssignedFrameIndex >= 0 ? (lastAssignedFrameIndex + 1) : 0;
}

inline bool ShouldHoldFrameForNextTick(int64_t frameTimestampQpc, int64_t idealQpc, int64_t targetIntervalTicks,
                                       int64_t holdSlackQpc) {
    if (frameTimestampQpc <= 0 || idealQpc <= 0 || targetIntervalTicks <= 0) {
        return false;
    }

    if (frameTimestampQpc <= idealQpc) {
        return false;
    }

    const int64_t currentDistance = AbsoluteTimestampDistance(frameTimestampQpc, idealQpc);
    const int64_t nextDistance = AbsoluteTimestampDistance(frameTimestampQpc, idealQpc + targetIntervalTicks);
    return nextDistance + std::max<int64_t>(holdSlackQpc, 0) < currentDistance;
}

template <typename FrameContainer>
size_t SelectFrameClosestToGrid(const FrameContainer& frames, size_t availableCount, int64_t gridStartQpc,
                                int64_t gridTickCount, int64_t targetIntervalTicks) {
    if (availableCount <= 1 || gridStartQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const int64_t idealQpc = ComputeIdealOutputQpc(gridStartQpc, gridTickCount, targetIntervalTicks);
    size_t bestIndex = 0;
    int64_t bestDistance = AbsoluteTimestampDistance(frames[0].timestamp, idealQpc);

    for (size_t i = 1; i < availableCount; ++i) {
        const int64_t distance = AbsoluteTimestampDistance(frames[i].timestamp, idealQpc);
        if (distance < bestDistance || (distance == bestDistance && frames[i].timestamp > frames[bestIndex].timestamp)) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

template <typename FrameContainer, typename Predicate>
size_t SelectFrameClosestToGridIf(const FrameContainer& frames, size_t availableCount, int64_t gridStartQpc,
                                  int64_t gridTickCount, int64_t targetIntervalTicks, Predicate&& predicate) {
    if (availableCount == 0 || gridStartQpc <= 0 || targetIntervalTicks <= 0) {
        return availableCount;
    }

    const int64_t idealQpc = ComputeIdealOutputQpc(gridStartQpc, gridTickCount, targetIntervalTicks);
    size_t bestIndex = availableCount;
    int64_t bestDistance = 0;

    for (size_t i = 0; i < availableCount; ++i) {
        if (!predicate(frames[i])) {
            continue;
        }

        const int64_t distance = AbsoluteTimestampDistance(frames[i].timestamp, idealQpc);
        if (bestIndex == availableCount || distance < bestDistance ||
            (distance == bestDistance && frames[i].timestamp > frames[bestIndex].timestamp)) {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    return bestIndex;
}

struct SourceTimelineState {
    int64_t startSourceQpc = 0;
    int64_t lastElapsedUs = 0;

    void Reset() {
        startSourceQpc = 0;
        lastElapsedUs = 0;
    }
};

inline int64_t ComputeSourceDrivenElapsedUs(int64_t qpcFreq, int64_t frameTimestampQpc, int64_t steadyElapsedUs,
                                             SourceTimelineState& state) {
    if (qpcFreq > 0 && frameTimestampQpc > 0) {
        if (state.startSourceQpc <= 0) {
            state.startSourceQpc = frameTimestampQpc;
        }

        if (frameTimestampQpc >= state.startSourceQpc) {
            const int64_t sourceElapsedUs = ((frameTimestampQpc - state.startSourceQpc) * 1000000) / qpcFreq;
            if (sourceElapsedUs > state.lastElapsedUs) {
                state.lastElapsedUs = sourceElapsedUs;
                return sourceElapsedUs;
            }
        }
    }

    state.lastElapsedUs = std::max(steadyElapsedUs, state.lastElapsedUs);
    return state.lastElapsedUs;
}

// Smooths the input frame delivery rate and snaps raw timestamps to a regular
// grid.  This prevents irregular frame hold patterns (e.g. 2:1:3:1) caused by
// jittery WGC delivery or duplicate timestamps.
class InputFrameRatePredictor {
public:
    void Reset() {
        smoothedIntervalQpc_ = 0.0;
        smoothedJitterQpc_ = 0.0;
        lastInputQpc_ = 0;
        gridOriginQpc_ = 0;
        frameCount_ = 0;
        jitterEmaUpdates_ = 0;
    }

    // Called each time a new source frame arrives.  Returns the smoothed
    // interval in QPC ticks (0 if not yet calibrated).
    int64_t Update(int64_t frameQpc, int64_t qpcFreq) {
        if (qpcFreq <= 0) {
            return 0;
        }

        if (lastInputQpc_ <= 0) {
            lastInputQpc_ = frameQpc;
            gridOriginQpc_ = frameQpc;
            return 0;
        }

        // Count duplicate timestamps toward calibration so the predictor can
        // stabilize even when only 1 frame arrives per encoder tick (common
        // when game FPS ~= encoder FPS).
        if (frameQpc == lastInputQpc_) {
            ++frameCount_;
            return static_cast<int64_t>(smoothedIntervalQpc_ + 0.5);
        }

        if (frameQpc < lastInputQpc_) {
            lastInputQpc_ = frameQpc;
            gridOriginQpc_ = frameQpc;
            frameCount_ = 0;
            return 0;
        }

        const int64_t rawInterval = frameQpc - lastInputQpc_;
        lastInputQpc_ = frameQpc;
        ++frameCount_;

        if (frameCount_ <= 2) {
            smoothedIntervalQpc_ = static_cast<double>(rawInterval);
            smoothedJitterQpc_ = 0.0;
            gridOriginQpc_ = frameQpc;
            return rawInterval;
        }

        double alpha = 0.3;
        if (frameCount_ < 8) {
            alpha = 0.6;
        } else if (smoothedIntervalQpc_ > 1.0) {
            double deviation = std::abs(static_cast<double>(rawInterval) - smoothedIntervalQpc_) / smoothedIntervalQpc_;
            if (deviation > 0.20) {
                alpha = 0.5;
            }
        }

        smoothedIntervalQpc_ = smoothedIntervalQpc_ * (1.0 - alpha) + static_cast<double>(rawInterval) * alpha;

        const double absJitter = std::abs(static_cast<double>(rawInterval) - smoothedIntervalQpc_);
        ++jitterEmaUpdates_;
        const double jitterAlpha = jitterEmaUpdates_ < 8 ? 0.5 : 0.1;
        smoothedJitterQpc_ = smoothedJitterQpc_ * (1.0 - jitterAlpha) + absJitter * jitterAlpha;

        return static_cast<int64_t>(smoothedIntervalQpc_ + 0.5);
    }

    // Snap a raw QPC timestamp to the nearest position on the predicted
    // regular grid.  This produces smooth frame timing even when actual
    // delivery is jittery.
    int64_t GetIdealTimestamp(int64_t rawQpc) const {
        if (smoothedIntervalQpc_ < 1.0 || gridOriginQpc_ <= 0 || rawQpc <= 0) {
            return rawQpc;
        }

        const double interval = smoothedIntervalQpc_;
        const double elapsed = static_cast<double>(rawQpc - gridOriginQpc_);
        const double gridPosition = round(elapsed / interval) * interval;
        return gridOriginQpc_ + static_cast<int64_t>(gridPosition + 0.5);
    }

    double GetPredictedFps(int64_t qpcFreq) const {
        if (smoothedIntervalQpc_ < 1.0 || qpcFreq <= 0) {
            return 0.0;
        }
        return static_cast<double>(qpcFreq) / smoothedIntervalQpc_;
    }

    double GetJitterUs(int64_t qpcFreq) const {
        if (smoothedJitterQpc_ < 0.5 || qpcFreq <= 0) {
            return 0.0;
        }
        return smoothedJitterQpc_ * 1000000.0 / static_cast<double>(qpcFreq);
    }

    bool IsCalibrated() const { return frameCount_ >= 4; }

    double SmoothedIntervalQpc() const { return smoothedIntervalQpc_; }

private:
    double smoothedIntervalQpc_ = 0.0;
    double smoothedJitterQpc_ = 0.0;
    int64_t lastInputQpc_ = 0;
    int64_t gridOriginQpc_ = 0;
    uint32_t frameCount_ = 0;
    uint32_t jitterEmaUpdates_ = 0;
};
