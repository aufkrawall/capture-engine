#include "mediaengine_internal.h"


size_t MediaEngine::StopAudioCaptureSources(bool discardPendingPackets) {


        std::lock_guard<std::mutex> stopLock(audioSourceStopMutex);
        size_t pendingAfterStop = 0;
        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->Stop(discardPendingPackets);
                if (!discardPendingPackets) {
                    pendingAfterStop += src.capture->PendingPacketCount();
                }
            }
            if (src.appCapture) {
                src.appCapture->Stop(discardPendingPackets);
                if (!discardPendingPackets) {
                    pendingAfterStop += src.appCapture->PendingPacketCount();
                }
            }
        }
        return pendingAfterStop;

}


MediaEngine::FinalSourceCatchupStatus MediaEngine::GetFinalCfrSourceCatchupStatus(int64_t targetUs) const {


        FinalSourceCatchupStatus status;
        if (!IsCfrRecording() || targetUs <= 0) {
            return status;
        }

        constexpr int kStopSampleRate = 48000;
        const int64_t targetSamples = ce::audio::ComputeDurationUsToSamples(targetUs, kStopSampleRate);
        for (const auto& kv : cachedTrackToSources) {
            const int track = kv.first;
            const auto trackIt = trackTimelineSamples.find(track);
            const int64_t trackCursorSamples = trackIt != trackTimelineSamples.end() ? trackIt->second : 0;
            const int64_t requestedSamples = targetSamples - trackCursorSamples;
            if (requestedSamples <= 0) {
                continue;
            }

            for (size_t srcIdx : kv.second) {
                if (srcIdx >= audioSources.size()) {
                    continue;
                }
                const auto& src = audioSources[srcIdx];
                if (!src.sharedEncoderPtr || !src.ringBuffer) {
                    continue;
                }

                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool appCaptureRouteEnded =
                    src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                const bool inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        true, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets, !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    true, src.timelineValid, src.bootstrapComplete, optionalUnstarted, true);
                const bool strictSource = src.sourceType != AudioConfig::Microphone;
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                const bool sparseStartedSourceMaySilence =
                    ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(sparseStartedSourceCanSilence,
                                                                                  bufferedTimelineSamples) ||
                    inactiveStartedAppSourceMaySilence;
                if (ce::audio::ShouldWaitForFinalCfrSourceCatchup(true, strictSource, optionalUnstarted,
                                                                  sparseStartedSourceMaySilence, requestedSamples,
                                                                  bufferedTimelineSamples)) {
                    status.ready = false;
                    status.sourceIndex = srcIdx;
                    status.track = track;
                    status.requestedSamples = requestedSamples;
                    status.bufferedSamples = bufferedTimelineSamples;
                    status.missingSamples =
                        requestedSamples -
                        static_cast<int64_t>(std::min<size_t>(
                            bufferedTimelineSamples, static_cast<size_t>(std::max<int64_t>(requestedSamples, 0))));
                    return status;

                }
            }
        }

        return status;

}


bool MediaEngine::WaitForFinalCfrAudioSourceCatchup(int64_t targetUs) {


        if (!ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(audioRunning.load(), audioOnly, targetUs) ||
            !IsCfrRecording()) {
            return true;
        }

        FinalSourceCatchupStatus status = GetFinalCfrSourceCatchupStatus(targetUs);
        if (status.ready) {
            return true;
        }

        constexpr auto kFinalCatchupMaxWait = std::chrono::milliseconds(500);
        const auto waitStart = std::chrono::steady_clock::now();
        const auto deadline = waitStart + kFinalCatchupMaxWait;
        DLL_Log(
            "[StopAudio] Waiting for final source catch-up: targetUs=%lld track=%d src=%zu requested=%lld "
            "buffered=%zu missing=%lld",
            targetUs, status.track, status.sourceIndex, status.requestedSamples, status.bufferedSamples,
            status.missingSamples);

        std::unique_lock<std::mutex> lock(audioDrainMutex);
        audioDrainCv.wait_until(lock, deadline, [this, targetUs, &status]() {
            if (!audioRunning.load(std::memory_order_acquire)) {
                return true;
            }
            status = GetFinalCfrSourceCatchupStatus(targetUs);
            return status.ready;
        });
        lock.unlock();

        status = GetFinalCfrSourceCatchupStatus(targetUs);
        const auto waitedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
        if (!status.ready) {
            DLL_Log(
                "[StopAudio] WARNING: Final source catch-up timed out after %lldms: targetUs=%lld track=%d src=%zu "
                "requested=%lld buffered=%zu missing=%lld. Final force drain may need silence padding.",
                static_cast<long long>(waitedMs), targetUs, status.track, status.sourceIndex, status.requestedSamples,
                status.bufferedSamples, status.missingSamples);
            return false;
        }

        DLL_Log("[StopAudio] Final source catch-up ready after %lldms for targetUs=%lld",
                static_cast<long long>(waitedMs), targetUs);
        return true;

}


