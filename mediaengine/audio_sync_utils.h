#pragma once

#include <algorithm>
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

}  // namespace ce::audio
