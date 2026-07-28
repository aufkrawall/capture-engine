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

    int64_t GetLastVideoFenceWaitUs() const {
        if (videoEnc)
            return videoEnc->GetLastFrameFenceWaitUs();
        return 0;
    }

    void ResetAudioPullStateForRecording() {
        encodedSamplesPerSource.assign(audioSources.size(), 0);
        trackTimelineSamples.clear();
        trackRealMixedSamples.clear();
        trackFullSilenceSamples.clear();
        trackPartialSilenceSamples.clear();
        trackWasSilent.clear();
        trackSilentSamples.clear();
        trackSilentChunks.clear();
        trackSilenceTransitions.clear();
        trackLastSilenceLogTick.clear();
        trackBootstrapComplete.clear();
        trackFirstPullAfterBootstrap.clear();
        trackBootstrapWaitLogCounters.clear();
        cachedTrackToSources.clear();
        for (size_t i = 0; i < audioSources.size(); ++i) {
            auto& src = audioSources[i];
            if (!src.ringBuffer || !src.sharedEncoderPtr) {
                continue;
            }
            cachedTrackToSources[src.track].push_back(i);
            trackTimelineSamples[src.track] = 0;
            trackRealMixedSamples[src.track] = 0;
            trackFullSilenceSamples[src.track] = 0;
            trackPartialSilenceSamples[src.track] = 0;
        }

        audioIngestWorstHeadroomSamples.store(kNoAudioIngestHeadroom, std::memory_order_relaxed);
        audioIngestReservoir = ce::audio::AudioIngestReservoirState{};
        audioIngestReservoirExtraMs = 0;
        audioIngestReservoirEvalTick = 0;
        audioIngestReservoirLogTick = 0;
        audioIngestReservoirLoggedMs = 0;
        audioIngestReservoirPeakMs = 0;

        warpCount = 0;
        dropLogCounter = 0;
        driftLogCounter = 0;
        trackSyncCheckCounters.clear();
        injectFrameLogCount = 0;
        screengrabFrameLogCount = 0;
        silenceLogCounter = 0;
        mixLogCounter = 0;
    }

    bool StartRecording() {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (recording)
            return true;
        processLoopbackIntegrityFailureSignaled = false;
        if (sharedMemLayout) {
            sharedMemLayout->runtimeState.recordingFailureCode.store(static_cast<uint32_t>(RecordingFailureCode::None),
                                                                     std::memory_order_release);
        }

        // Audio-only: open muxer, write header, skip video pipeline entirely
        if (audioOnly) {
            if (!audioOnlyFmtCtx) {
                DLL_Log("MediaEngine: Audio-only muxer not initialized");
                return false;
            }
            if (!audioOnlyOutputReservation.ReleaseToWriter()) {
                DLL_Log("MediaEngine: Reserved audio-only output identity changed before mux open: %s",
                        audioOnlyFilename.c_str());
                return false;
            }
            if (avio_open(&audioOnlyFmtCtx->pb, audioOnlyFilename.c_str(), AVIO_FLAG_WRITE) < 0) {
                DLL_Log("MediaEngine: Failed to open audio-only output file: %s", audioOnlyFilename.c_str());
                return false;
            }
            if (!ce::media::RequireMicrosecondMatroskaTimestampPrecision(audioOnlyFmtCtx)) {
                DLL_Log("MediaEngine: Matroska timestamp_precision=1000 is required for audio-only output");
                const int closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("MediaEngine: Failed to close rejected audio-only output: %d", closeResult);
                audioOnlyOutputReservation.CleanupOwnedFile();
                return false;
            }
            AVDictionary* opts = nullptr;
            const int headerResult = avformat_write_header(audioOnlyFmtCtx, &opts);
            av_dict_free(&opts);
            if (headerResult < 0) {
                DLL_Log("MediaEngine: Failed to write audio-only header: %d", headerResult);
                const int closeResult = avio_closep(&audioOnlyFmtCtx->pb);
                if (closeResult < 0)
                    DLL_Log("MediaEngine: Failed to close audio-only output after header failure: %d", closeResult);
                audioOnlyOutputReservation.CleanupOwnedFile();
                return false;
            }
            DLL_Log("MediaEngine: Audio-only recording writing to %s", audioOnlyFilename.c_str());

            // Set stream indices from muxer - only on OWNER encoders (unique)
            {
                int ownerCount = 0;
                for (unsigned int si = 0; si < audioOnlyFmtCtx->nb_streams; si++) {
                    int targetTrack = audioOnlyFmtCtx->streams[si]->id;
                    for (auto& src : audioSources) {
                        if (src.encoder && src.track == targetTrack) {
                            src.encoder->SetStreamIndex((int)si);
                            DLL_Log("[AudioSetup] Stream %d -> track %d encoder %p", si, targetTrack,
                                    (void*)src.encoder.get());
                            ownerCount++;
                            break;
                        }
                    }
                }
                DLL_Log("[AudioSetup] %d owner encoders assigned, %d streams", ownerCount,
                        (int)audioOnlyFmtCtx->nb_streams);
            }

            audioSyncPending.store(false);
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            audioStopDrainRequested.store(false, std::memory_order_release);
            audioStopDrainComplete.store(false, std::memory_order_release);
            preservePendingStartupAudioPackets.store(false, std::memory_order_release);
            ResetAudioPullStateForRecording();

            LARGE_INTEGER audioStartQpc{};
            QueryPerformanceCounter(&audioStartQpc);
            const int64_t audioStartQpcMs =
                qpcFreq > 0 ? (audioStartQpc.QuadPart * 1000) / qpcFreq : static_cast<int64_t>(GetTickCount64());
            const int64_t audioStartQpc100ns =
                qpcFreq > 0 ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
                                  static_cast<uint64_t>(audioStartQpc.QuadPart), static_cast<uint64_t>(qpcFreq)))
                            : 0;
            SyncAudioToFirstVideoFrame(audioStartQpcMs, audioStartQpc100ns);
            recordingStartTime = std::chrono::steady_clock::now();
            recording = true;

            int startedCount = 0;
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                auto& src = audioSources[srcIdx];
                if (src.captureFanoutOwnerIndex != srcIdx) {
                    DLL_Log("[AudioRoute] Audio-only route src=%zu track=%d uses capture owner=%zu", srcIdx, src.track,
                            src.captureFanoutOwnerIndex);
                    continue;
                }
                bool started = false;
                if (src.sourceType == AudioConfig::AppAudio && src.appCapture) {
                    if (!src.config.processName.empty()) {
                        started = src.appCapture->StartByName(src.config.processName);
                    } else if (src.config.processId != 0) {
                        started = src.appCapture->StartByPID(src.config.processId);
                    }
                } else if (src.capture) {
                    bool isLoopback = (src.sourceType == AudioConfig::SystemAudio);
                    started = src.capture->Start(src.config.device, isLoopback);
                }
                if (started)
                    startedCount++;
            }
            // Collect unique encoder pointers for track padding
            trackEncoders.clear();
            int encCount = 0, shmCount = 0;
            for (auto& src : audioSources) {
                if (src.encoder) {
                    encCount++;
                    bool dup = false;
                    for (auto* e : trackEncoders) {
                        if (e == src.encoder.get()) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                        trackEncoders.push_back(src.encoder.get());
                }
                if (src.sharedEncoderPtr)
                    shmCount++;
            }
            DLL_Log("[StartAudio] trackEncoders: %zu unique from %d owners (%d shared), %zu sources",
                    trackEncoders.size(), encCount, shmCount, audioSources.size());
            if (startedCount > 0) {
                if (!StartAudioThread()) {
                    recording = false;
                    for (auto* encoder : trackEncoders) {
                        if (encoder) {
                            encoder->Stop();
                        }
                    }
                    if (audioOnlyFmtCtx) {
                        audioOnlyTrailerSucceeded = av_write_trailer(audioOnlyFmtCtx) >= 0;
                        CleanupAudioOnlyMuxer();
                    }
                    DLL_Log("MediaEngine: Audio-only recording start failed because the audio worker could not start");
                    return false;
                }
            }

            DLL_Log("MediaEngine: Audio-only recording started (%d audio source(s))", startedCount);
            return true;
        }

        if (!videoEnc)
            return false;
        for (auto& src : audioSources) {
            if (src.encoder) {
                // If encoder failed to reinit during Stop(), reinit now
                if (!src.encoder->IsReady()) {
                    DLL_Log("MediaEngine: Audio encoder not ready, attempting reinit");
                    if (!src.encoder->Reinit()) {
                        DLL_Log("MediaEngine: Audio encoder reinit failed, skipping");
                        continue;
                    }
                    DLL_Log("MediaEngine: Audio encoder reinit successful");
                }
                videoEnc->AddAudioContext(src.config, src.encoder->GetCodecContext(), src.track);
                src.sharedEncoderPtr = src.encoder.get();
            }
        }
        // Shared sources must re-acquire the pointer from their reference
        // (Actually simpler: just re-iterate and update sharedEncoderPtr?
        // No, sharedEncoderPtr points to local AudioEncoder instance in another
        // source. That instance is stable, but its internal codecCtx execution
        // pointer changed. VideoEncoder needs the new CodecCtx. audioSources
        // already have the right encoder object.)

        // Start Video (Write Header / Open File)
        if (!videoEnc->Start())
            return false;

        // Audio stream is now added in EnsureDevice after video stream
        // We don't add it here anymore - just set the index when it becomes
        // available The stream index will be set after first frame in ProcessFrame
        timingModeFrozenForSession = true;
        sessionUseVfr = config.video.useVFR;
        const bool preserveWgcStartupQueues = IsWgcCfrRecording();

        // Start Audio Capture and Processing Thread
        if (!audioSources.empty()) {
            // NOTE: Don't set recording start time here!
            // We defer this to the first video frame in ProcessFrameD3D11 for perfect
            // A/V sync. Audio data captured before first video frame will be
            // discarded. IMPORTANT: Prevent ringbuffer from filling/overflowing
            // before first video frame. We intentionally discard audio until the
            // first video frame establishes the timeline.
            audioSyncPending.store(true);
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            audioStopDrainRequested.store(false, std::memory_order_release);
            audioStopDrainComplete.store(false, std::memory_order_release);
            preservePendingStartupAudioPackets.store(preserveWgcStartupQueues, std::memory_order_release);
            if (preserveWgcStartupQueues) {
                DLL_Log(
                    "[A/V START] WGC CFR startup queue preservation armed until first video anchor "
                    "(bounded by capture queue depth; pre-anchor packets are filtered by QPC)");
            }

            // CRITICAL: Reset video clock for new recording to prevent stale
            // timestamps
            firstVideoFrameMs = 0;               // Reset for new recording
            firstVideoFrameCommitted = false;
            videoElapsedMs.store(0);             // CRITICAL: Reset video clock for new recording
                                                 // to prevent stale timestamps
            recordingStartSystemQPCMs.store(0);  // CRITICAL: Reset QPC start time for new recording
            recordingStartSystemQpc100ns.store(0);
            wgcStartupExtraDelayQpc.store(0, std::memory_order_release);
            injectTimelineState.Reset();
            d3d11TimelineState.Reset();

            // PULL MODEL: Reset audio encoding state for new recording.
            ResetAudioPullStateForRecording();

            // PULL MODEL: CRITICAL - Clear ring buffers to start fresh
            for (auto& src : audioSources) {
                if (src.ringBuffer) {
                    size_t prevAvail = src.ringBuffer->GetAvailable();
                    src.ringBuffer->Clear();
                    if (prevAvail > 0) {
                        DLL_Log("MediaEngine: Cleared stale ringBuffer with %zu samples", prevAvail);
                    }
                }

                // DRIFT COMPENSATION: Reset sync state for new recording
                if (src.syncResampler) {
                    src.syncResampler->Reset();
                }
                // Reset format-conversion resampler so no SWR state from the
                // previous recording bleeds into the new one.
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
                src.timelineValid = false;
                src.isPrimed = false;
                src.bootstrapComplete = false;
                src.pendingUnderrunRecoveryFade = false;
                src.sawSyncPendingPackets = false;
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
                src.epochResetRequested->store(0, std::memory_order_release);
                src.epochResetAcknowledged->store(0, std::memory_order_release);
                src.epochSyncTailFlushedGeneration = 0;
                src.lastEpochTransitionWaitLogTick = 0;
            }

            // Start all audio sources
            int startedCount = 0;
            for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
                auto& src = audioSources[srcIdx];
                // Recording start will be set when first video frame arrives

                if (src.captureFanoutOwnerIndex != srcIdx) {
                    DLL_Log("[AudioRoute] Route src=%zu track=%d uses capture owner=%zu", srcIdx, src.track,
                            src.captureFanoutOwnerIndex);
                    continue;
                }

                bool started = false;

                if (src.sourceType == AudioConfig::AppAudio && src.appCapture) {
                    // Start per-app audio capture
                    if (!src.config.processName.empty()) {
                        started = src.appCapture->StartByName(src.config.processName);
                        DLL_Log(
                            "MediaEngine: App audio source starting for process '%s' "
                            "(track=%d)",
                            src.config.processName.c_str(), src.track);
                    } else if (src.config.processId != 0) {
                        started = src.appCapture->StartByPID(src.config.processId);
                        DLL_Log("MediaEngine: App audio source starting for PID %lu (track=%d)", src.config.processId,
                                src.track);
                    }
                } else if (src.capture) {
                    // Start regular capture: loopback for system audio, device for
                    // microphone
                    bool isLoopback = (src.sourceType == AudioConfig::SystemAudio);
                    started = src.capture->Start(src.config.device, isLoopback);
                    DLL_Log("MediaEngine: Audio source started (track=%d, type=%d)", src.track, (int)src.sourceType);
                }

                if (started) {
                    startedCount++;
                } else {
                    DLL_Log("MediaEngine: Audio source failed to start (track=%d, type=%d)", src.track,
                            (int)src.sourceType);
                }
            }

            if (startedCount > 0) {
                DLL_Log(
                    "MediaEngine: %d audio source(s) started (sync pending first "
                    "video frame)",
                    startedCount);
                if (!StartAudioThread()) {
                    DLL_Log("MediaEngine: Audio worker unavailable; continuing video recording without live audio");
                }
            }
        }

        recording = true;
        return true;
    }

    void CancelUncommittedVideoRecording() {
        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            if (!recording) {
                return;
            }
            recording = false;
        }

        DLL_Log("[RecordingLifecycle] Cancelling recording before first live video frame");
        StopAudioCaptureSources(true);
        audioRunning.store(false, std::memory_order_release);
        audioStopDrainRequested.store(false, std::memory_order_release);
        audioStopDrainComplete.store(true, std::memory_order_release);
        audioDrainCv.notify_all();
        if (audioThread.joinable()) {
            audioThread.join();
        }

        for (auto& src : audioSources) {
            if (src.encoder) {
                src.encoder->Cancel();
            }
            if (src.ringBuffer) {
                src.ringBuffer->Clear();
            }
            if (src.syncResampler) {
                src.syncResampler->Reset();
            }
            src.resampler.reset();
            src.postResampleBuffer.clear();
        }

        audioSyncPending.store(false, std::memory_order_release);
        audioFinalizingCfrStop.store(false, std::memory_order_release);
        preservePendingStartupAudioPackets.store(false, std::memory_order_release);
        firstVideoFrameMs = 0;
        firstVideoFrameCommitted = false;
        lastVideoFrameMs = 0;
        videoElapsedMs.store(0, std::memory_order_release);
        recordingStartSystemQPCMs.store(0, std::memory_order_release);
        recordingStartSystemQpc100ns.store(0, std::memory_order_release);
        injectTimelineState.Reset();
        d3d11TimelineState.Reset();
        timingModeFrozenForSession = false;
        activeScreenGrab = false;

        if (videoEnc) {
            videoEnc->Cancel();
        }
        DLL_Log("[STOP SUMMARY] Recording cancelled before live output; staged media discarded");
    }

    void StopRecording(bool cancelUncommittedVideo = false) {
        // Audio-only: stop audio thread, write trailer, clean up
        if (audioOnly) {
            {
                std::lock_guard<std::recursive_mutex> lock(muxMutex);
                if (!recording)
                    return;
            }
            // Stop WASAPI/process-loopback first while AudioLoop is still
            // alive. Stop(false) performs each source's one-shot final drain;
            // the CV handshake then proves those committed packets were
            // consumed before the loop and muxer are torn down.
            const size_t preservedStopPackets = audioRunning.load(std::memory_order_acquire)
                                                    ? StopCaptureSourcesAndDrainAudioLoop()
                                                    : StopAudioCaptureSources(false);
            audioRunning = false;
            audioDrainCv.notify_all();
            if (audioThread.joinable())
                audioThread.join();
            {
                std::lock_guard<std::recursive_mutex> lock(muxMutex);
                recording = false;
            }
            DLL_Log("[StopAudio] Audio-only source tail preserved before loop shutdown: packets=%zu",
                    preservedStopPackets);

            // Pad unique stream encoders to equal length
            std::map<int, AudioEncoder*> streamEnc2;
            for (auto& src2 : audioSources) {
                if (src2.encoder) {
                    int si = src2.encoder->GetStreamIndex();
                    if (si >= 0)
                        streamEnc2[si] = src2.encoder.get();
                }
            }
            if (streamEnc2.size() > 1) {
                constexpr int kChunkSamples = 4096;
                int64_t target = 0;
                for (auto& [si, enc] : streamEnc2) {
                    int64_t s = enc->GetSamplesCount();
                    if (s > target)
                        target = s;
                }
                target = ((target + kChunkSamples - 1) / kChunkSamples) * kChunkSamples;
                for (auto& [si, enc] : streamEnc2) {
                    AVCodecContext* ctx = enc->GetCodecContext();
                    const int channels =
                        std::clamp(ctx && ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2, 1, 8);
                    std::vector<float> silence(kChunkSamples * channels, 0.0f);
                    int64_t cur = enc->GetSamplesCount();
                    int64_t pad = target - cur;
                    if (pad > 0) {
                        int64_t remaining = pad;
                        while (remaining > 0) {
                            int chunk = (int)(std::min)(remaining, (int64_t)kChunkSamples);
                            enc->EncodeSamples((const uint8_t*)silence.data(), chunk * channels * (int)sizeof(float),
                                               channels, 48000, 32, 32, channels * 4, true, GetTickCount64());
                            remaining -= chunk;
                        }
                    }
                }
                // Converge until all match
                for (int iter = 0; iter < 10; iter++) {
                    target = 0;
                    for (auto& [si, enc] : streamEnc2) {
                        int64_t s = enc->GetSamplesCount();
                        if (s > target)
                            target = s;
                    }
                    bool allMatch = true;
                    for (auto& [si, enc] : streamEnc2) {
                        int64_t cur = enc->GetSamplesCount();
                        int64_t pad = target - cur;
                        if (pad > 0) {
                            allMatch = false;
                            AVCodecContext* ctx = enc->GetCodecContext();
                            const int channels = std::clamp(
                                ctx && ctx->ch_layout.nb_channels > 0 ? ctx->ch_layout.nb_channels : 2, 1, 8);
                            std::vector<float> silence(kChunkSamples * channels, 0.0f);
                            int64_t remaining = pad;
                            while (remaining > 0) {
                                int chunk = (int)(std::min)(remaining, (int64_t)kChunkSamples);
                                enc->EncodeSamples((const uint8_t*)silence.data(),
                                                   chunk * channels * (int)sizeof(float), channels, 48000, 32, 32,
                                                   channels * 4, true, GetTickCount64());
                                remaining -= chunk;
                            }
                            DLL_Log("[StopAudio] Convergence iter %d: stream %d +%lld to %lld", iter, si,
                                    (long long)pad, (long long)target);
                        }
                    }
                    if (allMatch)
                        break;
                }
                for (auto& [si, enc] : streamEnc2) {
                    DLL_Log("[StopAudio] Final stream %d: %lld samples", si, (long long)enc->GetSamplesCount());
                }
            }
            // Stop each unique encoder exactly once
            for (auto& [si, enc] : streamEnc2) {
                for (const auto& src : audioSources) {
                    if (src.encoder.get() == enc) {
                        enc->SetExpectedSourceSilenceSamples(static_cast<int64_t>(trackFullSilenceSamples[src.track]));
                        break;
                    }
                }
                enc->Stop();
            }
            for (auto& src : audioSources) {
                if (src.ringBuffer)
                    src.ringBuffer->Clear();
                if (src.syncResampler)
                    src.syncResampler->Reset();
            }
            if (audioOnlyFmtCtx) {
                audioOnlyTrailerSucceeded = av_write_trailer(audioOnlyFmtCtx) >= 0;
                if (!audioOnlyTrailerSucceeded) {
                    DLL_Log("[StopAudio] ERROR: Audio-only trailer write failed");
                }
                DLL_Log("[StopAudio] Audio-only recording finalized: %s", audioOnlyFilename.c_str());
                CleanupAudioOnlyMuxer();
            }
            firstVideoFrameMs = 0;
            firstVideoFrameCommitted = false;
            lastVideoFrameMs = 0;
            recordingStartSystemQPCMs.store(0);
            recordingStartSystemQpc100ns.store(0);
            injectTimelineState.Reset();
            d3d11TimelineState.Reset();
            audioOnly = false;
            DLL_Log(
                "[AVSyncAuto] stop_summary: audioOnly=1 resolvedRenderLatencyMs=%.3f confidence=%s reason=%s "
                "usedAudioProbe=%d",
                static_cast<double>(config.avSyncResolvedRenderLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str(), config.avSyncUsedAudioProbe ? 1 : 0);
            DLL_Log("[STOP SUMMARY] Audio-only recording finalized");
            return;
        }

        if (cancelUncommittedVideo && !firstVideoFrameCommitted) {
            CancelUncommittedVideoRecording();
            return;
