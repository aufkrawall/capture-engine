#include "mediaengine_internal.h"


bool MediaEngine::CreateSharedCaptureTextures(uint32_t width,  uint32_t height,  uint32_t format,  SharedMemoryLayout* sharedMem) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - no encoder");
            return false;
        }
        if (!sharedMem) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - sharedMem is null");
            return false;
        }

        // IMPORTANT: Set encoder dimensions and LUID from the parameters before
        // EnsureDevice Otherwise EnsureDevice fails because width/height are still
        // 0 or uses wrong GPU
        videoEnc->SetDimensions(width, height);
        videoEnc->SetAdapterLUID(sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());

        if (!videoEnc->EnsureDevice()) {
            DLL_Log(
                "MediaEngine: CreateSharedCaptureTextures - device init failed "
                "for LUID %08x:%08x",
                sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());
            return false;
        }
        return videoEnc->CreateSharedCaptureTextures(width, height, format, sharedMem);

}

void MediaEngine::WritePacket(AVPacket* pkt) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (audioOnly && audioOnlyFmtCtx) {
            if (pkt->stream_index >= 0 && (unsigned int)pkt->stream_index < audioOnlyFmtCtx->nb_streams) {
                AVStream* st = audioOnlyFmtCtx->streams[pkt->stream_index];
                AVRational codec_tb = {1, st->codecpar->sample_rate};
                if (codec_tb.den > 0)
                    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
            }
            av_interleaved_write_frame(audioOnlyFmtCtx, pkt);
        } else if (videoEnc) {
            videoEnc->WriteFrame(pkt);
        }

}

void MediaEngine::ReloadConfig(const AppConfig* newConfig) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        DLL_Log("MediaEngine::ReloadConfig called");

        // Update config
        this->config = *newConfig;
        trackAudioFormats = ResolveTrackAudioFormats(*newConfig);
        DLL_Log("[AVSyncAuto] engine_reload: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(this->config.avSyncResolvedRenderLatencyMs), this->config.avSyncConfidence.c_str(),
                this->config.avSyncReason.c_str(), this->config.avSyncUsedAudioProbe ? 1 : 0);

        // If recording, we can't fully re-init, but we can log a warning.
        if (recording) {
            DLL_Log(
                "MediaEngine: Config updated, but recording is active. Changes "
                "will apply on next recording.");
            return;
        }

        DLL_Log("MediaEngine: Re-initializing encoders with new config...");

        // Clear audio sources (and their encoders)
        audioSources.clear();
        DLL_Log("MediaEngine: Cleared existing audio sources");

        // Re-create VideoEncoder to apply all new settings
        videoEnc.reset();
        videoEnc = std::make_unique<VideoEncoder>();

        bool vRes =
            videoEnc->Init(config.video, 0, 0, config.video.fps, [this](AVPacket* pkt) { this->WritePacket(pkt); });


        if (!vRes) {
            DLL_Log("MediaEngine: Failed to re-init VideoEncoder!");
            return;
        }
        DLL_Log("MediaEngine: VideoEncoder re-initialized successfully.");

        // Re-create audio sources with new config (including new codec)
        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;
        // Guards against summing two identical app-audio captures into one track.
        std::set<std::string> seenAppAudioTrackKeys;

        for (size_t i = 0; i < config.audioSources.size(); i++) {
            const AudioConfig& audioConfig = config.audioSources[i];
            if (!audioConfig.enabled) {
                DLL_Log("MediaEngine::ReloadConfig audio source %zu disabled", i);
                continue;
            }

            DLL_Log("MediaEngine::ReloadConfig setting up audio source %zu (codec=%s)", i, audioConfig.codec.c_str());

            // Get the list of tracks this source should output to
            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                targetTracks.push_back((int)(i + 1));
            }

            DLL_Log("MediaEngine::ReloadConfig Audio source %zu targets %zu tracks", i, targetTracks.size());

            // For each target track, create or reuse an encoder
            for (int track : targetTracks) {
                // Defense in depth: never create a second app-audio capture for the
                // same process on the same track. Summing identical captures combs.
                if (audioConfig.sourceType == AudioConfig::AppAudio) {
                    const std::string appKey = AppAudioTrackKey(audioConfig, track);
                    if (!seenAppAudioTrackKeys.insert(appKey).second) {
                        DLL_Log(
                            "MediaEngine::ReloadConfig WARNING: duplicate app-audio source (process='%s' "
                            "processId=%lu) already targets track %d - skipping duplicate capture to avoid "
                            "comb-filter artifacts",
                            audioConfig.processName.empty() ? "<pid>" : audioConfig.processName.c_str(),
                            (unsigned long)audioConfig.processId, track);
                        continue;
                    }
                }
                TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
                AudioConfig resolvedAudioConfig = audioConfig;
                resolvedAudioConfig.outputChannels = audioConfig.downmix ? 2 : trackFormat.channels;
                resolvedAudioConfig.outputChannelMask =
                    audioConfig.downmix ? DefaultChannelMaskForChannels(2) : trackFormat.channelMask;
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu reusing encoder "
                        "for track %d",
                        i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes =
                        newEncoder->Init(resolvedAudioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::ReloadConfig Audio encoder for track %d failed", track);
                        continue;
                    }

                    videoEnc->AddAudioContext(resolvedAudioConfig, newEncoder->GetCodecContext(), track);

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = std::move(newEncoder);
                    source.sharedEncoderPtr = source.encoder.get();

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Created new encoder for track %d "
                        "(source %zu, type=%d)",
                        track, i, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }

                if (it != trackToEncoder.end()) {
                    AudioSource source;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = nullptr;
                    source.sharedEncoderPtr = encoderForTrack;

                    // Create appropriate capture type
                    if (audioConfig.sourceType == AudioConfig::AppAudio) {
                        source.appCapture = std::make_unique<ProcessLoopbackCapture>();
                        source.appCapture->SetRequestedFormat(48000, source.mixChannels, source.mixChannelMask);
                    } else {
                        source.capture = std::make_unique<AudioCapture>();
                    }

                    // INIT RING BUFFER AND SYNC RESAMPLER (Per-source drift compensation)
                    InitAudioSourceBuffers(source, audioConfig, i);

                    DLL_Log(
                        "MediaEngine::ReloadConfig Audio source %zu shares encoder "
                        "for track %d (type=%d)",
                        i, track, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }
            }
        }

        CoalesceCaptureRoutes();

        DLL_Log(
            "MediaEngine: ReloadConfig complete. Audio sources: %zu, unique "
            "tracks: %zu",
            audioSources.size(), trackToEncoder.size());

}

