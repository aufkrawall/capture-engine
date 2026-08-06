#include "mediaengine_internal.h"


void MediaEngine::SyncAudioToFirstVideoFrame(int64_t startQpcMs,  int64_t startQpc100ns,  bool preservePendingPackets) {


        audioSyncPending.store(true, std::memory_order_release);
        audioResetPreservePackets.store(preservePendingPackets, std::memory_order_release);
        recordingStartSystemQPCMs.store(startQpcMs, std::memory_order_release);
        recordingStartSystemQpc100ns.store(startQpc100ns, std::memory_order_release);
        const uint64_t generation = audioResetRequestedGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        DLL_Log(
            "[A/V START] Shared startup reset issued: generation=%llu startMs=%lld startQpc100ns=%lld "
            "sources=%zu preservePending=%d worker=%d",
            static_cast<unsigned long long>(generation), startQpcMs, startQpc100ns, audioSources.size(),
            preservePendingPackets ? 1 : 0, audioRunning.load(std::memory_order_acquire) ? 1 : 0);
        audioDrainCv.notify_all();

        bool resetAppliedByWorker = false;
        if (audioRunning.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(audioDrainMutex);
            audioDrainCv.wait(lock, [this, generation]() {
                return audioResetAcknowledgedGeneration.load(std::memory_order_acquire) >= generation ||
                       !audioRunning.load(std::memory_order_acquire);
            });
            resetAppliedByWorker = audioResetAcknowledgedGeneration.load(std::memory_order_acquire) >= generation;
        }
        if (!resetAppliedByWorker) {
            ApplyAudioTimelineReset(generation, startQpcMs, preservePendingPackets);
            audioResetAcknowledgedGeneration.store(generation, std::memory_order_release);
        }

        ResetAudioPullStateForRecording();
        audioResetCommittedGeneration.store(generation, std::memory_order_release);
        audioSyncPending.store(false, std::memory_order_release);
        preservePendingStartupAudioPackets.store(false, std::memory_order_release);
        audioDrainCv.notify_all();
        DLL_Log("[A/V START] Shared startup reset committed: generation=%llu routeAcks=%zu encoderAndPullState=ready",
                static_cast<unsigned long long>(generation), audioSources.size());

}


