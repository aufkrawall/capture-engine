#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>

inline int64_t AbsoluteTimestampDistance(int64_t lhs, int64_t rhs) {
    return (lhs >= rhs) ? (lhs - rhs) : (rhs - lhs);
}

template <typename Frame>
inline int64_t GetFrameSelectionTimestamp(const Frame& frame) {
    return frame.selectionTimestamp > 0 ? frame.selectionTimestamp : frame.timestamp;
}

template <typename FrameContainer>
size_t SelectFrameClosestToTimestamp(const FrameContainer& frames, size_t availableCount, int64_t targetTimestampQpc) {
    if (availableCount <= 1 || targetTimestampQpc <= 0) {
        return 0;
    }

    size_t bestIndex = 0;
    int64_t bestDistance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[0]), targetTimestampQpc);

    for (size_t i = 1; i < availableCount; ++i) {
        const int64_t distance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[i]), targetTimestampQpc);
        if (distance < bestDistance ||
            (distance == bestDistance &&
             GetFrameSelectionTimestamp(frames[i]) > GetFrameSelectionTimestamp(frames[bestIndex]))) {
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

        const int64_t distance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[i]), targetTimestampQpc);
        if (bestIndex == availableCount || distance < bestDistance ||
            (distance == bestDistance &&
             GetFrameSelectionTimestamp(frames[i]) > GetFrameSelectionTimestamp(frames[bestIndex]))) {
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

inline int64_t ComputeDelayedContentGridStartQpc(int64_t gridStartQpc, int64_t contentDelayQpc) {
    if (gridStartQpc <= 0 || contentDelayQpc <= 0) {
        return gridStartQpc;
    }

    return gridStartQpc - contentDelayQpc;
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

inline int64_t ResolveCfrTimelineElapsedUs(int64_t steadyElapsedUs, int64_t explicitTimelineElapsedUs,
                                           int64_t lastElapsedUs) {
    const int64_t candidateElapsedUs = explicitTimelineElapsedUs >= 0 ? explicitTimelineElapsedUs : steadyElapsedUs;
    return std::max(candidateElapsedUs, lastElapsedUs);
}

inline int64_t ResolveAuthoritativeCfrTimelineElapsedUs(int64_t steadyElapsedUs, int64_t explicitTimelineElapsedUs,
                                                        int64_t lastElapsedUs) {
    if (explicitTimelineElapsedUs >= 0) {
        return explicitTimelineElapsedUs;
    }
    if (lastElapsedUs > 0) {
        return lastElapsedUs;
    }
    return std::max<int64_t>(steadyElapsedUs, 0);
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
    int64_t bestDistance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[0]), idealQpc);

    for (size_t i = 1; i < availableCount; ++i) {
        const int64_t distance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[i]), idealQpc);
        if (distance < bestDistance ||
            (distance == bestDistance &&
             GetFrameSelectionTimestamp(frames[i]) > GetFrameSelectionTimestamp(frames[bestIndex]))) {
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

        const int64_t distance = AbsoluteTimestampDistance(GetFrameSelectionTimestamp(frames[i]), idealQpc);
        if (bestIndex == availableCount || distance < bestDistance ||
            (distance == bestDistance &&
             GetFrameSelectionTimestamp(frames[i]) > GetFrameSelectionTimestamp(frames[bestIndex]))) {
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

// Smooths the input frame delivery rate and reconstructs a locally uniform
// source cadence from noisy raw timestamps.  This prevents irregular frame
// hold patterns (e.g. 2:1:3:1) caused by jittery WGC delivery or duplicate
// timestamps.
class InputFrameRatePredictor {
public:
    void Reset() {
        smoothedIntervalQpc_ = 0.0;
        smoothedJitterQpc_ = 0.0;
        lastInputQpc_ = 0;
        gridOriginQpc_ = 0;
        frameCount_ = 0;
        jitterEmaUpdates_ = 0;
        lastSmoothedQpc_ = 0;
        lastSmoothedRawQpc_ = 0;
        smoothingSnapCount_ = 0;
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

    // Maximum per-frame deviation the monotonic smoother may introduce between
    // the smoothed selection timestamp and the raw source timestamp.  Sized to
    // cover compositor-clock quantization up to 3/4 of the source interval
    // (WGC/DXGI timestamps are DWM composition times: under VRR or composed
    // presentation a perfectly smooth game present stream arrives quantized,
    // e.g. ~4.2/8.3/12.5 ms interval mixes for a 140 fps game) while keeping
    // the content-time error of any emitted frame bounded by 3/4 of the CFR
    // output interval, well inside the active-delay hard sync cap.  Raw
    // timestamps stay untouched for sync validation and diagnostics.
    int64_t GetSmoothingMaxDeviationQpc(int64_t outputIntervalQpc) const {
        if (smoothedIntervalQpc_ < 1.0) {
            return 0;
        }
        const int64_t sourceBoundQpc = static_cast<int64_t>((smoothedIntervalQpc_ * 3.0) / 4.0);
        if (outputIntervalQpc <= 0) {
            return sourceBoundQpc;
        }
        return std::min<int64_t>(sourceBoundQpc, (outputIntervalQpc * 3) / 4);
    }

    // Monotonic bounded-deviation timestamp smoothing for CFR source selection.
    //
    // A CFR playout slaved to raw compositor timestamps sees recurring
    // artificial 9-17 ms holes at the fixed-latency read boundary and converts
    // a surplus source into constant single-tick repeats (observed: 22%
    // duplicates from a healthy 133-140 fps input into a 120 fps target).
    // This pulls each raw timestamp toward the predicted uniform grid position
    // while guaranteeing:
    //   1. bounded deviation:   |smoothed - raw| <= maxDeviationQpc
    //   2. strict monotonicity: smoothed(n) > smoothed(n-1)
    //   3. real gaps pass through un-smeared: a raw jump beyond
    //      2*interval + maxDeviation snaps to the raw timestamp (a genuine
    //      stall must stay a visible hold, not become smeared content time)
    // Quantization noise is zero-mean by construction, so the smoothed stream
    // advances at the true source rate and a surplus source is consumed as
    // pure surplus drops again instead of drop+repeat churn.
    int64_t SmoothMonotonicTimestamp(int64_t rawQpc, int64_t outputIntervalQpc) {
        if (rawQpc <= 0) {
            return rawQpc;
        }
        const int64_t maxDeviationQpc = GetSmoothingMaxDeviationQpc(outputIntervalQpc);
        const int64_t intervalQpc = static_cast<int64_t>(smoothedIntervalQpc_ + 0.5);
        if (!IsCalibrated() || intervalQpc <= 0 || maxDeviationQpc <= 0 || lastSmoothedQpc_ <= 0 ||
            lastSmoothedRawQpc_ <= 0 || rawQpc < lastSmoothedRawQpc_) {
            lastSmoothedQpc_ = std::max(rawQpc, lastSmoothedQpc_ + 1);
            lastSmoothedRawQpc_ = rawQpc;
            return lastSmoothedQpc_;
        }

        const int64_t rawGapQpc = rawQpc - lastSmoothedRawQpc_;
        if (rawGapQpc > intervalQpc * 2 + maxDeviationQpc) {
            // Genuine delivery stall / regime change: relock to the raw time so
            // post-stall content is not shown early by a stale prediction.
            ++smoothingSnapCount_;
            lastSmoothedQpc_ = std::max(rawQpc, lastSmoothedQpc_ + 1);
            lastSmoothedRawQpc_ = rawQpc;
            return lastSmoothedQpc_;
        }

        const int64_t predictedQpc = lastSmoothedQpc_ + intervalQpc;
        int64_t smoothedQpc = std::clamp(predictedQpc, rawQpc - maxDeviationQpc, rawQpc + maxDeviationQpc);
        smoothedQpc = std::max(smoothedQpc, lastSmoothedQpc_ + 1);
        lastSmoothedQpc_ = smoothedQpc;
        lastSmoothedRawQpc_ = rawQpc;
        return smoothedQpc;
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

    bool IsCalibrated() const {
        return frameCount_ >= 4;
    }

    // Number of stall/regime-change relocks performed by SmoothMonotonicTimestamp.
    uint64_t SmoothingSnapCount() const {
        return smoothingSnapCount_;
    }

    double SmoothedIntervalQpc() const {
        return smoothedIntervalQpc_;
    }

private:
    double smoothedIntervalQpc_ = 0.0;
    double smoothedJitterQpc_ = 0.0;
    int64_t lastInputQpc_ = 0;
    int64_t gridOriginQpc_ = 0;
    uint32_t frameCount_ = 0;
    uint32_t jitterEmaUpdates_ = 0;
    int64_t lastSmoothedQpc_ = 0;
    int64_t lastSmoothedRawQpc_ = 0;
    uint64_t smoothingSnapCount_ = 0;
};
