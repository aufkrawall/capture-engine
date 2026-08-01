        // continuity policies (encoder-lag reservoirs, CFR source waits, and
        // overload trimming) do not apply when there is no video encoder.
        const bool isCfrRecording = !audioOnly && IsCfrRecording();
        const bool isWgcCfrRecording = !audioOnly && IsWgcCfrRecording();

        const uint64_t pullTick = GetTickCount64();

        // Adaptive ingestion reservoir. Deepening the pull lookahead is the only
        // recovery for a consumer that overran the live capture edge: it freezes the
        // pull target so the producers overtake the cursor again, with zero content
        // loss and zero timeline change. Inactive for VFR/audio-only. The stop drain
        // leaves the state untouched: it pulls at zero latency to the exact endpoint,
        // and clearing the reservoir there would make the retained lead look like drift.
        if (!forceDrain) {
            const uint64_t reservoirTick = pullTick;
            const bool reservoirActive = isCfrRecording;
            const int64_t elapsedSinceEvalMs = audioIngestReservoirEvalTick == 0
                                                   ? 0
                                                   : static_cast<int64_t>(reservoirTick - audioIngestReservoirEvalTick);
            audioIngestReservoirEvalTick = reservoirTick;
            const int64_t observedHeadroomSamples =
                audioIngestWorstHeadroomSamples.exchange(kNoAudioIngestHeadroom, std::memory_order_acq_rel);
            const bool headroomObserved = observedHeadroomSamples != kNoAudioIngestHeadroom;
            const int64_t observedHeadroomMs = headroomObserved ? (observedHeadroomSamples * 1000) / SAMPLE_RATE : 0;
            const auto reservoirDecision = ce::audio::ComputeAudioIngestReservoir(
                audioIngestReservoir, reservoirActive, headroomObserved, observedHeadroomMs, elapsedSinceEvalMs);
            audioIngestReservoir.extraMs = reservoirDecision.extraMs;
            audioIngestReservoir.healthyElapsedMs = reservoirDecision.healthyElapsedMs;
            audioIngestReservoirExtraMs = reservoirDecision.extraMs;
            audioIngestReservoirPeakMs = std::max(audioIngestReservoirPeakMs, reservoirDecision.extraMs);
            const bool reservoirChanged = reservoirDecision.extraMs != audioIngestReservoirLoggedMs;
            if (reservoirChanged && reservoirTick - audioIngestReservoirLogTick >= 1000) {
                DLL_Log(
                    "[PullAudio] Ingest reservoir %s: extra=%lldms (was %lldms) total=%lldms "
                    "worstHeadroom=%lldms observed=%d atCap=%d peak=%lldms. Scheduling lookahead only - sample "
                    "timeline positions, track lengths, and PTS are unchanged.",
                    reservoirDecision.extraMs > audioIngestReservoirLoggedMs ? "deepened" : "relaxed",
                    (long long)reservoirDecision.extraMs, (long long)audioIngestReservoirLoggedMs,
                    (long long)(ce::audio::kDefaultSteadyAudioPullLatencyMs + reservoirDecision.extraMs),
                    (long long)observedHeadroomMs, headroomObserved ? 1 : 0, reservoirDecision.atCap ? 1 : 0,
                    (long long)audioIngestReservoirPeakMs);
                audioIngestReservoirLoggedMs = reservoirDecision.extraMs;
                audioIngestReservoirLogTick = reservoirTick;
            }
        }
        const int64_t effectiveAudioPullLatencyMs = kSteadyAudioPullLatencyMs + audioIngestReservoirExtraMs;
        const int64_t baseTargetLatencySamples = (effectiveAudioPullLatencyMs * SAMPLE_RATE) / 1000;
        // Every track in this pass shares one target, so a per-track hold must request its
        // raise against the same baseline; otherwise N tracks with the same shortfall would
        // stack N raises and overshoot the reservoir.
        const int64_t reservoirPassBaseMs = audioIngestReservoirExtraMs;
        const int64_t wallVideoMs = this->videoElapsedMs.load();
        int64_t encodedVideoMs = 0;
        int64_t audioTargetUs = videoTimelineUs;
        if (videoEnc && !isCfrRecording) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
                audioTargetUs = encodedVideoUs;
            }
        }
        if (isCfrRecording && videoEnc) {
            int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
            }
            if (audioTargetUs <= 0) {
                audioTargetUs = videoEnc->GetExpectedFinalDurationUs();
            }
        }
        if (audioTargetUs <= 0) {
            audioTargetUs = wallVideoMs * 1000;
        }
        if (audioTargetUs <= 0) {
            return;
        }
        int64_t audioTargetMs = audioTargetUs / 1000;

        auto now = std::chrono::steady_clock::now();
        int64_t steadyElapsedUs = 0;
        if (this->recordingStartTime.time_since_epoch().count() > 0) {
            steadyElapsedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        }
        int64_t timelineShortfallMs = std::max<int64_t>(0, steadyElapsedUs - audioTargetUs) / 1000;

        const int64_t videoPipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
        const uint32_t configuredWgcOutputFps =
            (isWgcCfrRecording && config.video.fps > 0) ? static_cast<uint32_t>(config.video.fps) : 0u;
        uint32_t wgcTargetFps = isWgcCfrRecording ? (configuredWgcOutputFps > 0 ? configuredWgcOutputFps : 1u) : 0u;
        uint32_t wgcDeliveredFps = 0u;
        uint32_t wgcDeliveredMin250Fps = 0u;
        uint32_t wgcDeliveredMin500Fps = 0u;
        int64_t wgcBufferedVideoContentLagMs = 0;
        bool wgcCoverageLossActive = false;
        uint32_t wgcOverloadFlags = 0u;
        bool wgcEncoderBottlenecked = false;
        uint32_t wgcQueueEmptyTickPermille = 0u;
        uint32_t wgcBufferedAtTickMin = 0u;
        uint32_t wgcSingleFrameTickCount = 0u;
        int64_t wgcSelectionBiasUs = 0;
        if (isWgcCfrRecording && sharedMemLayout) {
            const auto& runtimeState = sharedMemLayout->runtimeState;
            wgcOverloadFlags = runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
            wgcEncoderBottlenecked = runtimeState.encoderBottlenecked.load(std::memory_order_relaxed) != 0;
            const uint32_t telemetryTargetFps = runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
            wgcTargetFps = telemetryTargetFps > 0 ? telemetryTargetFps : wgcTargetFps;
            wgcDeliveredFps = runtimeState.wgcDeliveredFramesPerSec.load(std::memory_order_relaxed);
            wgcDeliveredMin250Fps = runtimeState.wgcDeliveredMin250Fps.load(std::memory_order_relaxed);
            wgcDeliveredMin500Fps = runtimeState.wgcDeliveredMin500Fps.load(std::memory_order_relaxed);
            wgcBufferedVideoContentLagMs = ce::audio::ComputeWgcBufferedVideoContentLagMs(
                runtimeState.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed));
            wgcQueueEmptyTickPermille = runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed);
            wgcBufferedAtTickMin = runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed);
            wgcSingleFrameTickCount = runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed);
            wgcSelectionBiasUs = runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
        }
        const uint32_t wgcRecordingCadenceFps =
            ce::audio::GetWgcRecordingCadenceFps(configuredWgcOutputFps, wgcTargetFps);
        if (isWgcCfrRecording) {
            wgcCoverageLossActive = ce::audio::HasWgcUnrecoverableCoverageLoss(
                wgcRecordingCadenceFps, videoPipelineLagMs, wgcBufferedVideoContentLagMs, wgcEncoderBottlenecked,
                wgcDeliveredFps);
        }
        const auto wgcAudioLagTargets = ce::audio::ComputeWgcAudioLagTargets(
            videoPipelineLagMs, wgcBufferedVideoContentLagMs, isWgcCfrRecording && wgcCoverageLossActive,
            kWgcCoverageLossMaxBufferedLagMs);
        const int64_t wgcSteadyStateBufferedAudioLagMs =
            (isWgcCfrRecording && !wgcCoverageLossActive)
                ? ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(
                      wgcTargetFps, wgcDeliveredFps, wgcDeliveredMin250Fps, wgcDeliveredMin500Fps,
                      wgcEncoderBottlenecked, wgcQueueEmptyTickPermille, wgcBufferedAtTickMin, wgcSingleFrameTickCount)
                : 0;
        const uint32_t effectiveDeliveredFpsForAudioContinuity =
            ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(wgcDeliveredFps, wgcDeliveredMin250Fps,
                                                                      wgcDeliveredMin500Fps);
        const bool wgcEncoderOnlyOverload =
            isWgcCfrRecording && ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(
                                     wgcEncoderBottlenecked, wgcCoverageLossActive, wgcRecordingCadenceFps,
                                     effectiveDeliveredFpsForAudioContinuity, kWgcEncoderHealthyDeliveryMarginFps);
        int64_t wgcEncoderShortfallBufferedLagMs = 0;
        if (isWgcCfrRecording && wgcEncoderOnlyOverload && !kWgcPreferVideoRepeatsOverAudioCuts) {
            wgcEncoderShortfallBufferedLagMs =
                std::clamp<int64_t>(timelineShortfallMs, 0, kWgcEncoderShortfallBufferedLagMaxMs);
        }

        // In WGC CFR mode the scheduled audio timeline is authoritative. When repeated
        // video frames are the preferred recovery mechanism, keep audio targets tied to
        // actual buffered-video lag rather than raw wall-clock encoder lag.
        const int64_t screenGrabDriftLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcCfrDriftLagMs(wgcAudioLagTargets, kWgcPreferVideoRepeatsOverAudioCuts,
                                                     wgcEncoderShortfallBufferedLagMs)
                : 0;
        const int64_t effectiveSourceClockDriftLagMs = ce::audio::ResolveAudioSourceClockDriftLagMs(
            isCfrRecording, isWgcCfrRecording, screenGrabDriftLagMs, videoPipelineLagMs, timelineShortfallMs);
        const int64_t wgcSelectedContentLeadMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLeadMs(wgcSelectionBiasUs) : 0;
        const int64_t wgcSelectedContentLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLagMs(wgcSelectionBiasUs) : 0;
        const int64_t wgcVisualContentLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcVisualContentLagMs(timelineShortfallMs, wgcSelectedContentLeadMs,
                                                          wgcSelectedContentLagMs, kWgcVisualSyncMaxBufferedLagMs)
                : 0;
        const int64_t screenGrabTargetBufferLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcCfrTargetBufferLagMs(
                                    wgcAudioLagTargets, wgcSteadyStateBufferedAudioLagMs,
                                    kWgcPreferVideoRepeatsOverAudioCuts, wgcEncoderShortfallBufferedLagMs)
                              : 0;
        const int64_t effectiveAudioTargetBufferLagMs = ce::audio::ResolveAudioTargetBufferLagMs(
            isCfrRecording, isWgcCfrRecording, screenGrabTargetBufferLagMs, videoPipelineLagMs);
        const int64_t targetBufferedLagCapMs =
            isWgcCfrRecording ? std::max<int64_t>(kWgcCoverageLossMaxBufferedLagMs, effectiveAudioTargetBufferLagMs)
                              : kMaxPipelineLagContributionMs;
        uint32_t maxWgcAudioLeadExcessSamples = 0;

        // Wall-clock audio anchor: when the video timeline has fallen behind real time
        // (shortfall >500ms), use wall-clock elapsed time as the audio pull target instead
        // of the stalled video PTS. Only for non-CFR modes. In CFR mode, video PTS advances
        // at exactly the target framerate regardless of encoder wall-clock speed, so the
        // "shortfall" between wall clock and PTS is expected and harmless. Activating the
        // anchor in CFR mode would pull audio to wall clock, making it advance faster than
        // video PTS and causing unbounded desync.
        const int64_t wallVideoLagMs = timelineShortfallMs;
        if (ce::audio::ShouldAllowWallClockAudioAnchor(isCfrRecording, forceDrain, wallVideoLagMs)) {
            const int64_t steadyElapsedMs = steadyElapsedUs / 1000;
            if (steadyElapsedMs > audioTargetMs && steadyElapsedMs - audioTargetMs > 200) {
                audioTargetUs = steadyElapsedUs;
                audioTargetMs = steadyElapsedMs;
                timelineShortfallMs = 0;
                if (dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Using wall-clock audio anchor: videoPts=%lldms, wallClock=%lldms, "
                        "pipelineLag=%lldms, shortfall=%lldms",
                        videoTimelineUs / 1000, steadyElapsedMs, videoPipelineLagMs, wallVideoLagMs);
                }
            }
        }
        const bool cfrTimelineRecoveryActive =
            ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(
                isCfrRecording, forceDrain, timelineShortfallMs, wgcEncoderBottlenecked);

        if (encodedSamplesPerSource.size() != audioSources.size()) {
            encodedSamplesPerSource.resize(audioSources.size(), 0);
        }

        const auto& trackToSources = cachedTrackToSources;
        for (const auto& kv : trackToSources) {
            int track = kv.first;
            const auto& srcIndices = kv.second;
            if (srcIndices.empty())
                continue;
            const TrackAudioFormat trackFormat = GetTrackAudioFormat(track);
            const int CHANNELS = std::clamp(trackFormat.channels, 1, 8);
            const uint32_t CHANNEL_MASK =
                trackFormat.channelMask != 0 ? trackFormat.channelMask : DefaultChannelMaskForChannels(CHANNELS);

            bool trackAllPrimed = true;
            int64_t trackMaxObservedLateStartMs = 0;
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);

                size_t primedSampleCount = ce::audio::ComputeBufferedRealAudioSamples(
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

            const bool trackStartupSettled =
                ce::audio::IsTrackAudioStartupSettled(trackBootstrapComplete[track], trackAllPrimed);
            int64_t trackAudioPullLatencyMs =
                forceDrain ? 0
                           : ce::audio::ComputeAudioPullLatencyMs(effectiveAudioPullLatencyMs, trackStartupSettled,
                                                                  trackMaxObservedLateStartMs);
            if (isCfrRecording) {
                trackAudioPullLatencyMs = ce::audio::ComputeSettledCfrAudioPullLatencyMs(
                    trackAudioPullLatencyMs, trackStartupSettled, trackAllPrimed, effectiveAudioPullLatencyMs);
            }
            const int64_t trackAudioTargetUs = audioTargetUs - (std::max<int64_t>(trackAudioPullLatencyMs, 0) * 1000);
            const int64_t trackAudioTargetMs = trackAudioTargetUs > 0 ? (trackAudioTargetUs / 1000) : 0;
            if (trackAudioTargetUs <= 0) {
                continue;
            }

            const int64_t targetSamples = ce::audio::ComputeDurationUsToSamples(trackAudioTargetUs, SAMPLE_RATE);
            const int64_t targetBufferedSamples = ce::audio::ComputeBufferedAudioTargetSamples(
                SAMPLE_RATE, baseTargetLatencySamples, effectiveAudioTargetBufferLagMs, targetBufferedLagCapMs);

            bool trackReadyForBootstrap = true;
            constexpr size_t kMinBootstrapRealSamples = static_cast<size_t>(SAMPLE_RATE / 40);  // 25ms
            const size_t requiredBootstrapSamples =
                ce::audio::ComputeRequiredBootstrapRealSamples(targetSamples, kMinBootstrapRealSamples);
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                size_t bufferedRealSamples = ce::audio::ComputeBufferedRealAudioSamples(
                    src.postResampleBuffer.size() / CHANNELS, src.startupSyntheticPostSamples);
                if (src.ringBuffer) {
                    bufferedRealSamples += ce::audio::ComputeBufferedRealAudioSamples(
                        src.ringBuffer->GetAvailable() / CHANNELS, src.startupSyntheticRingSamples);
                }
                const bool srcReady = ce::audio::IsSourceBootstrapReady(
                    src.bootstrapComplete, src.timelineValid, src.isPrimed, isAppAudioSource, bufferedRealSamples,
                    requiredBootstrapSamples, src.sawSyncPendingPackets);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool packetlessSilenceReady = ce::audio::ShouldBootstrapPacketlessSourceAsSilence(
                    isCfrRecording, src.timelineValid, bufferedRealSamples, targetSamples, requiredBootstrapSamples);
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                const bool srcTimelineReady = ce::audio::IsSourceBootstrapTimelineReady(
                    src.bootstrapComplete, optionalUnstarted, srcReady, packetlessSilenceReady, bufferedTimelineSamples,
                    targetSamples);
                trackReadyForBootstrap = trackReadyForBootstrap && srcTimelineReady;
            }

            if (!trackBootstrapComplete[track]) {
                const bool forcedBootstrap = forceDrain;
                if (!trackReadyForBootstrap && !forcedBootstrap) {
                    int& waitLogCounter = trackBootstrapWaitLogCounters[track];
                    if (waitLogCounter++ % 120 == 0) {
                        DLL_Log("[PullAudio] Track %d bootstrap pending - target=%lldms samples=%lld", track,
                                trackAudioTargetMs, targetSamples);
                    }
                    continue;
                }

                uint64_t bootstrapTrimmed = 0;
                uint64_t bootstrapProtected = 0;
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

            size_t firstSrcIdx = srcIndices[0];
            int64_t& trackCursorSamples = trackTimelineSamples[track];
            int64_t pendingSamples = targetSamples - trackCursorSamples;
            if (pendingSamples <= 0)
                continue;

            const bool initialTrackCatchup =
                trackFirstPullAfterBootstrap.count(track) && trackFirstPullAfterBootstrap[track];
            const bool overloadPullQuantum = (isWgcCfrRecording && (wgcOverloadFlags & 0x1u) != 0) ||
                                             (isCfrRecording && !isWgcCfrRecording && timelineShortfallMs > 100);
            int64_t samplesToEncode = ce::audio::ComputeAudioSamplesToEncode(
                pendingSamples, isCfrRecording, trackStartupSettled, forceDrain, initialTrackCatchup,
                overloadPullQuantum, ce::audio::kDefaultAudioPullQuantumSamples, kOverloadAudioPullQuantumSamples);
            if (samplesToEncode <= 0) {
                continue;
            }
            if (initialTrackCatchup) {
                trackFirstPullAfterBootstrap[track] = false;
            }

            const int64_t MAX_GAP_SAMPLES = (SAMPLE_RATE * 2);
            const int64_t MAX_SILENCE_CHUNK = SAMPLE_RATE / 2;
            // A track this far behind the live target is catching up after a read-stall (alt-tab, DPC
            // spike, encoder overload). It MUST make forward progress every pull: deferring to wait for
            // an under-buffered source would freeze it forever, because a co-mixed source sitting at the
            // live edge (e.g. a second app keeping up at ~100ms) NEVER accrues the full catch-up chunk.
            // That freeze is exactly how a multi-app track went permanently silent after one app's
            // backlog stalled it - the track deferred every iteration, its cursor froze, and the stalled
            // app's ring saturated. Only fires when >2s behind (a real stall), so normal pulls keep the
            // defer/buffer-wait protection untouched.
            const bool trackLargeBacklogDrain =
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

            bool deferForSourceBuffer = false;
            const bool finalStopDrain = audioStopDrainRequested.load(std::memory_order_acquire) ||
                                        audioFinalizingCfrStop.load(std::memory_order_acquire);
            constexpr int64_t kSparseStartedPartialSilenceThresholdSamples =
                ce::audio::kDefaultAudioPullQuantumSamples * 4;
            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];
                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool appCaptureRouteEnded =
                    src.appCaptureRouteEnded && src.appCaptureRouteEnded->load(std::memory_order_acquire);
                const bool inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        isCfrRecording, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    isCfrRecording, src.timelineValid, src.bootstrapComplete, optionalUnstarted, finalStopDrain);
                const size_t bufferedTimelineSamples = GetBufferedTimelineSamples(src);
                const bool sparseStartedSourceMaySilence =
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
                const int64_t sourceIngestIdleMs = src.lastRealPacketIngestTick == 0
                                                       ? std::numeric_limits<int64_t>::max()
                                                       : static_cast<int64_t>(pullTick - src.lastRealPacketIngestTick);
                const bool sourceRecentlyDeliveredRealPackets =
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
                continue;
            }

            size_t totalFloats = samplesToEncode * CHANNELS;
            std::vector<float> mixBuffer(totalFloats, 0.0f);
            int activeSources = 0;
            int eligibleSources = 0;

            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];

                const bool isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                const bool optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                const bool appCaptureRouteEnded = isAppAudioSource && src.appCaptureRouteEnded &&
                                                  src.appCaptureRouteEnded->load(std::memory_order_acquire);
                const bool inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        isCfrRecording, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                const bool sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    isCfrRecording, src.timelineValid, src.bootstrapComplete, optionalUnstarted, finalStopDrain);
                const bool sparseStartedSourceMaySilence =
                    ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
                        sparseStartedSourceCanSilence, GetBufferedTimelineSamples(src), samplesToEncode,
                        kSparseStartedPartialSilenceThresholdSamples) ||
                    inactiveStartedAppSourceMaySilence;
                const bool expectedTimelineSilence =
                    sparseStartedSourceMaySilence ||
                    ce::audio::IsExpectedSourceTimelineSilence(isAppAudioSource, appCaptureRouteEnded,
                                                               src.sourceType == AudioConfig::SystemAudio,
                                                               src.hasAlignedStart);
                if (optionalUnstarted) {
                    continue;
                }
                ++eligibleSources;

                size_t droppedFloats = src.ringBuffer->GetAndClearDroppedSamples();
                if (droppedFloats > 0) {
                    size_t droppedSamples = droppedFloats / CHANNELS;
                    src.overflowDropSamples += droppedSamples;
                    DLL_Log("[PullAudio] WARNING: Ring buffer overflow - %zu samples dropped for src=%d",
                            droppedSamples, (int)srcIdx);
                    src.syncSamplesOutput += (int64_t)droppedSamples;
                }

                size_t retainedFloats = src.ringBuffer->GetAndClearRetainedSamples();
                if (retainedFloats > 0) {
                    size_t retainedSamples = retainedFloats / CHANNELS;
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, retainedSamples);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, retainedSamples);
                    src.retainedNewestTrimSamples += retainedSamples;
                    src.pendingRetainedTrimSamples += retainedSamples;
                    src.pendingRetainedTrimEvents++;
                    src.latencyTrimSamples += retainedSamples;
                    src.pendingLatencyTrimSamples += retainedSamples;
                    src.pendingLatencyTrimEvents++;
                    if (isCfrRecording) {
                        const uint64_t nowTick = GetTickCount64();
                        if (nowTick - src.lastRetainedTrimWarnTick >= 1000) {
                            const size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            const size_t rbCapacity = src.ringBuffer->GetCapacity() / CHANNELS;
                            DLL_Log(
                                "[PullAudio] WARNING: CFR audio headroom exhausted - trimmed %zu oldest samples "
                                "for src=%d to retain newest audio (buffered=%zu target=%lld cap=%zu "
                                "pipelineLag=%lldms). This may cause audible discontinuities; encoder/capture "
                                "throughput is behind real time.",
                                retainedSamples, (int)srcIdx, rbAvailable, targetBufferedSamples, rbCapacity,
                                videoPipelineLagMs);
                            src.lastRetainedTrimWarnTick = nowTick;
                        }
                    }
                }

                const int64_t remainingStartupProtectionSamples = std::max<int64_t>(
                    0, static_cast<int64_t>(src.startupGapProtectionSamples) - encodedSamplesPerSource[srcIdx]);
                const bool startupTimelineProtected = remainingStartupProtectionSamples > 0;

                const int64_t targetLatencySamples = targetBufferedSamples;
                if (src.bootstrapComplete && src.syncResampler && src.syncResampler->IsReady()) {
                    size_t rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                    const int64_t expectedLeadSamplesForCorrection =
                        std::max<int64_t>(targetLatencySamples,
                                          baseTargetLatencySamples +
                                              (effectiveSourceClockDriftLagMs * SAMPLE_RATE / 1000));
                    const int64_t appDrainBudgetSamples = static_cast<int64_t>(rbAvailable);
                    const auto appAudioDrainBudgetDecision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
                        isCfrRecording, src.sourceType == AudioConfig::AppAudio, forceDrain, trackStartupSettled,
                        startupTimelineProtected, cfrTimelineRecoveryActive, appDrainBudgetSamples,
                        expectedLeadSamplesForCorrection,
                        kMinCompensationBufferSamples, static_cast<int64_t>(SAMPLE_RATE) * 10,
                        kAppAudioDrainMaxPitchPercent, kAppAudioDrainSlackSamples, kAppAudioDrainDeadbandSamples);
                    const double maxCompensationPercent =
                        appAudioDrainBudgetDecision.active
                            ? kAppAudioDrainMaxPitchPercent
                            : (isCfrRecording ? kTier1MaxPitchPercent : kDefaultMaxCompensationPercent);
                    src.syncResampler->SetMaxCompensationPercent(maxCompensationPercent);
                    const bool allowWgcCoverageLossTrim =
                        isWgcCfrRecording && wgcCoverageLossActive && !kWgcPreferVideoRepeatsOverAudioCuts &&
                        static_cast<int64_t>(rbAvailable) > targetLatencySamples + kWgcCoverageLossLeadSlackSamples;
                    if (!allowWgcCoverageLossTrim) {
                        src.wgcCoverageLossTrimAccumulator = 0.0;
                    }
                    if (!forceDrain && allowWgcCoverageLossTrim && !startupTimelineProtected) {
                        const int64_t dropSamplesTotal = static_cast<int64_t>(rbAvailable) -
                                                         (targetLatencySamples + kWgcCoverageLossLeadSlackSamples);
                        int64_t dropSamples = ce::audio::ComputeWgcCoverageLossTrimSamples(
                            samplesToEncode,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs),
                            src.wgcCoverageLossTrimAccumulator, kWgcCoverageLossMaxDropPerCall);
                        dropSamples = std::min(dropSamples, dropSamplesTotal);
                        if (dropSamples > 0 && src.ringBuffer) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)dropSamples * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.coverageLossTrimSamples += trimmedSamples;
                            src.pendingCoverageLossTrimSamples += trimmedSamples;
                            src.pendingCoverageLossTrimEvents++;
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimEvents++;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] WGC overload sync trim: src %d ahead by %lld samples - trimming %zu "
                                    "(target=%lld, slack=%lld, pipelineLag=%lldms, contentLag=%lldms, delivered=%u/%u "
                                    "fps, ratio=%.3f%%)",
                                    (int)srcIdx, static_cast<int64_t>(rbAvailable) - targetLatencySamples,
                                    trimmedSamples, targetLatencySamples, kWgcCoverageLossLeadSlackSamples,
                                    videoPipelineLagMs, wgcBufferedVideoContentLagMs, wgcDeliveredFps, wgcTargetFps,
                                    ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs,
                                                                           wgcBufferedVideoContentLagMs) *
                                        100.0);
                            }
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                    } else if (!forceDrain && isWgcCfrRecording && wgcCoverageLossActive &&
                               kWgcPreferVideoRepeatsOverAudioCuts &&
                               static_cast<int64_t>(rbAvailable) >
                                   targetLatencySamples + kWgcCoverageLossLeadSlackSamples &&
                               dropLogCounter++ % 500 == 0) {
                        DLL_Log(
                            "[PullAudio] WGC source-limited CFR repeats active: preserving continuous audio and "
                            "expecting CFR video "
                            "repeats to absorb mismatch (src=%d ahead=%lld target=%lld "
                            "slack=%lld pipelineLag=%lldms contentLag=%lldms wgcFrameLead=%lldms "
                            "wgcFrameLag=%lldms wgcSelBias=%lldus delivered=%u/%u fps ratio=%.3f%%)",
                            (int)srcIdx, static_cast<int64_t>(rbAvailable) - targetLatencySamples, targetLatencySamples,
                            kWgcCoverageLossLeadSlackSamples, videoPipelineLagMs, wgcBufferedVideoContentLagMs,
                            wgcSelectedContentLeadMs, wgcVisualContentLagMs, wgcSelectionBiasUs, wgcDeliveredFps,
                            wgcTargetFps,
                            ce::audio::ComputeWgcCoverageLossRatio(videoPipelineLagMs, wgcBufferedVideoContentLagMs) *
                                100.0);
                    } else if (!forceDrain && !startupTimelineProtected && !isCfrRecording) {
                        int64_t dropSamplesTotal = ce::audio::ComputeLeadTrimExcessSamples(
                            static_cast<int64_t>(rbAvailable), targetLatencySamples, kRuntimeMaxLeadSamples,
                            kLatencyTrimHysteresisSamples);
                        int64_t dropSamples = std::min(dropSamplesTotal, kRuntimeMaxDropPerCall);
                        if (dropSamples > 0 && src.ringBuffer) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)dropSamples * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
