#include "mediaengine_internal.h"

bool MediaEngine::AudioLoopInit(AudioLoopState& s) {


    DLL_Log("MediaEngine: Audio thread started (sources=%d)", (int)audioSources.size());
    s.packetCount = 0;
    s.mixCount = 0;

    // Check if any track has multiple sources (requires mixing)
    std::map<int, int> trackSourceCount;
    std::map<int, std::vector<size_t>> trackSourceIndices;  // track -> source indices
    for (size_t i = 0; i < audioSources.size(); i++) {
        auto& src = audioSources[i];
        trackSourceCount[src.track]++;
        trackSourceIndices[src.track].push_back(i);
    }

    // Per-track source summary (process names) and a runtime guard that surfaces
    // any duplicate app-audio capture feeding one track (identical streams comb).
    for (auto& kv : trackSourceIndices) {
        std::string summary;
        std::set<std::string> appIdentities;
        for (size_t idx : kv.second) {
            auto& s = audioSources[idx];
            std::string label;
            if (s.sourceType == AudioConfig::AppAudio) {
                const char* pn = s.config.processName.empty() ? "<pid>" : s.config.processName.c_str();
                label = std::string("app:") + pn;
                if (!appIdentities.insert(AppAudioTrackKey(s.config, kv.first)).second) {
                    DLL_Log(
                        "AudioLoop: WARNING - track %d has duplicate app-audio capture for '%s' - identical "
                        "streams will comb-filter when mixed",
                        kv.first, pn);
                }
            } else if (s.sourceType == AudioConfig::Microphone) {
                label = "mic";
            } else {
                label = "system";
            }
            if (!summary.empty())
                summary += ", ";
            summary += label;
        }
        DLL_Log("AudioLoop: Track %d sources: [%s]", kv.first, summary.c_str());
    }

    for (auto& kv : trackSourceCount) {
        if (kv.second > 1) {
            DLL_Log("AudioLoop: Track %d has %d sources - REAL mixing enabled", kv.first, kv.second);
        }
    }

    s.sourceTimestamps = std::vector<int64_t>(audioSources.size(), 0);
    s.sourceLoggedPreStartDrop = std::vector<bool>(audioSources.size(), false);
    s.sourceLastPackets = std::vector<AudioPacket>(audioSources.size());
    s.lastPacketTime = std::vector<std::chrono::steady_clock::time_point>(
        audioSources.size(), std::chrono::steady_clock::now());
    s.deferredFirstTimelinePackets = std::vector<AudioPacket>(audioSources.size());
    s.deferredFirstTimelinePacketValid = std::vector<bool>(audioSources.size(), false);
    s.deferredFirstTimelinePacketStartSamples = std::vector<int64_t>(audioSources.size(), 0);
    s.sourceCaptureEpochs = std::vector<uint64_t>(audioSources.size(), 0);
    s.pendingEpochPackets = std::vector<std::deque<AudioPacket>>(audioSources.size());
    s.captureFanoutQueues = std::vector<std::deque<AudioPacket>>(audioSources.size());
    s.captureFanoutPacketCounts = std::vector<uint64_t>(audioSources.size(), 0);
    s.batchedPreStartDiscardCounts = std::vector<uint64_t>(audioSources.size(), 0);
    s.lastAudioWorkerIteration = std::chrono::steady_clock::now();
    s.audioWorkerSchedulingDiagnosticsArmTime = s.lastAudioWorkerIteration + std::chrono::seconds(1);
    s.audioWorkerSchedulingGapEvents = 0;
    s.audioWorkerSchedulingGapMaxUs = 0;
    s.lastAudioWorkerSchedulingGapLogMs = 0;
    s.sharedStartupRebaseOffsetSamples = -1;
    s.appliedAudioResetGeneration = audioResetAcknowledgedGeneration.load(std::memory_order_acquire);
    s.audioOnlyStopTailFinalized = false;

    // Per-source A/V equalization: delay each audio source so every source sits at the same
    // (max) capture latency, matching the video content delay. Faster sources (e.g. a
    // near-zero-latency microphone vs a high-latency loopback endpoint) get leading silence
    // so they align with the delayed video AND with each other on mixed tracks. The slowest
    // source(s) get delay 0, so the common all-equal-latency case is unchanged (no regression).
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& eqSrc : audioSources) {
        if (eqSrc.config.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = eqSrc.config.captureLatencyMs;
        }
    }
    DLL_Log("[AVSyncAuto] audio_equalization: sources=%zu maxCaptureLatencyMs=%.3f confidence=%s reason=%s",
            audioSources.size(), static_cast<double>(maxAudioCaptureLatencyMs), config.avSyncConfidence.c_str(),
            config.avSyncReason.c_str());
    s.audioEqualizationDelaySamples = std::vector<int64_t>(audioSources.size(), 0);
    for (size_t i = 0; i < audioSources.size(); ++i) {
        const double deltaMs = static_cast<double>(maxAudioCaptureLatencyMs) -
                               static_cast<double>(audioSources[i].config.captureLatencyMs);
        s.audioEqualizationDelaySamples[i] =
            deltaMs > 0.0 ? static_cast<int64_t>(std::llround(deltaMs / 1000.0 * 48000.0)) : 0;
        if (s.audioEqualizationDelaySamples[i] > 0) {
            DLL_Log(
                "[AudioLoop] A/V equalization: src=%zu captureLatencyMs=%.3f delaySamples=%lld (%.1f ms) to "
                "match maxLatencyMs=%.3f",
                i, static_cast<double>(audioSources[i].config.captureLatencyMs),
                (long long)s.audioEqualizationDelaySamples[i],
                static_cast<double>(s.audioEqualizationDelaySamples[i]) * 1000.0 / 48000.0,
                static_cast<double>(maxAudioCaptureLatencyMs));
        }
    }

return true;
}