size_t MediaEngine::StopCaptureSourcesAndDrainAudioLoop() {


        const size_t pendingPackets = StopAudioCaptureSources(false);

        audioStopDrainComplete.store(false, std::memory_order_release);
        audioStopDrainRequested.store(true, std::memory_order_release);
        audioDrainCv.notify_all();

        {
            std::unique_lock<std::mutex> lock(audioDrainMutex);
            audioDrainCv.wait(lock, [this]() {
                return audioStopDrainComplete.load(std::memory_order_acquire) || !audioRunning.load();
            });
        }

        audioStopDrainRequested.store(false, std::memory_order_release);
        DLL_Log("[StopAudio] Capture stop-drain completed: preservedPackets=%zu audioOnly=%d", pendingPackets,
                audioOnly ? 1 : 0);
        return pendingPackets;

}


void MediaEngine::AudioThreadEntry() noexcept {


        bool failed = false;
        try {
            AudioLoop();
        } catch (const std::exception& e) {
            failed = true;
            DLL_Log("[AudioLoop] ERROR: Unhandled exception escaped audio worker: %s", e.what());
        } catch (...) {
            failed = true;
            DLL_Log("[AudioLoop] ERROR: Unknown exception escaped audio worker");
        }

        if (failed) {
            // No consumer remains after an unexpected worker exit. Stop the
            // producers and discard their queues instead of allowing them to
            // grow indefinitely for the rest of the recording.
            StopAudioCaptureSources(true);
        }
        audioRunning.store(false, std::memory_order_release);
        audioStopDrainComplete.store(true, std::memory_order_release);
        audioDrainCv.notify_all();

}


bool MediaEngine::StartAudioThread() {


        audioStopDrainComplete.store(false, std::memory_order_release);
        audioRunning.store(true, std::memory_order_release);
        try {
            audioThread = std::thread(&MediaEngine::AudioThreadEntry, this);
            return true;
        } catch (const std::exception& e) {
            DLL_Log("[AudioLoop] ERROR: Failed to create audio worker: %s", e.what());
        } catch (...) {
            DLL_Log("[AudioLoop] ERROR: Failed to create audio worker with unknown exception");
        }

        audioRunning.store(false, std::memory_order_release);
        audioStopDrainComplete.store(true, std::memory_order_release);
        audioDrainCv.notify_all();
        StopAudioCaptureSources(true);
        return false;

}


void MediaEngine::DrainStoppedCaptureQueuesBeforeFinalPull(int64_t targetUs) {


        if (!ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(audioRunning.load(), audioOnly, targetUs)) {
            return;
        }

        StopCaptureSourcesAndDrainAudioLoop();

}


