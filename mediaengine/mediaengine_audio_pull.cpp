#include "mediaengine_internal.h"


bool MediaEngine::ServiceAudioEpochResetOnPull(AudioSource& src,  size_t srcIdx) {


        if (!src.epochResetRequested || !src.epochResetAcknowledged) {
            return false;
        }
        const uint64_t requested = src.epochResetRequested->load(std::memory_order_acquire);
        const uint64_t acknowledged = src.epochResetAcknowledged->load(std::memory_order_acquire);
        if (requested == 0 || requested == acknowledged) {
            return false;
        }

        const int channels = std::clamp(src.mixChannels, 1, 8);
        if (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
            return false;
        }

        if (src.epochSyncTailFlushedGeneration != requested) {
            size_t syncTailSamples = 0;
            constexpr size_t kMaxFlushPasses = 8;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                bool flushComplete = false;
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult == AudioResampler::FlushResult::Error) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        DLL_Log(
                            "[AudioEpoch] ERROR: Sync-resampler flush failed src=%zu track=%d generation=%llu "
                            "flushed=%zu",
                            srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples);
                        return false;
                    }
                    if (flushResult == AudioResampler::FlushResult::Complete) {
                        flushComplete = true;
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
                if (!flushComplete) {
                    DLL_Log(
                        "[AudioEpoch] Sync-resampler flush pass bound reached src=%zu track=%d generation=%llu "
                        "flushed=%zu reportedDelay=%lld; continuing on the next pull pass",
                        srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                        static_cast<long long>(std::max<int64_t>(0, src.syncResampler->GetDelay())));
                    return false;
                }
            }
            src.epochSyncTailFlushedGeneration = requested;
            DLL_Log(
                "[AudioEpoch] Pull owner drained sync tail src=%zu track=%d generation=%llu syncTail=%zu "
                "postBuffered=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                src.postResampleBuffer.size() / static_cast<size_t>(channels));
        }

        if (!src.postResampleBuffer.empty()) {
            return false;
        }
        if (src.syncResampler && src.syncResampler->IsReady() && !src.syncResampler->Reset()) {
            DLL_Log("[AudioEpoch] ERROR: Pull-owner sync reset failed src=%zu track=%d generation=%llu", srcIdx,
                    src.track, static_cast<unsigned long long>(requested));
            return false;
        }

        src.startupSyntheticResamplerSamples = 0;
        src.startupSyntheticPostSamples = 0;
        src.dropFadeSamplesRemaining = 0;
        src.dropFadeStartL = 0.0f;
        src.dropFadeStartR = 0.0f;
        src.dropFadeStart.clear();
        src.underrunFadeSamplesRemaining = 0;
        src.pendingUnderrunRecoveryFade = true;
        src.appAudioBacklogDrainInitialized = false;
        src.appAudioBacklogDrainActive = false;
        src.appAudioBacklogDrainReason =
            static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::EpochRejoinPending);
        src.appAudioBacklogTargetSamples = 0;
        src.appAudioBacklogExcessSamples = 0;
        src.appAudioBacklogCompensationDelta = 0;
        src.prevLeadSamples = 0;
        src.prevLeadSnapshotMs = 0;
        src.lastRateUpdateMs = 0;
        src.currentRateDelta = 0;
        src.targetRateDelta = 0;
        src.rateCompActive = false;
        src.targetRateSaturated = false;
        src.epochResetAcknowledged->store(requested, std::memory_order_release);
        audioDrainCv.notify_all();
        DLL_Log(
            "[AudioEpoch] Pull owner acknowledged reset src=%zu track=%d generation=%llu ring=0 post=0 "
            "trackCursor=%lld sourceEncoded=%lld",
            srcIdx, src.track, static_cast<unsigned long long>(requested),
            static_cast<long long>(trackTimelineSamples[src.track]),
            srcIdx < encodedSamplesPerSource.size() ? static_cast<long long>(encodedSamplesPerSource[srcIdx]) : -1ll);
        return true;

}


