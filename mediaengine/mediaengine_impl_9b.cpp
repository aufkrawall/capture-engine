#include "mediaengine_internal.h"

bool MediaEngine::AudioLoopIteration(AudioLoopState& s) {
    constexpr int64_t kAudioWorkerSchedulingGapThresholdUs = AudioLoopState::kAudioWorkerSchedulingGapThresholdUs;

    auto& lastAudioWorkerIteration = s.lastAudioWorkerIteration;
    auto& audioWorkerSchedulingDiagnosticsArmTime = s.audioWorkerSchedulingDiagnosticsArmTime;
    auto& audioWorkerSchedulingGapEvents = s.audioWorkerSchedulingGapEvents;
    auto& audioWorkerSchedulingGapMaxUs = s.audioWorkerSchedulingGapMaxUs;
    auto& lastAudioWorkerSchedulingGapLogMs = s.lastAudioWorkerSchedulingGapLogMs;
    auto& appliedAudioResetGeneration = s.appliedAudioResetGeneration;
    auto& sourceTimestamps = s.sourceTimestamps;
    auto& sourceLoggedPreStartDrop = s.sourceLoggedPreStartDrop;
    auto& deferredFirstTimelinePacketValid = s.deferredFirstTimelinePacketValid;
    auto& deferredFirstTimelinePacketStartSamples = s.deferredFirstTimelinePacketStartSamples;
    auto& sourceCaptureEpochs = s.sourceCaptureEpochs;
    auto& sourceLastPackets = s.sourceLastPackets;
    auto& lastPacketTime = s.lastPacketTime;
    auto& pendingEpochPackets = s.pendingEpochPackets;
    auto& captureFanoutQueues = s.captureFanoutQueues;
    auto& trackNextTimestamp = s.trackNextTimestamp;
    auto& sharedStartupRebaseOffsetSamples = s.sharedStartupRebaseOffsetSamples;
    auto& audioOnlyStopTailFinalized = s.audioOnlyStopTailFinalized;
    auto& gotAnyPacket = s.gotAnyPacket;
    auto& now = s.now;
    auto& mixCount = s.mixCount;

            const auto audioWorkerIterationNow = std::chrono::steady_clock::now();
            const int64_t audioWorkerSchedulingGapUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                                           audioWorkerIterationNow - lastAudioWorkerIteration)
                                                           .count();
            lastAudioWorkerIteration = audioWorkerIterationNow;
            const uint64_t requestedResetGeneration = audioResetRequestedGeneration.load(std::memory_order_acquire);
            const bool audioTimelineCommitted =
                requestedResetGeneration <= audioResetCommittedGeneration.load(std::memory_order_acquire);
            if (audioTimelineCommitted && audioWorkerIterationNow >= audioWorkerSchedulingDiagnosticsArmTime &&
                recordingStartSystemQPCMs.load(std::memory_order_acquire) > 0 &&
                !audioStopDrainRequested.load(std::memory_order_acquire) &&
                audioWorkerSchedulingGapUs >= kAudioWorkerSchedulingGapThresholdUs) {
                ++audioWorkerSchedulingGapEvents;
                audioWorkerSchedulingGapMaxUs =
                    std::max(audioWorkerSchedulingGapMaxUs, audioWorkerSchedulingGapUs);
                const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          audioWorkerIterationNow.time_since_epoch())
                                          .count();
                if (audioWorkerSchedulingGapEvents <= 5 || nowMs - lastAudioWorkerSchedulingGapLogMs >= 1000) {
                    size_t maxRingSamples = 0;
                    size_t pendingFanoutPackets = 0;
                    size_t pendingEpochPacketCount = 0;
                    for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                        const auto& src = audioSources[srcIdx];
                        if (src.ringBuffer) {
                            const size_t channels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));
                            maxRingSamples = std::max(maxRingSamples, src.ringBuffer->GetAvailable() / channels);
                        }
                        pendingFanoutPackets += captureFanoutQueues[srcIdx].size();
                        pendingEpochPacketCount += pendingEpochPackets[srcIdx].size();
                    }
                    DLL_Log(
                        "[AudioLoop] Scheduling gap: gap=%lldus threshold=%lldus max=%lldus events=%llu "
                        "maxRing=%zu samples fanoutPending=%zu epochPending=%zu; packet continuity counters remain "
                        "authoritative for audible-loss classification",
                        (long long)audioWorkerSchedulingGapUs, (long long)kAudioWorkerSchedulingGapThresholdUs,
                        (long long)audioWorkerSchedulingGapMaxUs,
                        static_cast<unsigned long long>(audioWorkerSchedulingGapEvents), maxRingSamples,
                        pendingFanoutPackets, pendingEpochPacketCount);
                    lastAudioWorkerSchedulingGapLogMs = nowMs;
                }
            }
            if (requestedResetGeneration > appliedAudioResetGeneration) {
                const bool preserveResetPackets = audioResetPreservePackets.load(std::memory_order_acquire);
                ApplyAudioTimelineReset(requestedResetGeneration,
                                        recordingStartSystemQPCMs.load(std::memory_order_acquire),
                                        preserveResetPackets);
                std::fill(sourceTimestamps.begin(), sourceTimestamps.end(), 0);
                std::fill(sourceLoggedPreStartDrop.begin(), sourceLoggedPreStartDrop.end(), false);
                std::fill(deferredFirstTimelinePacketValid.begin(), deferredFirstTimelinePacketValid.end(), false);
                std::fill(deferredFirstTimelinePacketStartSamples.begin(),
                          deferredFirstTimelinePacketStartSamples.end(), 0);
                std::fill(sourceCaptureEpochs.begin(), sourceCaptureEpochs.end(), 0);
                std::fill(sourceLastPackets.begin(), sourceLastPackets.end(), AudioPacket{});
                std::fill(lastPacketTime.begin(), lastPacketTime.end(), std::chrono::steady_clock::now());
                for (auto& queue : pendingEpochPackets) {
                    queue.clear();
                }
                if (!preserveResetPackets) {
                    for (auto& queue : captureFanoutQueues) {
                        queue.clear();
                    }
                }
                trackNextTimestamp.clear();
                sharedStartupRebaseOffsetSamples = -1;
                audioOnlyStopTailFinalized = false;
                lastAudioWorkerIteration = std::chrono::steady_clock::now();
                audioWorkerSchedulingDiagnosticsArmTime = lastAudioWorkerIteration + std::chrono::seconds(1);
                appliedAudioResetGeneration = requestedResetGeneration;
                audioResetAcknowledgedGeneration.store(requestedResetGeneration, std::memory_order_release);
                audioDrainCv.notify_all();
            }

            if (requestedResetGeneration > audioResetCommittedGeneration.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait(lock, [this, requestedResetGeneration]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           audioResetCommittedGeneration.load(std::memory_order_acquire) >= requestedResetGeneration ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                return true;
            }

            if (audioSyncPending.load(std::memory_order_acquire) &&
                preservePendingStartupAudioPackets.load(std::memory_order_acquire) &&
                !audioStopDrainRequested.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(audioDrainMutex);
                audioDrainCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return !audioRunning.load(std::memory_order_acquire) ||
                           !audioSyncPending.load(std::memory_order_acquire) ||
                           audioStopDrainRequested.load(std::memory_order_acquire);
                });
                return true;
            }

            gotAnyPacket = false;
            now = std::chrono::steady_clock::now();


        // Poll each source and process its packet as one semantic step;
        // the fetch/validation and the resample/ring-write halves live in
        // their own units so the loop body stays readable.
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); srcIdx++) {
            auto& src = audioSources[srcIdx];
            if (!src.sharedEncoderPtr)
                continue;
            if (!src.capture && !src.appCapture && captureFanoutQueues[srcIdx].empty())
                continue;
            if (!AudioLoopPollSource(s, srcIdx))
                continue;
            AudioLoopCommitSource(s, srcIdx);
        }

        // ===================================================================
        // PULL MODEL (Phase 2): Legacy audio mixing logic removed
        // ===================================================================

        // Audio-only recording has no video thread to drive the pull model.
        // Use the same per-track mixer as video recording so every source
        // sharing a track contributes exactly once and every exported track
        // advances from one coherent sample cursor.
        if (audioOnly && recording && !audioStopDrainRequested.load(std::memory_order_acquire)) {
            const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - recordingStartTime)
                                          .count();
            if (elapsedUs > 0) {
                PullAndEncodeAudio(elapsedUs);
                ++mixCount;
            }
        }

        if (gotAnyPacket) {
            audioDrainCv.notify_all();
        }

        if (!gotAnyPacket) {
            if (audioStopDrainRequested.load(std::memory_order_acquire)) {
                if (audioOnly && recording && !audioOnlyStopTailFinalized) {
                    // All capture queues are now empty. Flush both resampler
                    // layers on their owning audio worker before selecting the
                    // final shared-track endpoint, then force every track to
                    // that same sample count.
                    FlushAudioOnlyResamplerTails();

                    constexpr int64_t kMixerSampleRate = 48000;
                    const int64_t wallElapsedUs =
                        std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - recordingStartTime)
                                                 .count());
                    int64_t finalTargetSamples =
                        ce::audio::ComputeDurationUsToSamples(wallElapsedUs, kMixerSampleRate);
                    for (const auto& src : audioSources) {
                        finalTargetSamples = std::max<int64_t>(finalTargetSamples,
                                                               static_cast<int64_t>(src.qpcAlignedWrittenSamples));
                        finalTargetSamples = std::max(finalTargetSamples, src.syncSamplesOutput);
                    }
                    for (const auto& track : trackTimelineSamples) {
                        finalTargetSamples = std::max(finalTargetSamples, track.second);
                    }
                    const int64_t finalTargetUs =
                        (finalTargetSamples * 1000000ll + kMixerSampleRate - 1) / kMixerSampleRate;


                    auto minimumTrackCursor = [this]() -> int64_t {
                        if (cachedTrackToSources.empty()) {
                            return 0;
                        }
                        int64_t minimum = std::numeric_limits<int64_t>::max();
                        for (const auto& track : cachedTrackToSources) {
                            const auto cursor = trackTimelineSamples.find(track.first);
                            minimum = std::min(minimum, cursor != trackTimelineSamples.end() ? cursor->second : 0);
                        }
                        return minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum;
                    };

                    const int64_t beforeDrain = minimumTrackCursor();
                    const int64_t missingSamples = std::max<int64_t>(0, finalTargetSamples - beforeDrain);
                    const int64_t maxDrainIterations = std::max<int64_t>(
                        1, (missingSamples + (kMixerSampleRate / 2) - 1) / (kMixerSampleRate / 2) + 8);
                    int64_t drainIterations = 0;
                    for (; drainIterations < maxDrainIterations; ++drainIterations) {
                        const int64_t before = minimumTrackCursor();
                        if (before >= finalTargetSamples || cachedTrackToSources.empty()) {
                            break;
                        }
                        PullAndEncodeAudio(finalTargetUs, true);
                        const int64_t after = minimumTrackCursor();
                        if (after <= before) {
                            DLL_Log(
                                "[StopAudio] WARNING: audio-only final pull made no progress: target=%lld "
                                "minimum=%lld iteration=%lld/%lld",
                                finalTargetSamples, after, drainIterations + 1, maxDrainIterations);
                            break;
                        }
                    }
                    const int64_t afterDrain = minimumTrackCursor();
                    audioOnlyStopTailFinalized = true;
                    DLL_Log(
                        "[StopAudio] Audio-only mixed tail finalized: target=%lld samples wall=%lldus "
                        "minimumBefore=%lld minimumAfter=%lld iterations=%lld tracks=%zu",
                        finalTargetSamples, wallElapsedUs, beforeDrain, afterDrain, drainIterations,
                        cachedTrackToSources.size());
                }
                audioStopDrainComplete.store(true, std::memory_order_release);
                audioDrainCv.notify_all();
            }

            std::unique_lock<std::mutex> lock(audioDrainMutex);
            audioDrainCv.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                return !audioRunning.load(std::memory_order_acquire) ||
                       audioStopDrainRequested.load(std::memory_order_acquire);
            });
        }
return true;
}
