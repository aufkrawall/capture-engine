#include "mediaengine_internal.h"

void MediaEngine::PullAndEncodeAudio(int64_t videoTimelineUs, bool forceDrain) {
    if (!recording || audioSources.empty())
        return;

    AudioPullState s;
    s.videoTimelineUs = videoTimelineUs;
    s.forceDrain = forceDrain;
    const auto& trackToSources = cachedTrackToSources;
    if (!ComputeAudioPullTargets(s, videoTimelineUs, forceDrain))
        return;

    for (const auto& kv : trackToSources) {
        int track = kv.first;
        const auto& srcIndices = kv.second;
        if (srcIndices.empty())
            continue;
        if (!PullTrackBootstrap(s, track, srcIndices))
            continue;
        if (!PullTrackGapAndBuffer(s, track, srcIndices))
            continue;
        if (!PullTrackEncodeSourcesA(s, track, srcIndices))
            continue;
        if (!PullTrackSyncMonitoring(s, track, srcIndices))
            continue;
    }

        // A pull may have consumed the last post-resampler samples of an old epoch.
        // Acknowledge only after that content has actually entered the common track cursor.
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }
        if (sharedMemLayout) {
            sharedMemLayout->runtimeState.wgcAudioLeadExcessSamples.store(
                s.isWgcCfrRecording ? s.maxWgcAudioLeadExcessSamples : 0u, std::memory_order_relaxed);
        }


}
