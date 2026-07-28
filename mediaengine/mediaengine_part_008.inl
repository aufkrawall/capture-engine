                                routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                            DLL_Log(
                                "[AppLatency] state src=%zu track=%d elevated=%d delayMs=%u targetMs=%u excessMs=%u "
                                "drain=%d reason=%s compDelta=%d comp=%.4f%% rbAvail=%zu queuePending=%zu "
                                "queueOverrun=%llu/%llu underruns=%u",
                                srcIdx, track, appLatencyElevated ? 1 : 0, appDelayMs, appTargetMs, appExcessMs,
                                src.appAudioBacklogDrainActive ? 1 : 0,
                                ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                                appCompPct, rbAvailSamples, pendingPackets,
                                static_cast<unsigned long long>(queueOverrunPackets),
                                static_cast<unsigned long long>(queueOverrunFrames), src.ringBufferUnderrunCount);
                            src.appLatencyWarnActive = appLatencyElevated;
                        }
                        if (appLatencyElevated && nowConsumeTick - src.lastAppLatencyWarnTick >= 5000) {
                            ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                            const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                            const uint64_t queueOverrunPackets =
                                routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                            const uint64_t queueOverrunFrames =
                                routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                            DLL_Log(
                                "[AppLatency] WARNING: app audio src=%zu track=%d delayMs=%u targetMs=%u excessMs=%u "
                                "rbAvail=%zu drain=%d reason=%s compDelta=%d comp=%.4f%% rateCompActive=%d "
                                "underruns=%u queuePending=%zu queueOverrun=%llu/%llu. Content backlog should drain "
                                "toward the video target without trims.",
                                srcIdx, track, appDelayMs, appTargetMs, appExcessMs, rbAvailSamples,
                                src.appAudioBacklogDrainActive ? 1 : 0,
                                ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                                appCompPct, src.rateCompActive ? 1 : 0, src.ringBufferUnderrunCount, pendingPackets,
                                static_cast<unsigned long long>(queueOverrunPackets),
                                static_cast<unsigned long long>(queueOverrunFrames));
                            src.lastAppLatencyWarnTick = nowConsumeTick;
                        }
                    }

                    if (!forceDrain && nowConsumeTick - src.lastAppConsumeDiagTick >= 1000) {
                        ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                        const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                        const uint64_t queueOverrunPackets =
                            routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                        const uint64_t queueOverrunFrames =
                            routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                        DLL_Log(
                            "[AppDiag] consume src=%zu track=%d delayMs=%u targetMs=%u excessMs=%u "
                            "realCopied=%zu postResampleBuf=%zu rbAvail=%zu syncReady=%d drain=%d reason=%s "
                            "compDelta=%d comp=%.4f%% rateCompActive=%d underruns=%u queuePending=%zu "
                            "queueOverrun=%llu/%llu trims(lat=%llu cat=%u/%llu)%s",
                            srcIdx, track, appDelayMs, appTargetMs, appExcessMs, realCopiedSamples,
                            src.postResampleBuffer.size() / CHANNELS, rbAvailSamples,
                            (src.syncResampler && src.syncResampler->IsReady()) ? 1 : 0,
                            src.appAudioBacklogDrainActive ? 1 : 0,
                            ce::audio::CfrAppAudioBacklogDrainReasonName(appDrainReason), src.currentRateDelta,
                            appCompPct, src.rateCompActive ? 1 : 0, src.ringBufferUnderrunCount, pendingPackets,
                            static_cast<unsigned long long>(queueOverrunPackets),
                            static_cast<unsigned long long>(queueOverrunFrames),
                            static_cast<unsigned long long>(src.latencyTrimSamples), src.catastrophicResyncEvents,
                            static_cast<unsigned long long>(src.catastrophicResyncSamples),
                            starvedWithRingData ? " STARVED_WITH_RING_DATA" : "");
                        src.lastAppConsumeDiagTick = nowConsumeTick;
                    }
                }
                if (toCopy < totalFloats) {
                    // Underrun: apply a short fade-out on the last real samples
                    // before the silence boundary to prevent audible clicks.
                    if (toCopy > 0) {
                        constexpr int kFadeSamples = 8;  // ~0.17ms at 48kHz
                        int realSamples = static_cast<int>(toCopy / CHANNELS);
                        int fadeStart = std::max(0, realSamples - kFadeSamples);
                        for (int s = fadeStart; s < realSamples; ++s) {
                            float alpha = static_cast<float>(realSamples - s) / static_cast<float>(kFadeSamples + 1);
                            const size_t base = static_cast<size_t>(s) * CHANNELS;
                            for (int ch = 0; ch < CHANNELS; ++ch) {
                                srcData[base + ch] *= alpha;
                            }
                        }
                    }
                    size_t padSamples = (totalFloats - toCopy) / CHANNELS;
                    const bool startupPadding = !src.bootstrapComplete;
                    if (!startupPadding && !expectedTimelineSilence) {
                        src.underrunPadSamples += padSamples;
                        if (src.syncResampler && src.syncResampler->IsReady() && src.rateCompActive) {
                            if (swr_set_compensation(src.syncResampler->GetSwrContext(), 0, SAMPLE_RATE * 10) >= 0) {
                                src.prevLeadSamples = 0;
                                src.prevLeadSnapshotMs = 0;
                                src.lastRateUpdateMs = 0;
                                src.currentRateDelta = 0;
                                src.targetRateDelta = 0;
                                src.rateCompActive = false;
                                src.targetRateSaturated = false;
                                src.ringBufferPeakSamples = 0;
                                src.ringBufferUnderrunCount = 0;
                            }
                        }
                        constexpr size_t kMaxSinglePadSamples = SAMPLE_RATE / 50;  // 20ms
                        if (padSamples > kMaxSinglePadSamples && dropLogCounter++ % 20 == 0) {
                            DLL_Log(
                                "[PullAudio] WARNING: Large single padding event - src %d padding %zu samples "
                                "(cap=%zu) - indicates timing problem, not transient underrun",
                                (int)srcIdx, padSamples, kMaxSinglePadSamples);
                        }
                        constexpr size_t kTotalPadWarningSamples = SAMPLE_RATE / 10;  // 100ms
                        if (src.underrunPadSamples > kTotalPadWarningSamples &&
                            (src.underrunPadSamples - padSamples) <= kTotalPadWarningSamples) {
                            DLL_Log(
                                "[PullAudio] WARNING: Total silence padding exceeded %zu samples (%.1fms) for src %d - "
                                "ring buffer is being drained too fast",
                                kTotalPadWarningSamples, (double)src.underrunPadSamples * 1000.0 / SAMPLE_RATE,
                                (int)srcIdx);
                        }
                    }
                    // Expected timeline silence (for example a focused game's process loopback muting on
                    // alt-tab) is not an underrun, but the first real samples after it still need a short
                    // route-local fade-in so an arbitrary waveform phase cannot click at the silence seam.
                    src.pendingUnderrunRecoveryFade = !startupPadding;
                    if (startupPadding && realCopiedSamples == 0) {
                        src.pendingStartupJoinFade = true;
                    }
                    if (!startupPadding && !expectedTimelineSilence && src.alignedStartMs >= 0 &&
                        padSamples >= (size_t)(SAMPLE_RATE / 200) && dropLogCounter++ % 100 == 0) {
                        DLL_Log(
                            "[PullAudio] WARNING: Source underrun - src %d padding %zu samples with silence "
                            "(available=%zu needed=%zu forceDrain=%d)",
                            (int)srcIdx, padSamples, available / CHANNELS, totalFloats / CHANNELS, forceDrain ? 1 : 0);
                    } else if (startupPadding && padSamples >= (size_t)(SAMPLE_RATE / 50) &&
                               dropLogCounter++ % 200 == 0) {
                        DLL_Log(
                            "[PullAudio] Startup padding - src %d aligned=%d primed=%d boot=%d pad=%zu available=%zu "
                            "need=%zu",
                            (int)srcIdx, (int)src.hasAlignedStart, (int)src.isPrimed, (int)src.bootstrapComplete,
                            padSamples, available / CHANNELS, totalFloats / CHANNELS);
                    }
                }

                for (size_t i = 0; i < totalFloats; i++) {
                    mixBuffer[i] += srcData[i];
                }
            }

            // If ALL sources for this track are silent (game pause), we MUST generate
            // silence. Otherwise, the Audio Stream timestamps stop advancing, and
            // av_interleaved_write_frame will BUFFER VIDEO PACKETS INDEFINITELY
            // waiting for audio to catch up. This causes the 32GB RAM leak.

            bool applyTransitionFade = false;

            if (activeSources == 0) {
                const bool wasSilent = trackWasSilent[track];
                trackWasSilent[track] = true;
                trackSilentSamples[track] += static_cast<uint64_t>(samplesToEncode);
                trackSilentChunks[track]++;
                const uint64_t nowTick = GetTickCount64();
                const uint64_t lastLogTick = trackLastSilenceLogTick[track];
                if (!wasSilent) {
                    trackSilenceTransitions[track]++;
                    DLL_Log(
                        "[PullAudio] Track %d entered silence: generated=%lld samples active=%d/%d "
                        "transitions=%llu target=%lldms encoded=%lld",
                        track, samplesToEncode, activeSources, eligibleSources,
                        static_cast<unsigned long long>(trackSilenceTransitions[track]), trackAudioTargetMs,
                        trackCursorSamples);
                    trackLastSilenceLogTick[track] = nowTick;
                } else if (nowTick - lastLogTick >= 1000) {
                    DLL_Log(
                        "[PullAudio] Track %d still silent: total=%llu samples (%.1fms) chunks=%llu active=%d/%d "
                        "target=%lldms encoded=%lld",
                        track, static_cast<unsigned long long>(trackSilentSamples[track]),
                        static_cast<double>(trackSilentSamples[track]) * 1000.0 / SAMPLE_RATE,
                        static_cast<unsigned long long>(trackSilentChunks[track]), activeSources, eligibleSources,
                        trackAudioTargetMs, trackCursorSamples);
                    trackLastSilenceLogTick[track] = nowTick;
                }
                if (silenceLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] Track %d silent - generating %lld samples of "
                        "silence to maintain sync",
                        track, samplesToEncode);
                }

                // Zero-fill the mix buffer (which is already zeroed by constructor, but
                // explicit is good)
                std::fill(mixBuffer.begin(), mixBuffer.end(), 0.0f);

                // We still 'processed' the mix (it's just silence)
                // Encode below...
            } else {
                auto it = trackWasSilent.find(track);
                if (it != trackWasSilent.end() && it->second) {
                    applyTransitionFade = true;
                    DLL_Log(
                        "[PullAudio] Track %d resumed after silence: silentTotal=%llu samples (%.1fms) chunks=%llu "
                        "active=%d/%d target=%lldms encoded=%lld",
                        track, static_cast<unsigned long long>(trackSilentSamples[track]),
                        static_cast<double>(trackSilentSamples[track]) * 1000.0 / SAMPLE_RATE,
                        static_cast<unsigned long long>(trackSilentChunks[track]), activeSources, eligibleSources,
                        trackAudioTargetMs, trackCursorSamples);
                    trackSilentSamples[track] = 0;
                    trackSilentChunks[track] = 0;
                }
                trackWasSilent[track] = false;
                if (activeSources < eligibleSources) {
                    const uint64_t nowTick = GetTickCount64();
                    const uint64_t lastLogTick = trackLastSilenceLogTick[track];
                    if (nowTick - lastLogTick >= 1000) {
                        DLL_Log(
                            "[PullAudio] Track %d partial/intermittent silence: active=%d/%d target=%lldms "
                            "encoded=%lld padTotal=%llu qpcGap=%llu qpcOverlap=%llu",
                            track, activeSources, eligibleSources, trackAudioTargetMs, trackCursorSamples,
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].underrunPadSamples),
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].packetTimelineGapSamples),
                            static_cast<unsigned long long>(audioSources[firstSrcIdx].packetTimelineOverlapSamples));
                        trackLastSilenceLogTick[track] = nowTick;
                    }
                }
                // Perform mixing for active sources
                // (Existing logic moved here or just fall through since mixBuffer is
                // already correct?) mixBuffer is already zeroed. We can just skip the
                // "if (activeSources == 0) continue" check and let the flow continue.
                // But we need to handle the "padding" logic inside the source loop.
                // The source loop summed into mixBuffer.
                // If activeSources==0, mixBuffer is [0,0,0...].
                // So we just proceed to encoding!
            }

            // Removed the 'continue' constraint.
            // Soft clipping: Clamp values to [-1, 1] to prevent distortion (only
            // needed if sources > 1)

            // Soft clipping: Clamp values to [-1, 1] to prevent distortion
            {
                int64_t trackPos = trackCursorSamples;
                const bool applyStartupTrackFade =
                    !applyTransitionFade && trackPos == 0 &&
                    audioSources[firstSrcIdx].packetBoundaryFadeInSamplesRemaining <= 0 &&
                    audioSources[firstSrcIdx].underrunFadeSamplesRemaining <= 0 &&
                    !audioSources[firstSrcIdx].pendingUnderrunRecoveryFade;
                const int64_t fadeSamples = applyTransitionFade ? SAMPLE_RATE / 20 : SAMPLE_RATE / 40;
                int64_t fadeStart = applyTransitionFade ? 0 : trackPos;
                if ((applyStartupTrackFade || applyTransitionFade) && fadeSamples > 0) {
                    for (int64_t s = 0; s < samplesToEncode; s++) {
                        int64_t global = fadeStart + s;
                        float gain = (global >= fadeSamples) ? 1.0f : (float)global / (float)fadeSamples;
                        size_t base = (size_t)s * CHANNELS;
                        for (int ch = 0; ch < CHANNELS; ++ch) {
                            mixBuffer[base + ch] *= gain;
                        }
                    }
                }
            }

            // Always use a smooth soft-knee limiter so any residual discontinuity from
            // packet stitching, drift correction, or track transitions cannot turn into
            // a hard clipped click at the encoder input. The limiter is transparent for
            // |sample| <= knee (bit-exact passthrough) and only shapes peaks above it, so
            // in-range audio is untouched (no global tanh waveshaper / no thinning).
            ce::audio::ApplySoftKneeLimiter(mixBuffer.data(), mixBuffer.size());

            // Encode the mixed samples using first source's encoder
            AudioEncoder* encoder = audioSources[firstSrcIdx].sharedEncoderPtr;
            if (encoder) {
                std::vector<uint8_t> encodeData(totalFloats * sizeof(float));
                memcpy(encodeData.data(), mixBuffer.data(), encodeData.size());

                // Calculate timestamp for this audio chunk
                // CRITICAL: Use RELATIVE time from sample count, not absolute QPC!
                // This must match what the encoder expects - time relative to recording
                // start
                int64_t audioChunkTimestampMs = (trackCursorSamples * 1000) / SAMPLE_RATE;

                const AudioEncoder::EncodeResult encodeResult = encoder->EncodeSamples(
                    encodeData.data(), (int)encodeData.size(), CHANNELS, SAMPLE_RATE, 32, 32, CHANNELS * 4,
                    true,  // float32
                    CHANNEL_MASK, audioChunkTimestampMs);
                if (encodeResult.failed || encodeResult.acceptedSamples != samplesToEncode) {
                    DLL_Log(
                        "[PullAudio] ERROR: Track %d encoder acceptance mismatch: requested=%lld accepted=%lld "
                        "submitted=%lld failed=%d cursor=%lld target=%lld",
                        track, static_cast<long long>(samplesToEncode),
                        static_cast<long long>(encodeResult.acceptedSamples),
                        static_cast<long long>(encodeResult.submittedSamples), encodeResult.failed ? 1 : 0,
                        static_cast<long long>(trackCursorSamples), static_cast<long long>(targetSamples));
                }
                samplesToEncode =
                    encodeResult.failed ? 0 : std::min<int64_t>(samplesToEncode, encodeResult.acceptedSamples);

                if (srcIndices.size() > 1 && mixLogCounter++ % 5000 == 0) {
                    DLL_Log("[PullAudio] Mixed %d sources for track %d (%lld samples)", activeSources, track,
                            samplesToEncode);
                }
            }

            if (activeSources == 0) {
                trackFullSilenceSamples[track] += static_cast<uint64_t>(samplesToEncode);
            } else {
                trackRealMixedSamples[track] += static_cast<uint64_t>(samplesToEncode);
                if (activeSources < eligibleSources) {
                    trackPartialSilenceSamples[track] += static_cast<uint64_t>(samplesToEncode);
                }
            }
            trackCursorSamples += samplesToEncode;

            // Keep source counters aligned for source-local diagnostics, but the
            // exported stream timeline is trackTimelineSamples[track].
            for (size_t srcIdx : srcIndices) {
                encodedSamplesPerSource[srcIdx] += samplesToEncode;
            }

            // A/V SYNC MONITORING: Periodic check for drift detection and audio health.
            // Track counters per audio track so multi-track sessions surface every
            // track's health instead of only whichever track hits the shared modulo.
            int& trackSyncCheckCounter = trackSyncCheckCounters[track];
            if (trackSyncCheckCounter++ % 1200 == 0 && firstSrcIdx < encodedSamplesPerSource.size()) {
                int64_t wallVideoMs = videoElapsedMs.load();
                int64_t videoMs = wallVideoMs;
                int64_t encodedVideoMs = wallVideoMs;
                int64_t encodedVideoUsForSummary = wallVideoMs * 1000;
                int64_t scheduledVideoUsForSummary = trackAudioTargetUs;
                if (videoEnc) {
                    int64_t encodedVideoUs = videoEnc->GetEncodedDurationUs();
                    if (encodedVideoUs > 0) {
                        encodedVideoUsForSummary = encodedVideoUs;
                        encodedVideoMs = encodedVideoUs / 1000;
                        if (!isCfrRecording) {
                            videoMs = encodedVideoMs;
                        }
                    }
                    if (isCfrRecording) {
                        const int64_t expectedVideoUs = videoEnc->GetExpectedFinalDurationUs();
                        if (expectedVideoUs > 0) {
                            scheduledVideoUsForSummary = expectedVideoUs;
                        }
                    }
                }
                int64_t audioSamples = trackTimelineSamples[track];
                int64_t audioUs = ce::audio::ComputeSamplesToDurationUs(audioSamples, SAMPLE_RATE);
                int64_t audioMs = audioUs / 1000;
                int64_t avDrift = audioMs - videoMs;
                int64_t latencyAdjustedAvDrift =
                    ce::audio::ComputeLatencyAdjustedAvDriftMs(avDrift, trackAudioPullLatencyMs);
                int64_t pipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
                const int64_t residualSamples = audioSamples - targetSamples;
                const int64_t residualUs = ce::audio::ComputeSamplesToDurationUs(residualSamples, SAMPLE_RATE);
                const int64_t audioVsEncodedUs = audioUs - encodedVideoUsForSummary;
                const int64_t audioVsTargetUs = audioUs - trackAudioTargetUs;
                const int64_t audioVsScheduledUs = audioUs - scheduledVideoUsForSummary;

                // Summarize all sources contributing to this track so issues on a
                // secondary app/system source are visible in the periodic sync log.
                uint64_t overflowDropped = 0;
                uint64_t retainedTrimmed = 0;
                uint64_t latencyTrimmed = 0;
                uint64_t tier2Trimmed = 0;
                uint64_t bootstrapTrimmed = 0;
                uint64_t postTrimmed = 0;
                uint64_t underrunPadded = 0;
                uint64_t coverageLossTrimmed = 0;
                uint64_t packetGapAdjusted = 0;
                uint64_t packetOverlapTrimmed = 0;
                std::string sourceSummary;
                for (size_t srcIdx : srcIndices) {
                    size_t rbLevel = 0;
                    int64_t syncOutput = 0;
                    int64_t alignedStartMs = -1;
                    uint64_t srcOverflowDropped = 0;
                    uint64_t srcRetainedTrimmed = 0;
                    uint64_t srcLatencyTrimmed = 0;
                    uint64_t srcTier2Trimmed = 0;
                    uint64_t srcBootstrapTrimmed = 0;
                    uint64_t srcPostTrimmed = 0;
                    uint64_t srcUnderrunPadded = 0;
                    uint64_t srcCoverageLossTrimmed = 0;
                    uint64_t srcSyntheticRing = 0;
                    uint64_t srcSyntheticInflight = 0;
                    uint64_t srcSyntheticPost = 0;
                    uint64_t srcPacketGapAdjusted = 0;
                    uint64_t srcPacketOverlapTrimmed = 0;
                    bool srcPrimed = false;
                    bool srcBootstrapped = false;
                    bool srcTimelineValid = false;
                    if (srcIdx < audioSources.size()) {
                        auto& src = audioSources[srcIdx];
                        if (src.ringBuffer) {
                            rbLevel = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                        syncOutput = src.syncSamplesOutput;
                        alignedStartMs = src.alignedStartMs;
                        srcOverflowDropped = src.overflowDropSamples;
                        srcRetainedTrimmed = src.retainedNewestTrimSamples;
                        srcLatencyTrimmed = src.latencyTrimSamples;
                        srcTier2Trimmed = src.tier2TrimSamples;
                        srcBootstrapTrimmed = src.bootstrapTrimSamples;
                        srcPostTrimmed = src.postResampleTrimSamples;
                        srcUnderrunPadded = src.underrunPadSamples;
                        srcCoverageLossTrimmed = src.coverageLossTrimSamples;
                        srcSyntheticRing = src.startupSyntheticRingSamples;
                        srcSyntheticInflight = src.startupSyntheticResamplerSamples;
                        srcSyntheticPost = src.startupSyntheticPostSamples;
                        srcPacketGapAdjusted = src.packetTimelineGapSamples;
                        srcPacketOverlapTrimmed = src.packetTimelineOverlapSamples;
                        srcPrimed = src.isPrimed;
                        srcBootstrapped = src.bootstrapComplete;
                        srcTimelineValid = src.timelineValid;
                    }

                    overflowDropped += srcOverflowDropped;
                    retainedTrimmed += srcRetainedTrimmed;
                    latencyTrimmed += srcLatencyTrimmed;
                    tier2Trimmed += srcTier2Trimmed;
                    bootstrapTrimmed += srcBootstrapTrimmed;
                    postTrimmed += srcPostTrimmed;
                    underrunPadded += srcUnderrunPadded;
                    coverageLossTrimmed += srcCoverageLossTrimmed;
                    packetGapAdjusted += srcPacketGapAdjusted;
                    packetOverlapTrimmed += srcPacketOverlapTrimmed;

                    char sourceState[448];
                    std::snprintf(
                        sourceState, sizeof(sourceState),
                        "src%zu(rb=%zu sync=%lld start=%lld tl=%d primed=%d boot=%d synth=%llu/%llu/%llu ovf=%llu "
                        "rtrim=%llu lat=%llu t2=%llu cov=%llu boottrim=%llu post=%llu pad=%llu qgap=%llu qov=%llu)",
                        srcIdx, rbLevel, syncOutput, alignedStartMs, (int)srcTimelineValid, (int)srcPrimed,
                        (int)srcBootstrapped, (unsigned long long)srcSyntheticRing,
                        (unsigned long long)srcSyntheticInflight, (unsigned long long)srcSyntheticPost,
                        (unsigned long long)srcOverflowDropped, (unsigned long long)srcRetainedTrimmed,
                        (unsigned long long)srcLatencyTrimmed, (unsigned long long)srcTier2Trimmed,
                        (unsigned long long)srcCoverageLossTrimmed, (unsigned long long)srcBootstrapTrimmed,
                        (unsigned long long)srcPostTrimmed, (unsigned long long)srcUnderrunPadded,
                        (unsigned long long)srcPacketGapAdjusted, (unsigned long long)srcPacketOverlapTrimmed);
                    if (!sourceSummary.empty()) {
                        sourceSummary += "; ";
                    }
                    sourceSummary += sourceState;
                }
                if (sourceSummary.empty()) {
                    sourceSummary = "none";
                }
                const uint64_t categorizedLatencyTrim =
                    std::min(latencyTrimmed, bootstrapTrimmed + retainedTrimmed + coverageLossTrimmed + tier2Trimmed);
                const uint64_t uncategorizedLatencyTrim = latencyTrimmed - categorizedLatencyTrim;

                for (size_t srcIdx : srcIndices) {
                    auto& src = audioSources[srcIdx];
                    if (src.pendingRetainedTrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] Retained-audio trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingRetainedTrimEvents,
                            static_cast<unsigned long long>(src.pendingRetainedTrimSamples),
                            static_cast<unsigned long long>(src.retainedNewestTrimSamples), targetBufferedSamples,
                            pipelineLagMs);
                        src.pendingRetainedTrimEvents = 0;
                        src.pendingRetainedTrimSamples = 0;
                    }
                    if (src.pendingCoverageLossTrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] WGC coverage-loss trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "target=%lld pipelineLag=%lldms contentLag=%lldms delivered=%u/%u fps",
                            srcIdx, src.pendingCoverageLossTrimEvents,
                            static_cast<unsigned long long>(src.pendingCoverageLossTrimSamples),
                            static_cast<unsigned long long>(src.coverageLossTrimSamples), targetBufferedSamples,
                            pipelineLagMs, wgcBufferedVideoContentLagMs, wgcDeliveredFps, wgcTargetFps);
                        src.pendingCoverageLossTrimEvents = 0;
                        src.pendingCoverageLossTrimSamples = 0;
                    }
                    if (src.pendingLatencyTrimEvents > 0) {
                        const double trimRatePerMinute =
                            ce::audio::ComputeSamplesPerMinute(src.pendingLatencyTrimSamples, 10000000ll) * 6.0;
                        const uint64_t categorizedLatencyTrim =
                            std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                                 src.coverageLossTrimSamples + src.tier2TrimSamples +
                                                                 src.catastrophicResyncSamples);
                        const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
                        DLL_Log(
                            "[PullAudio] Latency trim aggregate summary - src=%zu events=%u samples=%llu "
                            "total=%llu bootstrap=%llu retained=%llu coverage=%llu tier2=%llu cat=%llu "
                            "uncategorizedLive=%llu rate=%.1f/min target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingLatencyTrimEvents,
                            static_cast<unsigned long long>(src.pendingLatencyTrimSamples),
                            static_cast<unsigned long long>(src.latencyTrimSamples),
                            static_cast<unsigned long long>(src.bootstrapTrimSamples),
                            static_cast<unsigned long long>(src.retainedNewestTrimSamples),
                            static_cast<unsigned long long>(src.coverageLossTrimSamples),
                            static_cast<unsigned long long>(src.tier2TrimSamples),
                            static_cast<unsigned long long>(src.catastrophicResyncSamples),
                            static_cast<unsigned long long>(uncategorizedLatencyTrim), trimRatePerMinute,
                            targetBufferedSamples, pipelineLagMs);
                        src.pendingLatencyTrimEvents = 0;
                        src.pendingLatencyTrimSamples = 0;
                    }
                    if (src.pendingTier2TrimEvents > 0) {
                        DLL_Log(
                            "[PullAudio] Tier2 drift trim summary - src=%zu events=%u samples=%llu total=%llu "
                            "overallLatencyTrim=%llu target=%lld pipelineLag=%lldms",
                            srcIdx, src.pendingTier2TrimEvents,
                            static_cast<unsigned long long>(src.pendingTier2TrimSamples),
                            static_cast<unsigned long long>(src.tier2TrimSamples),
                            static_cast<unsigned long long>(src.latencyTrimSamples), targetBufferedSamples,
                            pipelineLagMs);
                        src.pendingTier2TrimEvents = 0;
                        src.pendingTier2TrimSamples = 0;
                    }
                }

                DLL_Log(
                    "[A/V SYNC CHECK] Track %d: Video=%lld ms, Audio=%lld ms, "
                    "Drift=%lld ms, DriftAdj=%lld ms, Pull=%lld ms, VideoWall=%lld ms, VideoEnc=%lld ms, "
                    "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld audio_vs_scheduled_us=%+lld "
                    "residual_samples=%+lld residual_us=%+lld target_samples=%lld cursor_samples=%lld "
                    "target_us=%lld cursor_us=%lld encoded_video_us=%lld scheduled_video_us=%lld "
                    "PipelineLag=%lld ms, "
                    "ContentLag=%lld ms, CovMode=%d, EncBottleneck=%d, Delivered=%u/%u, Over=0x%x, "
                    "TargetBuf=%lld ms, WgcFrameLead=%lld ms, WgcFrameLag=%lld ms, WgcSelBias=%lld us, Overflow=%llu, "
                    "RetainTrim=%llu, CoverageTrim=%llu, Tier2Trim=%llu, BootstrapTrim=%llu, "
                    "LatencyTrimTotal=%llu, UncategorizedLiveTrim=%llu, PostTrim=%llu, Pad=%llu, QpcGap=%llu, "
                    "QpcOverlap=%llu, Sources=%s",
                    track, videoMs, audioMs, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs, wallVideoMs,
                    encodedVideoMs, audioVsEncodedUs, audioVsTargetUs, audioVsScheduledUs, residualSamples, residualUs,
                    targetSamples, audioSamples, trackAudioTargetUs, audioUs, encodedVideoUsForSummary,
                    scheduledVideoUsForSummary, pipelineLagMs,
                    isWgcCfrRecording ? wgcBufferedVideoContentLagMs : pipelineLagMs, wgcCoverageLossActive ? 1 : 0,
                    wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps, wgcOverloadFlags,
                    (targetBufferedSamples * 1000) / SAMPLE_RATE, wgcSelectedContentLeadMs, wgcVisualContentLagMs,
                    wgcSelectionBiasUs, (unsigned long long)overflowDropped, (unsigned long long)retainedTrimmed,
                    (unsigned long long)coverageLossTrimmed, (unsigned long long)tier2Trimmed,
                    (unsigned long long)bootstrapTrimmed, (unsigned long long)latencyTrimmed,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)postTrimmed,
                    (unsigned long long)underrunPadded, (unsigned long long)packetGapAdjusted,
                    (unsigned long long)packetOverlapTrimmed, sourceSummary.c_str());

                DLL_Log(
                    "[A/V SYNC SUMMARY] Track %d: Wall=%lldms EncV=%lldms Audio=%lldms Drift=%+lldms "
                    "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld audio_vs_scheduled_us=%+lld "
                    "residual_samples=%+lld residual_us=%+lld target_samples=%lld cursor_samples=%lld "
                    "PipelineLag=%lldms PullLatency=%lldms WgcFrameLead=%lldms WgcFrameLag=%lldms EncBot=%d "
                    "Delivered=%u/%u",
                    track, wallVideoMs, encodedVideoMs, audioMs, avDrift, audioVsEncodedUs, audioVsTargetUs,
                    audioVsScheduledUs, residualSamples, residualUs, targetSamples, audioSamples, pipelineLagMs,
                    trackAudioPullLatencyMs, wgcSelectedContentLeadMs, wgcVisualContentLagMs,
                    wgcEncoderBottlenecked ? 1 : 0, wgcDeliveredFps, wgcTargetFps);

                // A deepened ingestion reservoir moves the pull TARGET back, so the already
                // exported cursor legitimately leads it by up to the accumulated extra
                // lookahead until wall time catches up. That lead is scheduling state, not
                // drift: sample positions never moved. A cursor BEHIND the target is still a
                // strict fault, and so is any lead the reservoir cannot explain.
                const int64_t reservoirLeadAllowanceSamples =
                    (std::max<int64_t>(0, audioIngestReservoirExtraMs) * SAMPLE_RATE) / 1000;
                if (isCfrRecording && trackStartupSettled &&
                    (residualSamples < 0 || residualSamples > reservoirLeadAllowanceSamples)) {
                    DLL_Log(
                        "[A/V ZERO DRIFT WARNING] Track %d residual_samples=%+lld residual_us=%+lld "
                        "target_samples=%lld cursor_samples=%lld target_us=%lld cursor_us=%lld "
                        "audio_vs_encoded_us=%+lld audio_vs_target_us=%+lld reservoirExtraMs=%lld",
                        track, residualSamples, residualUs, targetSamples, audioSamples, trackAudioTargetUs, audioUs,
                        audioVsEncodedUs, audioVsTargetUs, audioIngestReservoirExtraMs);
                }

                if (std::abs(latencyAdjustedAvDrift) > 100) {
                    DLL_Log(
                        "[A/V SYNC WARNING] Track %d adjusted drift exceeds 100ms! raw=%lldms adjusted=%lldms "
                        "pull=%lldms",
                        track, avDrift, latencyAdjustedAvDrift, trackAudioPullLatencyMs);
                }
            }
        }
        // A pull may have consumed the last post-resampler samples of an old epoch.
        // Acknowledge only after that content has actually entered the common track cursor.
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }
        if (sharedMemLayout) {
            sharedMemLayout->runtimeState.wgcAudioLeadExcessSamples.store(
                isWgcCfrRecording ? maxWgcAudioLeadExcessSamples : 0u, std::memory_order_relaxed);
        }
    }

    // Create shared D3D11 textures for Vulkan games to import
    bool CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format, SharedMemoryLayout* sharedMem) {
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

    void WritePacket(AVPacket* pkt) {
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

    void ReloadConfig(const AppConfig* newConfig) {
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
