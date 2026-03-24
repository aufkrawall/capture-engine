#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ce::audio {

constexpr int64_t kDefaultSteadyAudioPullLatencyMs = 0;
constexpr int64_t kDefaultAudioPullQuantumSamples = 240;

inline bool ShouldDeferAudioPullUntilQuantum(int64_t pendingSamples, bool trackStartupSettled, bool forceDrain,
                                             int64_t quantumSamples = kDefaultAudioPullQuantumSamples) {
    if (forceDrain || !trackStartupSettled) {
        return false;
    }

    if (pendingSamples <= 0) {
        return true;
    }

    return pendingSamples < std::max<int64_t>(1, quantumSamples);
}

inline int64_t ComputeVideoPipelineLagMs(int64_t wallVideoMs, int64_t encodedVideoMs) {
    if (wallVideoMs <= 0 || encodedVideoMs <= 0 || wallVideoMs <= encodedVideoMs) {
        return 0;
    }
    return wallVideoMs - encodedVideoMs;
}

inline int64_t ComputeBufferedAudioTargetSamples(int sampleRate, int64_t baseLatencySamples, int64_t videoPipelineLagMs,
                                                 int64_t maxLagContributionMs = 40) {
    if (sampleRate <= 0) {
        return std::max<int64_t>(0, baseLatencySamples);
    }

    int64_t lagSamples = 0;
    if (videoPipelineLagMs > 0) {
        const int64_t boundedLagMs =
            std::clamp<int64_t>(videoPipelineLagMs, 0, std::max<int64_t>(0, maxLagContributionMs));
        lagSamples = (boundedLagMs * sampleRate) / 1000;
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

inline int64_t ComputeLatencyAdjustedAvDriftMs(int64_t rawAvDriftMs, int64_t intentionalPullLatencyMs) {
    return rawAvDriftMs + std::max<int64_t>(intentionalPullLatencyMs, 0);
}

inline bool ShouldAllowWgcSteadyStateDriftCompensation(bool trackStartupSettled, int64_t videoPipelineLagMs,
                                                       int64_t bufferedSamples, int64_t targetLatencySamples,
                                                       int64_t leadWarningSamples) {
    if (!trackStartupSettled || videoPipelineLagMs > 0 || bufferedSamples <= 0) {
        return false;
    }

    const int64_t boundedTargetSamples = std::max<int64_t>(0, targetLatencySamples);
    const int64_t leadSamples = bufferedSamples - boundedTargetSamples;
    return leadSamples >= std::max<int64_t>(1, leadWarningSamples);
}

inline bool IsTrackAudioStartupSettled(bool trackBootstrapComplete, bool allSourcesPrimed) {
    return trackBootstrapComplete || allSourcesPrimed;
}

inline bool IsOptionalUnstartedAppAudioSource(bool isAppAudioSource, bool hasAlignedStart) {
    return isAppAudioSource && !hasAlignedStart;
}

inline bool IsSourceStartupPrimed(bool sourceIsPrimed, bool hasAlignedStart, bool isAppAudioSource) {
    return sourceIsPrimed || IsOptionalUnstartedAppAudioSource(isAppAudioSource, hasAlignedStart);
}

inline bool IsSourceBootstrapReady(bool sourceBootstrapComplete, bool hasAlignedStart, bool sourceIsPrimed,
                                   bool isAppAudioSource, size_t bufferedTimelineSamples,
                                   size_t requiredTimelineSamples) {
    if (sourceBootstrapComplete || IsOptionalUnstartedAppAudioSource(isAppAudioSource, hasAlignedStart)) {
        return true;
    }

    return hasAlignedStart && sourceIsPrimed && bufferedTimelineSamples >= requiredTimelineSamples;
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
