#include "mediaengine_internal.h"


MediaEngine::~MediaEngine() {


        try {
        StopRecording();
        } catch (...) {
            DLL_Log("[MediaEngine] Suppressed exception during destruction");
        }

}

float MediaEngine::ComputeRaisedCosineFade(size_t index,  size_t totalSamples) {


        if (totalSamples == 0) {
            return 1.0f;
        }

        const float t = static_cast<float>(index + 1) / static_cast<float>(totalSamples);
        return 0.5f * (1.0f - std::cos(3.14159265358979323846f * t));

}

void MediaEngine::ApplyPacketBoundaryFadeIn(float* interleavedSamples,  size_t sampleCount,  size_t channels, 
                                          size_t fadeSamples) {


        if (!interleavedSamples || channels == 0 || sampleCount == 0 || fadeSamples == 0) {
            return;
        }

        const size_t blendSamples = std::min(sampleCount, fadeSamples);
        for (size_t sampleIdx = 0; sampleIdx < blendSamples; ++sampleIdx) {
            const float fade = ComputeRaisedCosineFade(sampleIdx, fadeSamples);
            const size_t base = sampleIdx * channels;
            for (size_t ch = 0; ch < channels; ++ch) {
                interleavedSamples[base + ch] *= fade;
            }
        }

}

uint32_t MediaEngine::DefaultChannelMaskForChannels(int channels) {


        switch (channels) {
            case 1:
                return SPEAKER_FRONT_CENTER;
            case 2:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            case 3:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
            case 4:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 5:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_LEFT |
                       SPEAKER_BACK_RIGHT;
            case 6:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
            case 7:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_BACK_CENTER;
            case 8:
                return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                       SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
            default:
                return 0;
        }

}

int MediaEngine::ParseAudioSampleRate(const AudioConfig& audioConfig) {


        return ce::audio::ParseSampleRateOr(audioConfig.sampleRate, 48000);

}

int MediaEngine::AudioSourceLayoutPriority(AudioConfig::SourceType sourceType) {


        return sourceType == AudioConfig::Microphone ? 2 : 1;

}

MediaEngine::TrackAudioFormat MediaEngine::ProbeSourceTrackFormat(const AudioConfig& audioConfig) {


        TrackAudioFormat format;
        format.sampleRate = ParseAudioSampleRate(audioConfig);
        if (audioConfig.downmix) {
            format.channels = 2;
            format.channelMask = DefaultChannelMaskForChannels(2);
            format.priority = 0;
            return format;
        }

        AudioPacket probed{};
        bool probedOk = false;
        if (audioConfig.sourceType == AudioConfig::AppAudio) {
            // Process loopback accepts a requested format; preserve the default
            // render endpoint layout for app-only tracks instead of hard-coding stereo.
            probedOk = AudioCapture::ProbeMixFormat("", true, &probed);
        } else {
            probedOk = AudioCapture::ProbeMixFormat(audioConfig.device,
                                                    audioConfig.sourceType == AudioConfig::SystemAudio, &probed);
        }
        if (probedOk && probed.channels > 0) {
            format.channels = std::clamp(probed.channels, 1, 8);
            format.channelMask =
                probed.channelMask != 0 ? probed.channelMask : DefaultChannelMaskForChannels(format.channels);
            format.sampleRate = 48000;
        }
        format.priority = AudioSourceLayoutPriority(audioConfig.sourceType);
        return format;

}

std::map<int, MediaEngine::TrackAudioFormat> MediaEngine::ResolveTrackAudioFormats(const AppConfig& appConfig) {


        std::map<int, TrackAudioFormat> resolved;
        for (size_t i = 0; i < appConfig.audioSources.size(); ++i) {
            const AudioConfig& audioConfig = appConfig.audioSources[i];
            if (!audioConfig.enabled) {
                continue;
            }

            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                targetTracks.push_back(static_cast<int>(i + 1));
            }

            TrackAudioFormat candidate = ProbeSourceTrackFormat(audioConfig);
            for (int track : targetTracks) {
                auto it = resolved.find(track);
                if (it == resolved.end() || candidate.priority < it->second.priority) {
                    resolved[track] = candidate;
                    DLL_Log("[AudioFormat] Track %d resolved format: %dch mask=0x%x rate=%d priority=%d", track,
                            candidate.channels, candidate.channelMask, candidate.sampleRate, candidate.priority);
                }
            }
        }
        return resolved;

}

