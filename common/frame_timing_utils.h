#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

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

inline int64_t ComputeIdealOutputQpc(int64_t gridStartQpc, int64_t gridTickCount, int64_t targetIntervalTicks) {
    if (gridStartQpc <= 0 || targetIntervalTicks <= 0 || gridTickCount <= 0) {
        return gridStartQpc;
    }

    return gridStartQpc + (gridTickCount - 1) * targetIntervalTicks;
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