bool MediaEngine::Init(const AppConfig* config) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        DLL_Log("MediaEngine::Init starting");

        // Initialize QPC Frequency
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
        DLL_Log("MediaEngine: Trusted QPC Frequency: %lld", qpcFreq);

        this->config = *config;
        trackAudioFormats = ResolveTrackAudioFormats(*config);
        DLL_Log("[AVSyncAuto] engine_config: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(this->config.avSyncResolvedRenderLatencyMs), this->config.avSyncConfidence.c_str(),
                this->config.avSyncReason.c_str(), this->config.avSyncUsedAudioProbe ? 1 : 0);

        // Setup Video (Alloc Only) - skip for audio-only
        if (audioOnly) {
            DLL_Log("MediaEngine::Init audio-only mode - skipping VideoEncoder");
            videoEnc = nullptr;
            InitAudioOnlyMuxer(config);
        } else {
            DLL_Log("MediaEngine::Init creating VideoEncoder");
            videoEnc = std::make_unique<VideoEncoder>();
            DLL_Log("MediaEngine::Init calling VideoEncoder::Init");
            bool vRes = videoEnc->Init(config->video, 0, 0, config->video.fps,
                                       [this](AVPacket* pkt) { this->WritePacket(pkt); });
            if (!vRes) {
                DLL_Log("MediaEngine::Init VideoEncoder init failed");
                return false;
            }
            DLL_Log("MediaEngine::Init VideoEncoder initialized OK");
        }

        // Setup Audio Sources (supports multiple: system audio, microphone, etc.)
        DLL_Log("MediaEngine::Init audio sources count=%d", (int)config->audioSources.size());

        // Maps track number to encoder for that track
        std::map<int, AudioEncoder*> trackToEncoder;
        // Guards against summing two identical app-audio captures into one track.
        std::set<std::string> seenAppAudioTrackKeys;

        for (size_t i = 0; i < config->audioSources.size(); i++) {
            const AudioConfig& audioConfig = config->audioSources[i];
            if (!audioConfig.enabled) {
                DLL_Log("MediaEngine::Init audio source %zu disabled", i);
                continue;
            }

            DLL_Log(
                "MediaEngine::Init setting up audio source %zu (type=%d device=%s "
                "process=%s)",
                i, (int)audioConfig.sourceType, audioConfig.device.empty() ? "default" : audioConfig.device.c_str(),
                audioConfig.processName.empty() ? "N/A" : audioConfig.processName.c_str());

            // Get the list of tracks this source should output to
            std::vector<int> targetTracks = audioConfig.tracks;
            if (targetTracks.empty()) {
                // Default: use track (i+1)
                targetTracks.push_back((int)(i + 1));
            }

            DLL_Log("MediaEngine::Init Audio source %zu targets %zu tracks", i, targetTracks.size());

            // For each target track, create or reuse an encoder
            for (int track : targetTracks) {
                // Defense in depth: never create a second app-audio capture for the
                // same process on the same track. Summing identical captures combs.
                if (audioConfig.sourceType == AudioConfig::AppAudio) {
                    const std::string appKey = AppAudioTrackKey(audioConfig, track);
                    if (!seenAppAudioTrackKeys.insert(appKey).second) {
                        DLL_Log(
                            "MediaEngine::Init WARNING: duplicate app-audio source (process='%s' processId=%lu) "
                            "already targets track %d - skipping duplicate capture to avoid comb-filter artifacts",
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
                // Check if we already have an encoder for this track
                AudioEncoder* encoderForTrack = nullptr;
                auto it = trackToEncoder.find(track);
                if (it != trackToEncoder.end()) {
                    encoderForTrack = it->second;
                    DLL_Log("MediaEngine::Init Audio source %zu reusing encoder for track %d", i, track);
                } else {
                    // Create new encoder for this track
                    auto newEncoder = std::make_unique<AudioEncoder>();
                    bool aRes =
                        newEncoder->Init(resolvedAudioConfig, [this](AVPacket* pkt) { this->WritePacket(pkt); });

                    if (!aRes) {
                        DLL_Log("MediaEngine::Init Audio encoder for track %d failed", track);
                        continue;
                    }

                    // Register with VideoEncoder for stream creation (skip in audio-only)
                    if (!audioOnly) {
                        videoEnc->AddAudioContext(resolvedAudioConfig, newEncoder->GetCodecContext(), track);
                    }

                    encoderForTrack = newEncoder.get();
                    trackToEncoder[track] = encoderForTrack;

                    // Create AudioSource to own this encoder
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

                    // Set video time getter for clock drift compensation
                    source.encoder->SetVideoTimeGetter([this]() { return GetVideoElapsedMs(); });

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
                        "MediaEngine::Init Created new encoder for track %d (source "
                        "%zu, type=%d)",
                        track, i, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }

                // If this source doesn't own the encoder, create a source entry that
                // shares it
                if (it != trackToEncoder.end()) {
                    AudioSource source;
                    source.config = audioConfig;
                    source.config = resolvedAudioConfig;
                    source.track = track;
                    source.configuredSourceIndex = i;
                    source.sourceType = audioConfig.sourceType;
                    source.mixChannels =
                        resolvedAudioConfig.outputChannels > 0 ? resolvedAudioConfig.outputChannels : 2;
                    source.mixChannelMask = resolvedAudioConfig.outputChannelMask != 0
                                                ? resolvedAudioConfig.outputChannelMask
                                                : DefaultChannelMaskForChannels(source.mixChannels);
                    source.encoder = nullptr;  // Shared, not owned
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
                        "MediaEngine::Init Audio source %zu shares encoder for track "
                        "%d (type=%d)",
                        i, track, (int)audioConfig.sourceType);
                    audioSources.push_back(std::move(source));
                }
            }
        }

        CoalesceCaptureRoutes();

        DLL_Log("MediaEngine::Init complete. Audio sources: %zu, unique tracks: %zu", audioSources.size(),
                trackToEncoder.size());

        // Audio-only: create muxer streams for each audio track
        if (audioOnly && audioOnlyFmtCtx) {
            for (auto& kv : trackToEncoder) {
                int track = kv.first;
                AudioEncoder* enc = kv.second;
                AVStream* stream = avformat_new_stream(audioOnlyFmtCtx, enc->GetCodecContext()->codec);

                if (stream) {
                    stream->id = track;
                    stream->time_base = enc->GetCodecContext()->time_base;
                    stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    avcodec_parameters_from_context(stream->codecpar, enc->GetCodecContext());
                    DLL_Log("MediaEngine: Created audio-only muxer stream for track %d (stream idx %d)", track,
                            stream->index);
                }
            }
        }

        return true;

}