MediaEngine::TrackAudioFormat MediaEngine::GetTrackAudioFormat(int track) const {


        auto it = trackAudioFormats.find(track);
        if (it != trackAudioFormats.end()) {
            return it->second;
        }
        return {};

}

void MediaEngine::CaptureDropFadeAnchor(AudioSource& src,  int channels) {


        channels = std::clamp(channels, 1, 8);
        src.dropFadeStart.assign(static_cast<size_t>(channels), 0.0f);
        if (src.postResampleBuffer.size() < static_cast<size_t>(channels)) {
            src.dropFadeStartL = 0.0f;
            src.dropFadeStartR = 0.0f;
            return;
        }
        const size_t base = src.postResampleBuffer.size() - static_cast<size_t>(channels);
        for (int ch = 0; ch < channels; ++ch) {
            src.dropFadeStart[static_cast<size_t>(ch)] = src.postResampleBuffer[base + static_cast<size_t>(ch)];
        }
        src.dropFadeStartL = src.dropFadeStart[0];
        src.dropFadeStartR = channels > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];

}

float MediaEngine::GetDropFadeAnchor(const AudioSource& src,  int channel) {


        if (channel >= 0 && channel < static_cast<int>(src.dropFadeStart.size())) {
            return src.dropFadeStart[static_cast<size_t>(channel)];
        }
        return 0.0f;

}

int64_t MediaEngine::GetVideoElapsedMs() const {


        return videoElapsedMs.load();

}

bool MediaEngine::SessionUsesVfr() const {


        return timingModeFrozenForSession ? sessionUseVfr : config.video.useVFR;

}

bool MediaEngine::SessionUsesScreenGrab() const {


        return activeScreenGrab;

}

int64_t MediaEngine::GetCommittedVideoElapsedUs(int64_t fallbackElapsedUs) const {


        if (SessionUsesVfr() || !videoEnc) {
            return fallbackElapsedUs;
        }

        const int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
        return encodedDurationUs > 0 ? encodedDurationUs : fallbackElapsedUs;

}

void MediaEngine::CommitVideoElapsedUs(SourceTimelineState& timelineState,  int64_t elapsedUs) {


        if (elapsedUs < 0) {
            return;
        }

        timelineState.lastElapsedUs = std::max(timelineState.lastElapsedUs, elapsedUs);
        this->videoElapsedMs.store(elapsedUs / 1000);
        lastVideoFrameMs = elapsedUs / 1000;

}

size_t MediaEngine::GetBufferedTimelineSamples(const AudioSource& src) const {


        const size_t kChannels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));

        size_t bufferedSamples = src.postResampleBuffer.size() / kChannels;
        if (src.ringBuffer) {
            bufferedSamples += src.ringBuffer->GetAvailable() / kChannels;
        }
        return bufferedSamples;

}

void MediaEngine::PublishAudioIngestHeadroom(int64_t headroomSamples,  int sampleRate) {


        if (sampleRate <= 0) {
            return;
        }
        const int64_t normalizedSamples =
            sampleRate == 48000 ? headroomSamples : (headroomSamples * 48000) / sampleRate;
        int64_t observedWorst = audioIngestWorstHeadroomSamples.load(std::memory_order_relaxed);
        while (normalizedSamples < observedWorst && !audioIngestWorstHeadroomSamples.compare_exchange_weak(
                                                        observedWorst, normalizedSamples, std::memory_order_relaxed)) {
        }

}