void MediaEngine::FlushAudioOnlyResamplerTails() {


        constexpr size_t kMaxFlushPasses = 8;
        constexpr size_t kSyncPumpSamples = 4800;
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            auto& src = audioSources[srcIdx];
            const TrackAudioFormat trackFormat = GetTrackAudioFormat(src.track);
            const int channels = std::clamp(trackFormat.channels, 1, 8);

            size_t captureTailSamples = 0;
            if (src.resampler && src.resampler->IsReady() && src.ringBuffer) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.resampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Capture-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    if (tailSamples > 0 && tailData && tailData[0]) {
                        float* tailFloats = reinterpret_cast<float*>(tailData[0]);
                        if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                            const size_t fadeSamples = static_cast<size_t>(src.packetBoundaryFadeInSamplesRemaining);
                            ApplyPacketBoundaryFadeIn(tailFloats, static_cast<size_t>(tailSamples),
                                                      static_cast<size_t>(channels), fadeSamples);
                            src.packetBoundaryFadeInSamplesRemaining =
                                fadeSamples > static_cast<size_t>(tailSamples)
                                    ? static_cast<int>(fadeSamples - static_cast<size_t>(tailSamples))
                                    : 0;
                        }
                        const size_t writtenFloats = src.ringBuffer->WriteRetainNew(
                            tailFloats, static_cast<size_t>(tailSamples) * static_cast<size_t>(channels));
                        const size_t writtenSamples = writtenFloats / static_cast<size_t>(channels);
                        src.qpcAlignedWrittenSamples += writtenSamples;
                        captureTailSamples += writtenSamples;
                    }
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            size_t syncInputSamples = 0;
            while (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
                const size_t before = src.ringBuffer->GetAvailable();
                if (!PumpSourceRingThroughSyncResampler(src, srcIdx, channels,
                                                        kSyncPumpSamples * static_cast<size_t>(channels))) {
                    break;
                }
                syncInputSamples += (before - src.ringBuffer->GetAvailable()) / static_cast<size_t>(channels);
            }

            size_t syncTailSamples = 0;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Sync-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            if (captureTailSamples > 0 || syncTailSamples > 0) {
                DLL_Log(
                    "[StopAudio] Flushed audio-only resampler tails: src=%zu track=%d captureTail=%zu "
                    "syncInput=%zu syncTail=%zu postBuffered=%zu",
                    srcIdx, src.track, captureTailSamples, syncInputSamples, syncTailSamples,
                    src.postResampleBuffer.size() / static_cast<size_t>(channels));
            }
        }

}


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

