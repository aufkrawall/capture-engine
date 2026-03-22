#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ce::audio {

inline int64_t ComputeVideoPipelineLagMs(int64_t wallVideoMs, int64_t encodedVideoMs) {
    if (wallVideoMs <= 0 || encodedVideoMs <= 0 || wallVideoMs <= encodedVideoMs) {
        return 0;
    }
    return wallVideoMs - encodedVideoMs;
}

inline int64_t ComputeBufferedAudioTargetSamples(int sampleRate, int64_t baseLatencySamples,
                                                 int64_t videoPipelineLagMs) {
    if (sampleRate <= 0) {
        return std::max<int64_t>(0, baseLatencySamples);
    }

    int64_t lagSamples = 0;
    if (videoPipelineLagMs > 0) {
        lagSamples = (videoPipelineLagMs * sampleRate) / 1000;
    }

    return std::max<int64_t>(0, baseLatencySamples + lagSamples);
}

inline int64_t ComputeAudioPullLatencyMs(int64_t steadyLatencyMs, bool allSourcesPrimed,
                                         int64_t maxObservedLateStartMs) {
    const int64_t baseLatencyMs = std::max<int64_t>(0, steadyLatencyMs);
    if (allSourcesPrimed) {
        return baseLatencyMs;
    }

    const int64_t startupLatencyMs = std::max<int64_t>(baseLatencyMs + 30, maxObservedLateStartMs + 20);
    return std::clamp<int64_t>(startupLatencyMs, baseLatencyMs, 120);
}

inline bool IsTrackAudioStartupSettled(bool trackBootstrapComplete, bool allSourcesPrimed) {
    return trackBootstrapComplete || allSourcesPrimed;
}

inline size_t ComputeBufferedRealAudioSamples(size_t bufferedSamples, uint64_t syntheticBufferedSamples) {
    if (syntheticBufferedSamples >= bufferedSamples) {
        return 0;
    }
    return bufferedSamples - static_cast<size_t>(syntheticBufferedSamples);
}

inline uint64_t ConsumeSyntheticBufferedSamples(uint64_t& syntheticBufferedSamples, uint64_t consumedSamples) {
    const uint64_t syntheticConsumed = std::min<uint64_t>(syntheticBufferedSamples, consumedSamples);
    syntheticBufferedSamples -= syntheticConsumed;
    return syntheticConsumed;
}

}  // namespace ce::audio
