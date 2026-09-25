#include "mediaengine_internal.h"
#include "audio_fault_accounting.h"


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


// Fades that shape freshly resampled source output before it enters the
// post-resample backlog: drop crossfade, packet-boundary fade-in and the
// underrun recovery fade-in. Records whether a fade-in shaped this pull.
void MediaEngine::ApplyResampledChunkFades(AudioSource& src, float* outFloats, int outSamples, int CHANNELS) {
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kRuntimeDropFadeSamples = AudioPullState::kRuntimeDropFadeSamples;
    if (!outFloats || outSamples <= 0 || CHANNELS <= 0) {
        return;
    }
    if (src.dropFadeSamplesRemaining > 0) {
        // The ramp length must equal the armed length: every trim arms
        // kRuntimeDropFadeSamples, and a longer divisor started the
        // crossfade at 60% of the new signal instead of at the anchor.
        const int kDropFadeSamples = static_cast<int>(kRuntimeDropFadeSamples);
        src.dropFadeSamplesRemaining = std::min(src.dropFadeSamplesRemaining, kDropFadeSamples);
        int blendSamples = std::min(src.dropFadeSamplesRemaining, outSamples);
        int blendStart = kDropFadeSamples - src.dropFadeSamplesRemaining;
        for (int s = 0; s < blendSamples; s++) {
            float alpha = (float)(blendStart + s + 1) / kDropFadeSamples;
            for (int ch = 0; ch < CHANNELS; ++ch) {
                const size_t idx = static_cast<size_t>(s) * CHANNELS + ch;
                const float anchor = GetDropFadeAnchor(src, ch);
                outFloats[idx] = anchor + (outFloats[idx] - anchor) * alpha;
            }
        }
        if (blendSamples > 0) {
            src.dropFadeStart.assign(static_cast<size_t>(CHANNELS), 0.0f);
            const size_t base = static_cast<size_t>(blendSamples - 1) * CHANNELS;
            for (int ch = 0; ch < CHANNELS; ++ch) {
                src.dropFadeStart[static_cast<size_t>(ch)] = outFloats[base + ch];
            }
            src.dropFadeStartL = src.dropFadeStart[0];
            src.dropFadeStartR = CHANNELS > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
        }
        src.dropFadeSamplesRemaining -= blendSamples;
    }
    if (src.packetBoundaryFadeInSamplesRemaining > 0) {
        const int blendSamples = std::min(src.packetBoundaryFadeInSamplesRemaining, outSamples);
        for (int s = 0; s < blendSamples; ++s) {
            const float alpha = ComputeRaisedCosineFade(
                static_cast<size_t>(s),
                static_cast<size_t>(std::max(src.packetBoundaryFadeInSamplesRemaining, 1)));
            const size_t base = static_cast<size_t>(s) * CHANNELS;
            for (int ch = 0; ch < CHANNELS; ++ch) {
                outFloats[base + ch] *= alpha;
            }
        }
        src.packetBoundaryFadeInSamplesRemaining -= blendSamples;
        src.fadeInAppliedThisPull = src.fadeInAppliedThisPull || blendSamples > 0;
    }
    if (src.pendingUnderrunRecoveryFade) {
        src.underrunFadeSamplesRemaining = SAMPLE_RATE / 40;  // 25ms - smoother transitions
        src.pendingUnderrunRecoveryFade = false;
    }
    if (src.underrunFadeSamplesRemaining > 0) {
        const int kUnderrunFadeSamples = SAMPLE_RATE / 40;  // 25ms - smoother transitions
        int blendSamples = std::min(src.underrunFadeSamplesRemaining, outSamples);
        int blendStart = kUnderrunFadeSamples - src.underrunFadeSamplesRemaining;
        for (int s = 0; s < blendSamples; s++) {
            float alpha = (float)(blendStart + s + 1) / kUnderrunFadeSamples;
            const size_t base = static_cast<size_t>(s) * CHANNELS;
            for (int ch = 0; ch < CHANNELS; ++ch) {
                outFloats[base + ch] *= alpha;
            }
        }
        src.underrunFadeSamplesRemaining -= blendSamples;
        src.fadeInAppliedThisPull = src.fadeInAppliedThisPull || blendSamples > 0;
    }
}


float MediaEngine::GetDropFadeAnchor(const AudioSource& src,  int channel) {


        if (channel >= 0 && channel < static_cast<int>(src.dropFadeStart.size())) {
            return src.dropFadeStart[static_cast<size_t>(channel)];
        }
        return 0.0f;

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

bool MediaEngine::AudioSourcesLostTheirDevice() const {
    bool lost = false;
    for (const AudioSource& src : audioSources) {
        if (!src.capture) {
            continue;
        }
        const uint32_t episodes = src.capture->GetDeviceUnavailableEpisodes();
        if (episodes == 0) {
            continue;
        }
        lost = true;
        DLL_Log(
            "[AudioFinalization] track=%d source=%s ran without its audio device %u time(s) (absent, refused or lost "
            "and not re-acquired at once); those spans hold silence, so the recording is reported as degraded",
            src.track, src.sourceType == AudioConfig::Microphone ? "microphone" : "system", episodes);
    }
    return lost;
}

void MediaEngine::LatchAudioOverrunLossForStop() {
    uint64_t lost = 0;
    for (const AudioSource& src : audioSources) {
        lost += src.timelineStarvationDropSamples;
    }
    audioOverrunLostSamplesThisRecording = lost;
}

bool MediaEngine::AudioContentWasLost() const {
    ce::audio::RecordingAudioLossEvidence evidence;
    evidence.overrunLostSamples = audioOverrunLostSamplesThisRecording;
    evidence.audioWorkerFailed = audioWorkerFailedThisRecording.load(std::memory_order_acquire);
    if (!ce::audio::IsRecordingAudioContentLost(evidence)) {
        return false;
    }
    DLL_Log(
        "[AudioFinalization] audio content was lost: overrunLostSamples=%llu (%.1f ms at 48 kHz) workerFailed=%d; "
        "track lengths stay exact but that content never reached the file, so the recording is reported as degraded",
        static_cast<unsigned long long>(evidence.overrunLostSamples),
        static_cast<double>(evidence.overrunLostSamples) / 48.0, evidence.audioWorkerFailed ? 1 : 0);
    return true;
}

bool MediaEngine::AudioTracksHaveContentHoles() const {
    bool holes = false;
    // Each track's encoder is owned by exactly one source (the track's first).
    for (const AudioSource& src : audioSources) {
        const AudioEncoder* encoder = src.encoder.get();
        if (!encoder) {
            continue;
        }
        const int64_t holeSamples = encoder->GetFinalizationReport().contentHoleSamples;
        if (holeSamples <= 0) {
            continue;
        }
        holes = true;
        DLL_Log(
            "[AudioFinalization] encoder=%s holds %lld sample(s) of content holes (audio it consumed but could not "
            "encode, placed as silence at their exact positions); the recording is reported as degraded",
            encoder->GetRuntimeContract().encoderName.c_str(), static_cast<long long>(holeSamples));
    }
    return holes;
}