void MediaEngine::CoalesceCaptureRoutes() {


        using CaptureRouteKey = std::tuple<int, std::string>;
        std::map<CaptureRouteKey, size_t> owners;
        std::map<CaptureRouteKey, std::pair<int, uint32_t>> appCaptureFormats;
        size_t physicalCaptureCount = 0;
        size_t sharedRouteCount = 0;

        auto captureKey = [](const AudioSource& src) -> CaptureRouteKey {
            std::string physicalIdentity;
            if (src.sourceType == AudioConfig::AppAudio) {
                physicalIdentity = ce::audio::AppAudioTrackIdentity(
                    src.config.processName, static_cast<unsigned long>(src.config.processId), 0);
            } else {
                physicalIdentity = src.config.device.empty() ? "<default>" : src.config.device;
                std::transform(physicalIdentity.begin(), physicalIdentity.end(), physicalIdentity.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            }
            return {static_cast<int>(src.sourceType), std::move(physicalIdentity)};
        };

        for (const auto& src : audioSources) {
            if (src.sourceType != AudioConfig::AppAudio) {
                continue;
            }
            const CaptureRouteKey key = captureKey(src);
            auto& format = appCaptureFormats[key];
            if (src.mixChannels > format.first) {
                format = {src.mixChannels, src.mixChannelMask};
            }
        }

        for (size_t idx = 0; idx < audioSources.size(); ++idx) {
            auto& src = audioSources[idx];
            const CaptureRouteKey key = captureKey(src);
            const auto [it, inserted] = owners.emplace(key, idx);
            if (inserted) {
                src.captureFanoutOwnerIndex = idx;
                if (src.appCapture) {
                    const auto format = appCaptureFormats[key];
                    src.appCapture->SetRequestedFormat(48000, format.first, format.second);
                    DLL_Log("[AudioRoute] App capture owner=%zu configuredSource=%zu requests shared format=%dch/0x%x",
                            idx, src.configuredSourceIndex, format.first, format.second);
                }
                ++physicalCaptureCount;
                continue;
            }

            const size_t ownerIdx = it->second;
            src.captureFanoutOwnerIndex = ownerIdx;
            src.capture.reset();
            src.appCapture.reset();
            ++sharedRouteCount;
            DLL_Log(
                "[AudioRoute] Source route src=%zu track=%d shares physical capture owner=%zu track=%d "
                "configuredSource=%zu type=%d format=%dch/0x%x",
                idx, src.track, ownerIdx, audioSources[ownerIdx].track, src.configuredSourceIndex,
                static_cast<int>(src.sourceType), src.mixChannels, src.mixChannelMask);
        }

        DLL_Log("[AudioRoute] Capture topology: physical=%zu routedFollowers=%zu totalRoutes=%zu", physicalCaptureCount,
                sharedRouteCount, audioSources.size());

}

ProcessLoopbackCapture* MediaEngine::GetAppCaptureForRoute(size_t srcIdx) {


        if (srcIdx >= audioSources.size()) {
            return nullptr;
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        if (ownerIdx >= audioSources.size()) {
            return nullptr;
        }
        return audioSources[ownerIdx].appCapture.get();

}

std::pair<int64_t, int64_t> MediaEngine::GetCaptureGroupBufferedSampleRange(size_t srcIdx) const {


        if (srcIdx >= audioSources.size()) {
            return {0, 0};
        }
        const size_t ownerIdx = audioSources[srcIdx].captureFanoutOwnerIndex;
        int64_t minimum = std::numeric_limits<int64_t>::max();
        int64_t maximum = 0;
        for (const auto& route : audioSources) {
            if (route.captureFanoutOwnerIndex != ownerIdx || !route.ringBuffer) {
                continue;
            }
            const size_t channels = static_cast<size_t>(std::max(1, route.mixChannels));
            const int64_t bufferedSamples = static_cast<int64_t>(route.ringBuffer->GetAvailable() / channels);
            minimum = std::min(minimum, bufferedSamples);
            maximum = std::max(maximum, bufferedSamples);
        }
        return {minimum == std::numeric_limits<int64_t>::max() ? 0 : minimum, maximum};

}

void MediaEngine::InitAudioSourceBuffers(AudioSource& source,  const AudioConfig& audioConfig,  size_t sourceIdx) {


        constexpr int kMixerSampleRate = 48000;
        constexpr size_t kDefaultAudioRingBufferSeconds = 8;
        // Heavy CFR overload runs can fall tens of seconds behind real time even
        // while audio/video file durations still stay mathematically equal. Give CFR
        // enough retention headroom to avoid destructive oldest-audio trims in those
        // runs so we preserve pitch and avoid crackle while diagnostics report the
        // underlying encoder shortfall honestly.
        constexpr size_t kCfrAudioRingBufferSeconds = 30;
        const bool isCfrPath = ce::audio::ShouldUseCfrAudioContinuityPolicy(config.video.useVFR);
        const size_t ringBufferSeconds = isCfrPath ? kCfrAudioRingBufferSeconds : kDefaultAudioRingBufferSeconds;
        const int channels = std::clamp(source.mixChannels, 1, 8);
        const size_t capacity = static_cast<size_t>(kMixerSampleRate) * ringBufferSeconds * channels;
        source.fullRingBufferCapacityFloats = capacity;

        // App-audio sources commonly target candidate processes that may not be
        // running (a "capture whichever game is running" profile). Allocating the
        // full multi-second CFR retention buffer (~11.5 MB at 30s/48k/stereo) for
        // every such source wastes memory on sources that never capture. Start app
        // sources with a small buffer and grow in-place to full capacity on first
        // captured audio (see AudioLoop). System/mic sources are always active, so
        // they keep full capacity immediately.
        constexpr size_t kAppAudioInitialRingBufferSeconds = 1;
        const size_t initialCapacity = (source.appCapture != nullptr)
                                           ? std::min(capacity, static_cast<size_t>(kMixerSampleRate) *
                                                                    kAppAudioInitialRingBufferSeconds * channels)
                                           : capacity;
        source.ringBuffer = std::make_unique<AudioRingBuffer>(initialCapacity);
        DLL_Log(
            "MediaEngine::Init RingBuffer created for source %zu. Cap=%zu floats (initial, full=%zu floats/%zus), "
            "rate=%d channels=%d mask=0x%x deferred=%d",
            sourceIdx, initialCapacity, capacity, ringBufferSeconds, kMixerSampleRate, channels, source.mixChannelMask,
            initialCapacity < capacity ? 1 : 0);

        source.syncResampler = std::make_unique<AudioResampler>();
        AudioResampler::InputFormat syncInFmt;
        syncInFmt.sampleRate = kMixerSampleRate;
        syncInFmt.channels = channels;
        syncInFmt.bitsPerSample = 32;
        syncInFmt.validBitsPerSample = 32;
        syncInFmt.isFloat = true;
        syncInFmt.blockAlign = channels * 4;
        syncInFmt.channelMask = source.mixChannelMask;
        AudioResampler::OutputFormat syncOutFmt;
        syncOutFmt.sampleRate = kMixerSampleRate;
        syncOutFmt.channels = channels;
        syncOutFmt.sampleFmt = AV_SAMPLE_FMT_FLT;
        syncOutFmt.channelMask = source.mixChannelMask;
        source.syncResampler->Init(syncInFmt, syncOutFmt);
        DLL_Log("MediaEngine::Init SyncResampler created for source %zu (rate=%d channels=%d)", sourceIdx,
                kMixerSampleRate, channels);

}