bool MediaEngine::PullTrackBootstrap(AudioPullState& s, int track, const std::vector<size_t>& srcIndices) {
    auto& isCfrRecording = s.isCfrRecording;
    auto& effectiveAudioPullLatencyMs = s.effectiveAudioPullLatencyMs;
    auto& baseTargetLatencySamples = s.baseTargetLatencySamples;
    auto& audioTargetUs = s.audioTargetUs;
    auto& effectiveAudioTargetBufferLagMs = s.effectiveAudioTargetBufferLagMs;
    auto& targetBufferedLagCapMs = s.targetBufferedLagCapMs;
    auto& trackFormat = s.trackFormat;
    auto& CHANNELS = s.CHANNELS;
    auto& CHANNEL_MASK = s.CHANNEL_MASK;
    auto& trackAllPrimed = s.trackAllPrimed;
    auto& trackMaxObservedLateStartMs = s.trackMaxObservedLateStartMs;
    auto& trackStartupSettled = s.trackStartupSettled;
    auto& trackAudioPullLatencyMs = s.trackAudioPullLatencyMs;
    auto& trackAudioTargetUs = s.trackAudioTargetUs;
    auto& trackAudioTargetMs = s.trackAudioTargetMs;
    auto& targetSamples = s.targetSamples;
    auto& targetBufferedSamples = s.targetBufferedSamples;
    auto& trackReadyForBootstrap = s.trackReadyForBootstrap;
    auto& requiredBootstrapSamples = s.requiredBootstrapSamples;
    auto& firstSrcIdx = s.firstSrcIdx;
    auto& pendingSamples = s.pendingSamples;
    auto& isAppAudioSource = s.isAppAudioSource;
    auto& primedSampleCount = s.primedSampleCount;
    auto& bufferedRealSamples = s.bufferedRealSamples;
    auto& srcReady = s.srcReady;
    auto& optionalUnstarted = s.optionalUnstarted;
    auto& packetlessSilenceReady = s.packetlessSilenceReady;
    auto& bufferedTimelineSamples = s.bufferedTimelineSamples;
    auto& srcTimelineReady = s.srcTimelineReady;
    auto& forcedBootstrap = s.forcedBootstrap;
    auto& bootstrapTrimmed = s.bootstrapTrimmed;
    auto& bootstrapProtected = s.bootstrapProtected;
    auto& remainingStartupProtectionSamples = s.remainingStartupProtectionSamples;
    auto& forceDrain = s.forceDrain;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kPrimedSourceCushionSamples = AudioPullState::kPrimedSourceCushionSamples;
    constexpr size_t kMinBootstrapRealSamples = AudioPullState::kMinBootstrapRealSamples;
        auto& trackCursorSamples = trackTimelineSamples[track];
            trackFormat = GetTrackAudioFormat(track);
            CHANNELS = std::clamp(trackFormat.channels, 1, 8);
            CHANNEL_MASK =
                trackFormat.channelMask != 0 ? trackFormat.channelMask : DefaultChannelMaskForChannels(CHANNELS);

            trackAllPrimed = true;
            trackMaxObservedLateStartMs = 0;
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);

                primedSampleCount = ce::audio::ComputeBufferedRealAudioSamples(
                    src.postResampleBuffer.size() / CHANNELS, src.startupSyntheticPostSamples);
                if (src.ringBuffer) {
                    primedSampleCount += ce::audio::ComputeBufferedRealAudioSamples(
                        src.ringBuffer->GetAvailable() / CHANNELS, src.startupSyntheticRingSamples);
                }
                if (src.hasAlignedStart && !src.isPrimed &&
                    primedSampleCount >= static_cast<size_t>(kPrimedSourceCushionSamples)) {
                    src.isPrimed = true;
                    DLL_Log(
                        "[PullAudio] Source primed - src=%d realBuffered=%zu samples synthetic(ring=%llu inflight=%llu "
                        "post=%llu) lateStart=%lldms app=%d",
                        (int)srcIdx, primedSampleCount, (unsigned long long)src.startupSyntheticRingSamples,
                        (unsigned long long)src.startupSyntheticResamplerSamples,
                        (unsigned long long)src.startupSyntheticPostSamples, src.observedLateStartMs,
                        isAppAudioSource ? 1 : 0);
                }

                if (ce::audio::ShouldRestoreSettledSourceBootstrap(
                        trackBootstrapComplete[track], src.timelineValid, src.isPrimed, src.bootstrapComplete)) {
                    src.bootstrapComplete = true;
                    DLL_Log(
                        "[AudioEpoch] WARNING: restored recording-sticky source bootstrap eligibility "
                        "src=%zu track=%d timelineValid=1 primed=1 trackBootstrap=1 realBuffered=%zu. "
                        "This liveness recovery prevents an epoch rejoin from blocking the settled CFR track.",
                        srcIdx, track, primedSampleCount);
                }

                trackAllPrimed =
                    trackAllPrimed && ce::audio::IsSourceStartupPrimed(src.isPrimed, src.timelineValid,
                                                                       isAppAudioSource, src.sawSyncPendingPackets);
                trackMaxObservedLateStartMs = std::max(trackMaxObservedLateStartMs, src.observedLateStartMs);
            }

            trackStartupSettled =
                ce::audio::IsTrackAudioStartupSettled(trackBootstrapComplete[track], trackAllPrimed);
            trackAudioPullLatencyMs =
                forceDrain ? 0
                           : ce::audio::ComputeAudioPullLatencyMs(effectiveAudioPullLatencyMs, trackStartupSettled,
                                                                  trackMaxObservedLateStartMs);
            if (isCfrRecording) {
                trackAudioPullLatencyMs = ce::audio::ComputeSettledCfrAudioPullLatencyMs(
                    trackAudioPullLatencyMs, trackStartupSettled, trackAllPrimed, effectiveAudioPullLatencyMs);
            }
            trackAudioTargetUs = audioTargetUs - (std::max<int64_t>(trackAudioPullLatencyMs, 0) * 1000);
            trackAudioTargetMs = trackAudioTargetUs > 0 ? (trackAudioTargetUs / 1000) : 0;
            if (trackAudioTargetUs <= 0) {
                return false;
            }

            targetSamples = ce::audio::ComputeDurationUsToSamples(trackAudioTargetUs, SAMPLE_RATE);
            targetBufferedSamples = ce::audio::ComputeBufferedAudioTargetSamples(
                SAMPLE_RATE, baseTargetLatencySamples, effectiveAudioTargetBufferLagMs, targetBufferedLagCapMs);

            trackReadyForBootstrap = true;
            requiredBootstrapSamples =
                ce::audio::ComputeRequiredBootstrapRealSamples(targetSamples, kMinBootstrapRealSamples);
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                bufferedRealSamples = ce::audio::ComputeBufferedRealAudioSamples(
                    src.postResampleBuffer.size() / CHANNELS, src.startupSyntheticPostSamples);
                if (src.ringBuffer) {
                    bufferedRealSamples += ce::audio::ComputeBufferedRealAudioSamples(
                        src.ringBuffer->GetAvailable() / CHANNELS, src.startupSyntheticRingSamples);
                }
                srcReady = ce::audio::IsSourceBootstrapReady(
                    src.bootstrapComplete, src.timelineValid, src.isPrimed, isAppAudioSource, bufferedRealSamples,
                    requiredBootstrapSamples, src.sawSyncPendingPackets);
                optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                packetlessSilenceReady = ce::audio::ShouldBootstrapPacketlessSourceAsSilence(
                    isCfrRecording, src.timelineValid, bufferedRealSamples, targetSamples, requiredBootstrapSamples);
                bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                srcTimelineReady = ce::audio::IsSourceBootstrapTimelineReady(
                    src.bootstrapComplete, optionalUnstarted, srcReady, packetlessSilenceReady, bufferedTimelineSamples,
                    targetSamples);
                trackReadyForBootstrap = trackReadyForBootstrap && srcTimelineReady;
            }

            if (!trackBootstrapComplete[track]) {
                forcedBootstrap = forceDrain;
                if (!trackReadyForBootstrap && !forcedBootstrap) {
                    int& waitLogCounter = trackBootstrapWaitLogCounters[track];
                    if (waitLogCounter++ % 120 == 0) {
                        DLL_Log("[PullAudio] Track %d bootstrap pending - target=%lldms samples=%lld", track,
                                trackAudioTargetMs, targetSamples);
                    }
                    return false;
                }

                bootstrapTrimmed = 0;
                bootstrapProtected = 0;
                for (size_t srcIdx : srcIndices) {
                    auto& src = audioSources[srcIdx];
                    const int64_t remainingStartupProtectionSamples = std::max<int64_t>(
                        0, static_cast<int64_t>(src.startupGapProtectionSamples) - encodedSamplesPerSource[srcIdx]);
                    bootstrapProtected += static_cast<uint64_t>(remainingStartupProtectionSamples);
                    src.bootstrapComplete = true;
                }

                trackBootstrapComplete[track] = true;
                trackFirstPullAfterBootstrap[track] = true;
                trackBootstrapWaitLogCounters[track] = 0;
                DLL_Log(
                    "[PullAudio] Track %d bootstrap complete - target=%lldms samples=%lld forced=%d trimmed=%llu "
                    "protected=%llu lookahead=%lldms",
                    track, trackAudioTargetMs, targetSamples, forcedBootstrap ? 1 : 0,
                    (unsigned long long)bootstrapTrimmed, (unsigned long long)bootstrapProtected,
                    trackAudioPullLatencyMs);
            }

            firstSrcIdx = srcIndices[0];
            pendingSamples = targetSamples - trackCursorSamples;
            if (pendingSamples <= 0)
                return false;

    return true;
}
bool MediaEngine::PullTrackGapAndBuffer(AudioPullState& s, int track, const std::vector<size_t>& srcIndices) {
    auto& isCfrRecording = s.isCfrRecording;
    auto& isWgcCfrRecording = s.isWgcCfrRecording;
    auto& pullTick = s.pullTick;
    auto& reservoirPassBaseMs = s.reservoirPassBaseMs;
    auto& timelineShortfallMs = s.timelineShortfallMs;
    auto& wgcOverloadFlags = s.wgcOverloadFlags;
    auto& CHANNELS = s.CHANNELS;
    auto& trackStartupSettled = s.trackStartupSettled;
    auto& trackAudioPullLatencyMs = s.trackAudioPullLatencyMs;
    auto& trackAudioTargetMs = s.trackAudioTargetMs;
    auto& targetSamples = s.targetSamples;
    auto& pendingSamples = s.pendingSamples;
    auto& initialTrackCatchup = s.initialTrackCatchup;
    auto& overloadPullQuantum = s.overloadPullQuantum;
    auto& samplesToEncode = s.samplesToEncode;
    auto& MAX_GAP_SAMPLES = s.MAX_GAP_SAMPLES;
    auto& MAX_SILENCE_CHUNK = s.MAX_SILENCE_CHUNK;
    auto& trackLargeBacklogDrain = s.trackLargeBacklogDrain;
    auto& deferForSourceBuffer = s.deferForSourceBuffer;
    auto& finalStopDrain = s.finalStopDrain;
    auto& totalFloats = s.totalFloats;
    auto& mixBuffer = s.mixBuffer;
    auto& activeSources = s.activeSources;
    auto& eligibleSources = s.eligibleSources;
    auto& isAppAudioSource = s.isAppAudioSource;
    auto& optionalUnstarted = s.optionalUnstarted;
    auto& bufferedTimelineSamples = s.bufferedTimelineSamples;
    auto& appCaptureRouteEnded = s.appCaptureRouteEnded;
    auto& inactiveStartedAppSourceMaySilence = s.inactiveStartedAppSourceMaySilence;
    auto& sparseStartedSourceCanSilence = s.sparseStartedSourceCanSilence;
    auto& sparseStartedSourceMaySilence = s.sparseStartedSourceMaySilence;
    auto& sourceIngestIdleMs = s.sourceIngestIdleMs;
    auto& sourceRecentlyDeliveredRealPackets = s.sourceRecentlyDeliveredRealPackets;
    auto& available = s.available;
    auto& it = s.it;
    auto& forceDrain = s.forceDrain;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kOverloadAudioPullQuantumSamples = AudioPullState::kOverloadAudioPullQuantumSamples;
    constexpr int64_t kSparseStartedPartialSilenceThresholdSamples = AudioPullState::kSparseStartedPartialSilenceThresholdSamples;
        auto& trackCursorSamples = trackTimelineSamples[track];
            initialTrackCatchup =
                trackFirstPullAfterBootstrap.count(track) && trackFirstPullAfterBootstrap[track];
            overloadPullQuantum = (isWgcCfrRecording && (wgcOverloadFlags & 0x1u) != 0) ||
                                             (isCfrRecording && !isWgcCfrRecording && timelineShortfallMs > 100);
            samplesToEncode = ce::audio::ComputeAudioSamplesToEncode(
                pendingSamples, isCfrRecording, trackStartupSettled, forceDrain, initialTrackCatchup,
                overloadPullQuantum, ce::audio::kDefaultAudioPullQuantumSamples, kOverloadAudioPullQuantumSamples);
            if (samplesToEncode <= 0) {
                return false;
            }
            if (initialTrackCatchup) {
                trackFirstPullAfterBootstrap[track] = false;
            }

            MAX_GAP_SAMPLES = (SAMPLE_RATE * 2);
            MAX_SILENCE_CHUNK = SAMPLE_RATE / 2;
            // A track this far behind the live target is catching up after a read-stall (alt-tab, DPC
            // spike, encoder overload). It MUST make forward progress every pull: deferring to wait for
            // an under-buffered source would freeze it forever, because a co-mixed source sitting at the
            // live edge (e.g. a second app keeping up at ~100ms) NEVER accrues the full catch-up chunk.
            // That freeze is exactly how a multi-app track went permanently silent after one app's
            // backlog stalled it - the track deferred every iteration, its cursor froze, and the stalled
            // app's ring saturated. Only fires when >2s behind (a real stall), so normal pulls keep the
            // defer/buffer-wait protection untouched.
            trackLargeBacklogDrain =
                ce::audio::ShouldSuppressBufferDeferForCatchup(samplesToEncode, MAX_GAP_SAMPLES, initialTrackCatchup);
            if (samplesToEncode > MAX_GAP_SAMPLES && !initialTrackCatchup && !isCfrRecording) {
                warpCount++;
                DLL_Log(
                    "[PullAudio] Large A/V gap (%.2f sec) on track %d - inserting silence (warp #%d). target=%lld, "
                    "encoded=%lld",
                    (double)samplesToEncode / SAMPLE_RATE, track, warpCount, targetSamples, trackCursorSamples);

                for (size_t srcIdx : srcIndices) {
                    if (audioSources[srcIdx].ringBuffer) {
                        size_t skippedFloats =
                            audioSources[srcIdx].ringBuffer->Skip(audioSources[srcIdx].ringBuffer->GetAvailable());
                        ce::audio::ConsumeSyntheticBufferedSamples(audioSources[srcIdx].startupSyntheticRingSamples,
                                                                   skippedFloats / CHANNELS);
                        ce::audio::ConsumeSyntheticBufferedSamples(audioSources[srcIdx].startupGapProtectionSamples,
                                                                   skippedFloats / CHANNELS);
                    }
                }

                samplesToEncode = std::min(samplesToEncode, MAX_SILENCE_CHUNK);
            } else if (samplesToEncode > MAX_GAP_SAMPLES && !initialTrackCatchup) {
                warpCount++;
                samplesToEncode = std::min(samplesToEncode, MAX_SILENCE_CHUNK);
                if (dropLogCounter++ % 20 == 0) {
                    DLL_Log(
                        "[PullAudio] Large CFR audio backlog (%.2f sec) on track %d - draining in bounded chunks "
                        "without dropping buffered audio (chunk=%lld samples, target=%lld, encoded=%lld)",
                        (double)pendingSamples / SAMPLE_RATE, track, samplesToEncode, targetSamples,
                        trackCursorSamples);
                }
            }

            deferForSourceBuffer = false;
            finalStopDrain = audioStopDrainRequested.load(std::memory_order_acquire) ||
                                        audioFinalizingCfrStop.load(std::memory_order_acquire);
                ce::audio::kDefaultAudioPullQuantumSamples * 4;
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                appCaptureRouteEnded =
                    src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        isCfrRecording, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    isCfrRecording, src.timelineValid, src.bootstrapComplete, optionalUnstarted, finalStopDrain);
                bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                sparseStartedSourceMaySilence =
                    ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
                        sparseStartedSourceCanSilence, bufferedTimelineSamples, samplesToEncode,
                        kSparseStartedPartialSilenceThresholdSamples) ||
                    inactiveStartedAppSourceMaySilence;
                if (!trackLargeBacklogDrain &&
                    ce::audio::ShouldDeferCfrAudioPullForSourceBuffer(isCfrRecording, forceDrain, optionalUnstarted,
                                                                      sparseStartedSourceMaySilence, samplesToEncode,
                                                                      bufferedTimelineSamples)) {
                    deferForSourceBuffer = true;
                    if (dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] CFR source wait: track=%d src=%zu buffered=%zu requested=%lld "
                            "target=%lldms encoded=%lld. Deferring audio pull to preserve real source samples.",
                            track, srcIdx, bufferedTimelineSamples, samplesToEncode, trackAudioTargetMs,
                            trackCursorSamples);
                    }
                    break;
                }
                // A started source that is still delivering real packets is LATE, not silent.
                // Exporting silence here is what permanently destroys its audio: the write
                // cursor is pinned forward and every later packet is trimmed as timeline
                // overlap while both cursors advance at wall rate. Hold the pull instead and
                // deepen the reservoir by the shortfall so the producer can overtake again.
                // The hold is bounded by the reservoir cap so a genuinely stuck source can
                // never freeze a co-mixed track.
                sourceIngestIdleMs = src.lastRealPacketIngestTick == 0
                                                       ? std::numeric_limits<int64_t>::max()
                                                       : static_cast<int64_t>(pullTick - src.lastRealPacketIngestTick);
                sourceRecentlyDeliveredRealPackets =
                    sourceIngestIdleMs <= ce::audio::kAudioIngestLiveSourceWindowMs;
                if (!trackLargeBacklogDrain && !inactiveStartedAppSourceMaySilence && !finalStopDrain &&
                    ce::audio::ShouldHoldCfrAudioPullForLateLiveSource(
                        isCfrRecording, forceDrain, src.timelineValid, src.bootstrapComplete, !appCaptureRouteEnded,
                        sourceRecentlyDeliveredRealPackets, samplesToEncode, bufferedTimelineSamples,
                        audioIngestReservoirExtraMs)) {
                    const int64_t shortfallSamples =
                        samplesToEncode - static_cast<int64_t>(bufferedTimelineSamples);
                    const int64_t shortfallMs = std::max<int64_t>(1, (shortfallSamples * 1000) / SAMPLE_RATE);
                    audioIngestReservoirExtraMs =
                        std::max(audioIngestReservoirExtraMs,
                                 ce::audio::RaiseAudioIngestReservoirForShortfall(reservoirPassBaseMs, shortfallMs));
                    audioIngestReservoir.extraMs = audioIngestReservoirExtraMs;
                    audioIngestReservoir.healthyElapsedMs = 0;
                    audioIngestReservoirPeakMs = std::max(audioIngestReservoirPeakMs, audioIngestReservoirExtraMs);
                    deferForSourceBuffer = true;
                    if (dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] Late live source hold: track=%d src=%zu buffered=%zu requested=%lld "
                            "shortfall=%lldms idle=%lldms target=%lldms encoded=%lld reservoirExtra=%lldms. Holding "
                            "the pull instead of exporting silence so late real audio is not destroyed as overlap.",
                            track, srcIdx, bufferedTimelineSamples, samplesToEncode, shortfallMs,
                            sourceIngestIdleMs == std::numeric_limits<int64_t>::max() ? -1 : sourceIngestIdleMs,
                            trackAudioTargetMs, trackCursorSamples, audioIngestReservoirExtraMs);
                    }
                    break;
                }
                if (sparseStartedSourceMaySilence &&
                    bufferedTimelineSamples < static_cast<size_t>(std::max<int64_t>(samplesToEncode, 0)) &&
                    dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Timeline source gap silence: track=%d src=%zu buffered=%zu requested=%lld "
                        "target=%lldms encoded=%lld lookahead=%lldms inactiveApp=%d idle=%lldms. Source contributes "
                        "available samples plus silence for missing range after the ingestion reservoir.",
                        track, srcIdx, bufferedTimelineSamples, samplesToEncode, trackAudioTargetMs, trackCursorSamples,
                        trackAudioPullLatencyMs, inactiveStartedAppSourceMaySilence ? 1 : 0,
                        sourceIngestIdleMs == std::numeric_limits<int64_t>::max() ? -1 : sourceIngestIdleMs);
                }
            }
            if (deferForSourceBuffer) {
                return false;
            }

            totalFloats = samplesToEncode * CHANNELS;
            mixBuffer =std::vector<float> (totalFloats, 0.0f);
            activeSources = 0;
            eligibleSources = 0;
    return true;
}
