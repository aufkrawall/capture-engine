#include "mediaengine_internal.h"

bool MediaEngine::PullTrackSyncMonitoring(AudioPullState& s, int track, const std::vector<size_t>& srcIndices) {
                auto& isCfrRecording = s.isCfrRecording;
                auto& isWgcCfrRecording = s.isWgcCfrRecording;
                auto& wallVideoMs = s.wallVideoMs;
                auto& encodedVideoMs = s.encodedVideoMs;
                auto& wgcTargetFps = s.wgcTargetFps;
                auto& wgcDeliveredFps = s.wgcDeliveredFps;
                auto& wgcBufferedVideoContentLagMs = s.wgcBufferedVideoContentLagMs;
                auto& wgcCoverageLossActive = s.wgcCoverageLossActive;
                auto& wgcOverloadFlags = s.wgcOverloadFlags;
                auto& wgcEncoderBottlenecked = s.wgcEncoderBottlenecked;
                auto& wgcSelectionBiasUs = s.wgcSelectionBiasUs;
                auto& wgcSelectedContentLeadMs = s.wgcSelectedContentLeadMs;
                auto& wgcVisualContentLagMs = s.wgcVisualContentLagMs;
                auto& encodedVideoUs = s.encodedVideoUs;
                auto& CHANNELS = s.CHANNELS;
                auto& CHANNEL_MASK = s.CHANNEL_MASK;
                auto& trackStartupSettled = s.trackStartupSettled;
                auto& trackAudioPullLatencyMs = s.trackAudioPullLatencyMs;
                auto& trackAudioTargetUs = s.trackAudioTargetUs;
                auto& trackAudioTargetMs = s.trackAudioTargetMs;
                auto& targetSamples = s.targetSamples;
                auto& targetBufferedSamples = s.targetBufferedSamples;
                auto& firstSrcIdx = s.firstSrcIdx;
                auto& samplesToEncode = s.samplesToEncode;
                auto& totalFloats = s.totalFloats;
                auto& mixBuffer = s.mixBuffer;
                auto& activeSources = s.activeSources;
                auto& eligibleSources = s.eligibleSources;
                auto& applyTransitionFade = s.applyTransitionFade;
                auto& bootstrapTrimmed = s.bootstrapTrimmed;
                auto& wasSilent = s.wasSilent;
                auto& nowTick = s.nowTick;
                auto& lastLogTick = s.lastLogTick;
                auto& it = s.it;
                auto& trackPos = s.trackPos;
                auto& applyStartupTrackFade = s.applyStartupTrackFade;
                auto& fadeSamples = s.fadeSamples;
                auto& fadeStart = s.fadeStart;
                auto& encodeData = s.encodeData;
                auto& audioChunkTimestampMs = s.audioChunkTimestampMs;
                auto& videoMs = s.videoMs;
                auto& encodedVideoUsForSummary = s.encodedVideoUsForSummary;
                auto& scheduledVideoUsForSummary = s.scheduledVideoUsForSummary;
                auto& audioSamples = s.audioSamples;
                auto& audioUs = s.audioUs;
                auto& audioMs = s.audioMs;
                auto& avDrift = s.avDrift;
                auto& latencyAdjustedAvDrift = s.latencyAdjustedAvDrift;
                auto& pipelineLagMs = s.pipelineLagMs;
                auto& residualSamples = s.residualSamples;
                auto& residualUs = s.residualUs;
                auto& audioVsEncodedUs = s.audioVsEncodedUs;
                auto& audioVsTargetUs = s.audioVsTargetUs;
                auto& audioVsScheduledUs = s.audioVsScheduledUs;
                auto& overflowDropped = s.overflowDropped;
                auto& retainedTrimmed = s.retainedTrimmed;
                auto& latencyTrimmed = s.latencyTrimmed;
                auto& tier2Trimmed = s.tier2Trimmed;
                auto& postTrimmed = s.postTrimmed;
                auto& underrunPadded = s.underrunPadded;
                auto& coverageLossTrimmed = s.coverageLossTrimmed;
                auto& packetGapAdjusted = s.packetGapAdjusted;
                auto& packetOverlapTrimmed = s.packetOverlapTrimmed;
                auto& categorizedLatencyTrim = s.categorizedLatencyTrim;
                auto& uncategorizedLatencyTrim = s.uncategorizedLatencyTrim;
                auto& reservoirLeadAllowanceSamples = s.reservoirLeadAllowanceSamples;
                auto& encodeResult = s.encodeResult;
                constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
        auto& trackCursorSamples = trackTimelineSamples[track];

            // If ALL sources for this track are silent (game pause), we MUST generate
            // silence. Otherwise, the Audio Stream timestamps stop advancing, and
            // av_interleaved_write_frame will BUFFER VIDEO PACKETS INDEFINITELY
            // waiting for audio to catch up. This causes the 32GB RAM leak.

            applyTransitionFade = false;

            if (activeSources == 0) {
                wasSilent = trackWasSilent[track];
                trackWasSilent[track] = true;
                trackSilentSamples[track] += static_cast<uint64_t>(samplesToEncode);
                trackSilentChunks[track]++;
                nowTick = GetTickCount64();
                lastLogTick = trackLastSilenceLogTick[track];
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
                it = trackWasSilent.find(track);
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
                trackPos = trackCursorSamples;
                applyStartupTrackFade =
                    !applyTransitionFade && trackPos == 0 &&
                    audioSources[firstSrcIdx].packetBoundaryFadeInSamplesRemaining <= 0 &&
                    audioSources[firstSrcIdx].underrunFadeSamplesRemaining <= 0 &&
                    !audioSources[firstSrcIdx].pendingUnderrunRecoveryFade;
                fadeSamples = applyTransitionFade ? SAMPLE_RATE / 20 : SAMPLE_RATE / 40;
                fadeStart = applyTransitionFade ? 0 : trackPos;
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
                encodeData =std::vector<uint8_t> (totalFloats * sizeof(float));
                memcpy(encodeData.data(), mixBuffer.data(), encodeData.size());

                // Calculate timestamp for this audio chunk
                // CRITICAL: Use RELATIVE time from sample count, not absolute QPC!
                // This must match what the encoder expects - time relative to recording
                // start
                audioChunkTimestampMs = (trackCursorSamples * 1000) / SAMPLE_RATE;

                encodeResult = encoder->EncodeSamples(
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
                wallVideoMs = videoElapsedMs.load();
                videoMs = wallVideoMs;
                encodedVideoMs = wallVideoMs;
                encodedVideoUsForSummary = wallVideoMs * 1000;
                scheduledVideoUsForSummary = trackAudioTargetUs;
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
                audioSamples = trackTimelineSamples[track];
                audioUs = ce::audio::ComputeSamplesToDurationUs(audioSamples, SAMPLE_RATE);
                audioMs = audioUs / 1000;
                avDrift = audioMs - videoMs;
                latencyAdjustedAvDrift =
                    ce::audio::ComputeLatencyAdjustedAvDriftMs(avDrift, trackAudioPullLatencyMs);
                pipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
                residualSamples = audioSamples - targetSamples;
                residualUs = ce::audio::ComputeSamplesToDurationUs(residualSamples, SAMPLE_RATE);
                audioVsEncodedUs = audioUs - encodedVideoUsForSummary;
                audioVsTargetUs = audioUs - trackAudioTargetUs;
                audioVsScheduledUs = audioUs - scheduledVideoUsForSummary;

                // Summarize all sources contributing to this track so issues on a
                // secondary app/system source are visible in the periodic sync log.
                overflowDropped = 0;
                retainedTrimmed = 0;
                latencyTrimmed = 0;
                tier2Trimmed = 0;
                bootstrapTrimmed = 0;
                postTrimmed = 0;
                underrunPadded = 0;
                coverageLossTrimmed = 0;
                packetGapAdjusted = 0;
                packetOverlapTrimmed = 0;
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
                categorizedLatencyTrim =
                    std::min(latencyTrimmed, bootstrapTrimmed + retainedTrimmed + coverageLossTrimmed + tier2Trimmed);
                uncategorizedLatencyTrim = latencyTrimmed - categorizedLatencyTrim;

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
                reservoirLeadAllowanceSamples =
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
    return true;
}
