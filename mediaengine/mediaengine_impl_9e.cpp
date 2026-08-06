#include "mediaengine_internal.h"

bool MediaEngine::AudioLoopTail(AudioLoopState& s) {
    constexpr int64_t kAudioWorkerSchedulingGapThresholdUs = AudioLoopState::kAudioWorkerSchedulingGapThresholdUs;

    auto& audioWorkerSchedulingGapEvents = s.audioWorkerSchedulingGapEvents;
    auto& audioWorkerSchedulingGapMaxUs = s.audioWorkerSchedulingGapMaxUs;
    auto& captureFanoutPacketCounts = s.captureFanoutPacketCounts;
    auto& batchedPreStartDiscardCounts = s.batchedPreStartDiscardCounts;
    auto& captureFanoutQueues = s.captureFanoutQueues;
    auto& packetCount = s.packetCount;
    auto& mixCount = s.mixCount;

    DLL_Log("[AudioLoop] Scheduling summary: events=%llu maxGap=%lldus threshold=%lldus",
            static_cast<unsigned long long>(audioWorkerSchedulingGapEvents),
            (long long)audioWorkerSchedulingGapMaxUs, (long long)kAudioWorkerSchedulingGapThresholdUs);

    for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
        if (audioSources[srcIdx].captureFanoutOwnerIndex != srcIdx ||
            (captureFanoutPacketCounts[srcIdx] == 0 && batchedPreStartDiscardCounts[srcIdx] == 0)) {
            continue;
        }
        size_t pendingFollowers = 0;
        for (size_t routeIdx = 0; routeIdx < audioSources.size(); ++routeIdx) {
            if (audioSources[routeIdx].captureFanoutOwnerIndex == srcIdx) {
                pendingFollowers += captureFanoutQueues[routeIdx].size();
            }
        }
        DLL_Log(
            "[AudioRoute] Stop owner=%zu fannedPackets=%llu batchedPreStartDiscards=%llu "
            "pendingFollowerPackets=%zu",
            srcIdx, static_cast<unsigned long long>(captureFanoutPacketCounts[srcIdx]),
            static_cast<unsigned long long>(batchedPreStartDiscardCounts[srcIdx]), pendingFollowers);
    }

    DLL_Log(
        "MediaEngine: Audio thread stopped, processed %d packets, %d mixed "
        "chunks",
        packetCount, mixCount);

    return true;
}
