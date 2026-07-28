                }
            }
        }

        return status;
    }

    bool WaitForFinalCfrAudioSourceCatchup(int64_t targetUs) {
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

    size_t StopCaptureSourcesAndDrainAudioLoop() {
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

    void AudioThreadEntry() noexcept {
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

    bool StartAudioThread() {
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

    void DrainStoppedCaptureQueuesBeforeFinalPull(int64_t targetUs) {
        if (!ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(audioRunning.load(), audioOnly, targetUs)) {
            return;
        }

        StopCaptureSourcesAndDrainAudioLoop();
    }

    void ApplyAudioTimelineReset(uint64_t generation, int64_t startQpcMs, bool preservePendingPackets) {
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
                static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::WithinSlack);
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

    void SyncAudioToFirstVideoFrame(int64_t startQpcMs, int64_t startQpc100ns, bool preservePendingPackets = false) {
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

    // Pull Model: source counters are diagnostic/source-local; each exported
    // track advances from trackTimelineSamples so source order cannot change
    // the muxed timeline.
    std::vector<int64_t> encodedSamplesPerSource;
    std::map<int, int64_t> trackTimelineSamples;
    std::map<int, uint64_t> trackRealMixedSamples;
    std::map<int, uint64_t> trackFullSilenceSamples;
    std::map<int, uint64_t> trackPartialSilenceSamples;

    std::map<int, bool> trackWasSilent;
    std::map<int, uint64_t> trackSilentSamples;
    std::map<int, uint64_t> trackSilentChunks;
    std::map<int, uint64_t> trackSilenceTransitions;
    std::map<int, uint64_t> trackLastSilenceLogTick;
    std::map<int, bool> trackBootstrapComplete;
    std::map<int, bool> trackFirstPullAfterBootstrap;
    std::map<int, int> trackBootstrapWaitLogCounters;

    // Cached track→source index map, built once in StartRecording to avoid
    // per-frame reconstruction (~120 rebuilds/sec at 120fps).
    std::map<int, std::vector<size_t>> cachedTrackToSources;

    // Adaptive CFR audio ingestion reservoir. The AudioLoop thread publishes the worst
    // observed distance between a freshly placed packet and the already-exported cursor;
    // PullAndEncodeAudio consumes it and deepens the pull lookahead so the producer can
    // overtake the consumer again. Deepening is sync-neutral: it moves only the pull
    // target, never a sample's timeline position. See audio_sync_utils.h for the policy.
    std::atomic<int64_t> audioIngestWorstHeadroomSamples{std::numeric_limits<int64_t>::max()};
    ce::audio::AudioIngestReservoirState audioIngestReservoir;
    int64_t audioIngestReservoirExtraMs = 0;
    uint64_t audioIngestReservoirEvalTick = 0;
    uint64_t audioIngestReservoirLogTick = 0;
    int64_t audioIngestReservoirLoggedMs = 0;
    int64_t audioIngestReservoirPeakMs = 0;

    // PullAndEncodeAudio counters — reset per recording to avoid stale state
    int warpCount = 0;
    int dropLogCounter = 0;
    int driftLogCounter = 0;
    std::map<int, int> trackSyncCheckCounters;
    std::map<int, TrackAudioFormat> trackAudioFormats;

    // Per-recording frame log counters (avoid statics that leak across recordings)
    int injectFrameLogCount = 0;
    int screengrabFrameLogCount = 0;
    int silenceLogCounter = 0;
    int mixLogCounter = 0;

    int64_t GetLastVideoEncodeTimeUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameEncodeTimeUs();
        return 0;
    }

    int64_t GetLastFrameFenceWaitUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;
    }

    bool WasLastFrameDeferred() const {
        if (videoEnc)
            return videoEnc->WasLastFrameDeferred();
        return false;
    }

    bool CanRepeatLastFrame() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        return videoEnc && recording && firstVideoFrameCommitted && videoEnc->CanRepeatLastFrame();
    }

    void ResetRepeatFrameCache() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ResetRepeatFrameCache();
        }
    }

    void ReleaseEncoderTextures() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ReleasePreservedEncoderTextures();
        }
    }

    void UpdateVideoEncoderSharedMem(void* sharedMem, void* shmemBuffer) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        sharedMemLayout = (SharedMemoryLayout*)sharedMem;
        if (videoEnc) {
            videoEnc->SetSharedMem((SharedMemoryLayout*)sharedMem, (ShmemBuffer*)shmemBuffer);
        }
    }

    void SetSourcePrefers10BitHint(bool prefer10Bit) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            DLL_Log("[VideoEncoder] SetSourcePrefers10Bit(%s)", prefer10Bit ? "true" : "false");
            videoEnc->SetSourcePrefers10Bit(prefer10Bit);
        }
    }

    void SetCursorCompositionSuppressedHint(bool suppressed) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            DLL_Log("[VideoEncoder] Cursor composition %s (capture frames %s the cursor)",
                    suppressed ? "suppressed" : "active", suppressed ? "already contain" : "do not contain");
            videoEnc->SetCursorCompositionSuppressed(suppressed);
        }
    }

    void SetActiveScreenGrab(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        activeScreenGrab = enabled;
    }

    void SetAudioOnly(bool enabled) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        audioOnly = enabled;
    }

    void InitAudioOnlyMuxer(const AppConfig* config) {
        const std::filesystem::path exeDir = ce::capture_output::GetExecutableDirectory();
        const std::filesystem::path outDir =
            ce::capture_output::ResolveCaptureDirectory(config->video.outputDir, exeDir);
        audioOnlyOutputReservation =
            ce::capture_output::ReservedCaptureOutput::Reserve(outDir, L"capture_audio", L"mka");
        if (!audioOnlyOutputReservation) {
            DLL_Log("MediaEngine: Failed to reserve collision-safe audio-only output in %s", outDir.string().c_str());
            audioOnlyFmtCtx = nullptr;
            return;
        }
        audioOnlyFilename = audioOnlyOutputReservation.Utf8Path();
        audioOnlyTrailerSucceeded = false;
        if (avformat_alloc_output_context2(&audioOnlyFmtCtx, nullptr, "matroska", audioOnlyFilename.c_str()) < 0) {
            DLL_Log("MediaEngine: Failed to create audio-only muxer");
            audioOnlyFmtCtx = nullptr;
            audioOnlyOutputReservation.CleanupOwnedFile();
            audioOnlyFilename.clear();
        }
    }

    void CleanupAudioOnlyMuxer() {
        int closeResult = 0;
        if (audioOnlyFmtCtx) {
            if (audioOnlyFmtCtx->pb) {
                closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0) {
                    DLL_Log("MediaEngine: Failed to close audio-only output: %d", closeResult);
                }
            }
            avformat_free_context(audioOnlyFmtCtx);
            audioOnlyFmtCtx = nullptr;
        }
        if (audioOnlyTrailerSucceeded && closeResult >= 0) {
            audioOnlyOutputReservation.Publish();
        } else {
            audioOnlyOutputReservation.CleanupOwnedFile();
        }
        audioOnlyTrailerSucceeded = false;
        audioOnlyFilename.clear();
    }

    // Trusted System QPC Frequency
    int64_t qpcFreq = 0;

    bool Init(const AppConfig* config) {
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