void MediaEngine::ServiceSourceIngestStarvation(AudioSource& src,  size_t srcIdx,  int64_t packetStartSamples, 
                                       int64_t overlapSamples,  size_t retainedWriteSamples,  int resampledSamples, 
                                       int sampleRate,  uint64_t nowTick) {


        const bool packetFullyDestroyed = resampledSamples > 0 && retainedWriteSamples == 0 && overlapSamples > 0;
        if (!packetFullyDestroyed) {
            src.timelineStarvationBeganTick = 0;
            return;
        }

        src.timelineStarvationDropSamples += static_cast<uint64_t>(resampledSamples);
        if (src.timelineStarvationBeganTick == 0) {
            src.timelineStarvationBeganTick = nowTick;
        }

        const int64_t boundedRate = std::max<int64_t>(1, sampleRate);
        const int64_t starvedElapsedMs = static_cast<int64_t>(nowTick - src.timelineStarvationBeganTick);
        const int64_t deficitSamples =
            std::max<int64_t>(0, static_cast<int64_t>(src.qpcAlignedWrittenSamples) - packetStartSamples);
        const bool reservoirAtCap = audioIngestReservoirExtraMs >= ce::audio::kAudioIngestMaxExtraReservoirMs;

        if (nowTick - src.lastTimelineStarvationWarnTick >= 1000) {
            DLL_Log(
                "[AudioLoop] WARNING: source ingest starvation src=%zu track=%d process=%s destroyed=%llu samples "
                "(%.1fms total) deficit=%lld samples (%lldms) starvedFor=%lldms reservoirExtra=%lldms atCap=%d. "
                "The exported cursor ran past the live capture edge; every packet for this range is real audio "
                "being discarded as timeline overlap.",
                srcIdx, src.track, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                (unsigned long long)src.timelineStarvationDropSamples,
                static_cast<double>(src.timelineStarvationDropSamples) * 1000.0 / static_cast<double>(boundedRate),
                (long long)deficitSamples, (long long)(deficitSamples * 1000 / boundedRate),
                (long long)starvedElapsedMs, (long long)audioIngestReservoirExtraMs, reservoirAtCap ? 1 : 0);
            src.lastTimelineStarvationWarnTick = nowTick;
        }

        if (!ce::audio::ShouldResyncStarvedLiveAudioSource(IsCfrRecording(), false, reservoirAtCap, true,
                                                           starvedElapsedMs, deficitSamples,
                                                           /*minStarvedMs=*/1500,
                                                           /*minDeficitSamples=*/boundedRate / 100)) {
            return;
        }

        src.timelineResyncOffsetSamples += deficitSamples;
        src.timelineResyncSuppressedSamples += static_cast<uint64_t>(deficitSamples);
        src.timelineResyncEvents++;
        src.timelineStarvationBeganTick = 0;
        src.packetBoundaryFadeInSamplesRemaining = static_cast<int>(std::max<int64_t>(1, boundedRate / 750));
        DLL_Log(
            "[AudioLoop] WARNING: unrecoverable ingest starvation - re-anchoring src=%zu track=%d process=%s by "
            "%lld samples (%lldms) after %lldms at the reservoir cap. This source skips that much content once "
            "so live audio resumes; track lengths, PTS, and every other source are unchanged.",
            srcIdx, src.track, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
            (long long)deficitSamples, (long long)(deficitSamples * 1000 / boundedRate), (long long)starvedElapsedMs);

}

size_t MediaEngine::DropOldestBufferedSamples(AudioSource& src,  size_t samplesToDrop) {


        const size_t kChannels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));

        if (samplesToDrop == 0) {
            return 0;
        }

        size_t droppedSamples = 0;

        size_t postSamples = src.postResampleBuffer.size() / kChannels;
        size_t dropFromPost = std::min(postSamples, samplesToDrop);
        if (dropFromPost > 0) {
            size_t dropFloats = dropFromPost * kChannels;
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, dropFromPost);
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, dropFromPost);
            src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                         src.postResampleBuffer.begin() + static_cast<std::ptrdiff_t>(dropFloats));
            droppedSamples += dropFromPost;
            samplesToDrop -= dropFromPost;
        }

        if (samplesToDrop > 0 && src.ringBuffer) {
            size_t droppedFloats = src.ringBuffer->Skip(samplesToDrop * kChannels);
            size_t droppedFromRing = droppedFloats / kChannels;
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, droppedFromRing);
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, droppedFromRing);
            droppedSamples += droppedFromRing;
        }

        if (droppedSamples > 0) {
            src.latencyTrimSamples += droppedSamples;
            src.bootstrapTrimSamples += droppedSamples;
        }

        return droppedSamples;

}

void MediaEngine::DiscardPendingAudioPackets() {


        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->DiscardPendingPackets();
            }
            if (src.appCapture) {
                src.appCapture->DiscardPendingPackets();
            }
        }

}

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
