#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>

inline int64_t AbsoluteTimestampDistance(int64_t lhs, int64_t rhs) {
    return (lhs >= rhs) ? (lhs - rhs) : (rhs - lhs);
}

template <typename FrameContainer>
size_t SelectFrameClosestToGrid(const FrameContainer& frames, size_t availableCount, int64_t gridStartQpc,
                                int64_t gridTickCount, int64_t targetIntervalTicks) {
    if (availableCount <= 1 || gridStartQpc <= 0 || targetIntervalTicks <= 0) {
        return 0;
    }

    const int64_t idealQpc = gridStartQpc + gridTickCount * targetIntervalTicks;
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