void MediaEngine::ApplyAudioTimelineReset(uint64_t generation,  int64_t startQpcMs,  bool preservePendingPackets) {


        // Discard anything captured before the first video frame for the normal
        // low-latency start. WGC CFR smoothness startup is the exception: the
        // selected video frame is deliberately older, so queued post-anchor audio
        // is the content that matches that frame and must be preserved.
        if (!preservePendingPackets) {
            DiscardPendingAudioPackets();
        }
        std::set<AudioEncoder*> resetEncoders;
        for (auto& src : audioSources) {
            if (src.sharedEncoderPtr && resetEncoders.insert(src.sharedEncoderPtr).second) {
                src.sharedEncoderPtr->ResetForRecordingStart(0, generation);
            }
            if (src.ringBuffer) {
                src.ringBuffer->Clear();
            }
            if (src.syncResampler) {
                src.syncResampler->Reset();
            }
            // AudioLoop may have already preprocessed packets while waiting for the first
            // video frame. Drop that conversion state too so the track starts from a clean
            // audio timeline anchor.
            src.resampler.reset();
            src.postResampleBuffer.clear();
            src.syncSamplesOutput = 0;
            src.dropFadeSamplesRemaining = 0;
            src.dropFadeStartL = 0.0f;
            src.dropFadeStartR = 0.0f;
            src.dropFadeStart.clear();
            src.underrunFadeSamplesRemaining = 0;
            src.packetBoundaryFadeInSamplesRemaining = 0;
            src.overflowDropSamples = 0;
            src.retainedNewestTrimSamples = 0;
            src.latencyTrimSamples = 0;
            src.tier2TrimSamples = 0;
            src.bootstrapTrimSamples = 0;
            src.postResampleTrimSamples = 0;
            src.underrunPadSamples = 0;
            src.coverageLossTrimSamples = 0;
            src.startupSyntheticRingSamples = 0;
            src.startupSyntheticResamplerSamples = 0;
            src.startupSyntheticPostSamples = 0;
            src.startupGapProtectionSamples = 0;
            src.qpcAlignedWrittenSamples = 0;
            src.packetTimelineGapSamples = 0;
            src.packetTimelineOverlapSamples = 0;
            src.startupRebasedGapSamples = 0;
            src.lateAppJoinSuppressedGapSamples = 0;
            src.lateAppJoinPreservedGapSamples = 0;
            src.alignedStartMs = -1;
            src.observedLateStartMs = 0;
            src.hasAlignedStart = false;
            src.sawCaptureEpoch = false;
            const bool appCaptureRouteEnded =
                src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
            src.timelineValid = src.sourceType != AudioConfig::AppAudio || appCaptureRouteEnded;
            src.isPrimed = false;
            src.bootstrapComplete = false;
            src.pendingUnderrunRecoveryFade = false;
            src.startupRealAudioSeen = false;
            src.pendingStartupJoinFade = false;
            src.pendingRetainedTrimSamples = 0;
            src.pendingRetainedTrimEvents = 0;
            src.pendingLatencyTrimSamples = 0;
            src.pendingLatencyTrimEvents = 0;
            src.pendingTier2TrimSamples = 0;
            src.pendingTier2TrimEvents = 0;
            src.pendingCoverageLossTrimSamples = 0;
            src.pendingCoverageLossTrimEvents = 0;
            src.lastAppPlaceDiagTick = 0;
            src.lastAppConsumeDiagTick = 0;
            src.appLatencyBuckets[0] = 0;
            src.appLatencyBuckets[1] = 0;
            src.appLatencyBuckets[2] = 0;
            src.appLatencyBuckets[3] = 0;
            src.appLatencyBuckets[4] = 0;
            src.appLatencySampleCount = 0;
            src.appLatencySumMs = 0;
            src.appLatencyMaxMs = 0;
            src.appLatencyTargetSumMs = 0;
            src.appLatencyExcessSumMs = 0;
            src.appLatencyExcessMaxMs = 0;
            src.appLatencyDrainingSamples = 0;
            src.appLatencyStopDrainSampleCount = 0;
            src.appLatencyStopDrainSumMs = 0;
            src.appLatencyStopDrainMaxMs = 0;
            src.appLatencyDrainTransitions = 0;
            src.appLatencyMaxAbsCompDelta = 0;
            src.lastAppLatencyWarnTick = 0;
            src.appLatencyWarnActive = false;
            src.appAudioBacklogDrainInitialized = false;
            src.appAudioBacklogDrainActive = false;
            src.appAudioBacklogDrainReason =
                static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::SourceBootstrapPending);
            src.appAudioBacklogTargetSamples = 0;
            src.appAudioBacklogExcessSamples = 0;
            src.appAudioBacklogCompensationDelta = 0;
            src.catastrophicResyncSamples = 0;
            src.catastrophicResyncEvents = 0;
            src.lastRetainedTrimWarnTick = 0;
            src.lastPacketTimelineAdjustWarnTick = 0;
            src.lastRealPacketIngestTick = 0;
            src.timelineStarvationDropSamples = 0;
            src.timelineStarvationBeganTick = 0;
            src.lastTimelineStarvationWarnTick = 0;
            src.timelineResyncOffsetSamples = 0;
            src.timelineResyncSuppressedSamples = 0;
            src.timelineResyncEvents = 0;
            src.wgcCoverageLossTrimAccumulator = 0.0;
            src.prevLeadSamples = 0;
            src.prevLeadSnapshotMs = 0;
            src.lastRateUpdateMs = 0;
            src.currentRateDelta = 0;
            src.targetRateDelta = 0;
            src.rateCompActive = false;
            src.targetRateSaturated = false;
            src.timelineResetGeneration = generation;
            src.epochResetRequested->store(0, std::memory_order_release);
            src.epochResetAcknowledged->store(0, std::memory_order_release);
            src.epochSyncTailFlushedGeneration = 0;
            src.lastEpochTransitionWaitLogTick = 0;
            DLL_Log(
                "[A/V START] Audio route acknowledged reset: generation=%llu src=%zu track=%d type=%d "
                "startMs=%lld timelineValid=%d captureLatencyMs=%.3f",
                static_cast<unsigned long long>(generation), static_cast<size_t>(&src - audioSources.data()), src.track,
                static_cast<int>(src.sourceType), startQpcMs, src.timelineValid ? 1 : 0,
                static_cast<double>(src.config.captureLatencyMs));
        }
        DLL_Log("[A/V START] Reset generation=%llu applied atomically: routes=%zu encoders=%zu preservePending=%d",
                static_cast<unsigned long long>(generation), audioSources.size(), resetEncoders.size(),
                preservePendingPackets ? 1 : 0);

}


void MediaEngine::AudioLoop() {
    AudioLoopState s;
    if (!AudioLoopInit(s))
        return;
    while (audioRunning) {
        AudioLoopIteration(s);
    }
    AudioLoopTail(s);
}

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
