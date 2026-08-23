#include "mediaengine_internal.h"


bool MediaEngine::StopRecording(bool cancelUncommittedVideo) {


        // Audio-only: stop audio thread, write trailer, clean up
        if (audioOnly) {
            {
                std::lock_guard<std::recursive_mutex> lock(muxMutex);
                if (!recording)
                    return false;
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
                                               // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
                                                   // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
            bool audioOutputPublished = false;
            if (audioOnlyFmtCtx) {
                audioOnlyTrailerSucceeded = av_write_trailer(audioOnlyFmtCtx) >= 0;
                if (!audioOnlyTrailerSucceeded) {
                    DLL_Log("[StopAudio] ERROR: Audio-only trailer write failed");
                }
                DLL_Log("[StopAudio] Audio-only recording finalized: %s",
                        ce::privacy::CollapsePathForLog(audioOnlyFilename).c_str());
                audioOutputPublished = CleanupAudioOnlyMuxer();
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
            return audioOutputPublished;
        }

        if (cancelUncommittedVideo && !firstVideoFrameCommitted) {
            CancelUncommittedVideoRecording();
            return false;

        }
        if (cancelUncommittedVideo) {
            DLL_Log("[RecordingLifecycle] Cancellation arrived after the first video frame committed; finalizing");
        }

        ExtendCfrToCommonAudioLattice();

        int64_t endUs = 0;
        constexpr int kStopSampleRate = 48000;
        const int64_t expectedVideoUsForStop = videoEnc ? videoEnc->GetExpectedFinalDurationUs() : 0;
        const bool finalCfrStopPending = ce::audio::ShouldDrainStoppedCaptureQueuesBeforeFinalAudioPull(
                                             audioRunning.load(), audioOnly, expectedVideoUsForStop) &&
                                         IsCfrRecording();
        if (finalCfrStopPending) {
            audioFinalizingCfrStop.store(true, std::memory_order_release);
            DLL_Log("[StopAudio] Final CFR stop pending: targetUs=%lld", expectedVideoUsForStop);
        }
        WaitForFinalCfrAudioSourceCatchup(expectedVideoUsForStop);
        DrainStoppedCaptureQueuesBeforeFinalPull(expectedVideoUsForStop);
        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            if (!recording) {
                audioFinalizingCfrStop.store(false, std::memory_order_release);
                return false;
            }

            // Set recording end timestamp on all audio encoders BEFORE stopping
            // the audio thread so the final drain and flush both target the exact
            // encoded video length.
            if (videoEnc) {
                int64_t expectedDurationUs = videoEnc->GetExpectedFinalDurationUs();
                int64_t encodedDurationUs = videoEnc->GetEncodedDurationUs();
                int64_t durationUs = expectedDurationUs > 0 ? expectedDurationUs : encodedDurationUs;
                if (IsCfrRecording()) {
                    const int64_t wallDurationUs = std::max<int64_t>(
                        IsWgcCfrRecording() ? d3d11TimelineState.lastElapsedUs : injectTimelineState.lastElapsedUs,
                        videoElapsedMs.load() * 1000);

                    DLL_Log(
                        "MediaEngine: CFR stop durations. Expected: %lld us, Encoded: %lld us, Wall: %lld us, "
                        "Selected: %lld us",
                        expectedDurationUs, encodedDurationUs, wallDurationUs, durationUs);
                }
                endUs = durationUs;

                DLL_Log(
                    "MediaEngine: Setting audio end time. Start: %lld us, Dur: %lld "
                    "us, End: %lld us",
                    0LL, durationUs, endUs);

                for (auto& src : audioSources) {
                    if (src.sharedEncoderPtr) {
                        src.sharedEncoderPtr->SetRecordingEndUs(endUs);
                    }
                }

                if (endUs > 0) {
                    // CFR: use PTS-based target so audio matches video exactly.
                    // The encoder thread's drain already covers most audio; this final
                    // pull fills any remaining gap without racing (drain is done by now).
                    // PullAndEncodeAudio intentionally limits very large gaps to bounded
                    // chunks, so force-drain loops until every exported track reaches the
                    // selected video endpoint or forward progress genuinely stops.
                    const int64_t cfrAudioTargetUs = IsCfrRecording() ? videoEnc->GetExpectedFinalDurationUs() : endUs;
                    const int64_t cfrAudioTargetSamples =
                        ce::audio::ComputeDurationUsToSamples(cfrAudioTargetUs, kStopSampleRate);
                    auto minTrackCursorSamples = [this]() -> int64_t {
                        if (cachedTrackToSources.empty()) {
                            return 0;
                        }
                        int64_t minSamples = std::numeric_limits<int64_t>::max();
                        for (const auto& kv : cachedTrackToSources) {
                            const auto trackIt = trackTimelineSamples.find(kv.first);
                            const int64_t trackSamples = trackIt != trackTimelineSamples.end() ? trackIt->second : 0;
                            minSamples = std::min(minSamples, trackSamples);
                        }
                        return minSamples == std::numeric_limits<int64_t>::max() ? 0 : minSamples;
                    };
                    const int64_t minEncodedBefore = minTrackCursorSamples();
                    int64_t minEncodedAfter = minEncodedBefore;
                    const int64_t missingBefore = std::max<int64_t>(0, cfrAudioTargetSamples - minEncodedBefore);
                    const int64_t maxForceDrainIterations =
                        std::max<int64_t>(1, (missingBefore + (kStopSampleRate / 2) - 1) / (kStopSampleRate / 2) + 4);
                    int64_t forceDrainIterations = 0;
                    for (int64_t attempt = 0; attempt < maxForceDrainIterations; ++attempt) {
                        const int64_t beforeIteration = minTrackCursorSamples();
                        if (beforeIteration >= cfrAudioTargetSamples) {
                            minEncodedAfter = beforeIteration;
                            break;
                        }
                        PullAndEncodeAudio(cfrAudioTargetUs, true);
                        ++forceDrainIterations;
                        const int64_t afterIteration = minTrackCursorSamples();
                        minEncodedAfter = afterIteration;
                        if (afterIteration >= cfrAudioTargetSamples) {
                            break;
                        }
                        if (afterIteration <= beforeIteration) {
                            DLL_Log(
                                "[StopAudio] WARNING: forceDrain made no progress: targetUs=%lld targetSamples=%lld "
                                "minTrack=%lld iteration=%lld/%lld",
                                cfrAudioTargetUs, cfrAudioTargetSamples, afterIteration, forceDrainIterations,
                                maxForceDrainIterations);
                            break;
                        }
                    }
                    if (minEncodedAfter < cfrAudioTargetSamples) {
                        DLL_Log(
                            "[StopAudio] WARNING: forceDrain incomplete: targetUs=%lld targetSamples=%lld "
                            "minTrackAfter=%lld missing=%lld iterations=%lld/%lld",
                            cfrAudioTargetUs, cfrAudioTargetSamples, minEncodedAfter,
                            cfrAudioTargetSamples - minEncodedAfter, forceDrainIterations, maxForceDrainIterations);
                    }
                    DLL_Log(
                        "[StopAudio] forceDrain: targetUs=%lld minTrackBefore=%lld minTrackAfter=%lld "
                        "pulled=%lld samples (%.1f ms) iterations=%lld",
                        cfrAudioTargetUs, minEncodedBefore, minEncodedAfter, minEncodedAfter - minEncodedBefore,
                        (double)(minEncodedAfter - minEncodedBefore) / 48.0, forceDrainIterations);
                }
            }

            recording = false;
        }

        // Stop audio thread first
        audioRunning = false;
        audioDrainCv.notify_all();
        if (audioThread.joinable()) {
            audioThread.join();
        }
        if (finalCfrStopPending) {
            audioFinalizingCfrStop.store(false, std::memory_order_release);
            DLL_Log("[StopAudio] Final CFR stop pending cleared");
        }

        // Final A/V sync diagnostic before stopping sources
        {
            const int64_t expectedVideoMs = expectedVideoUsForStop / 1000;
            const int64_t expectedVideoSamples =
                ce::audio::ComputeDurationUsToSamples(expectedVideoUsForStop, kStopSampleRate);
            for (size_t i = 0; i < audioSources.size() && i < encodedSamplesPerSource.size(); i++) {
                const int64_t encSamples = encodedSamplesPerSource[i];
                const int64_t diffSamples = encSamples - expectedVideoSamples;
                const double diffMs = (double)diffSamples * 1000.0 / kStopSampleRate;
                DLL_Log(
                    "[StopAudio] Source %zu: encodedSamples=%lld expectedVideoSamples=%lld diff=%+lld (%+.1f ms) "
                    "track=%d",
                    i, encSamples, expectedVideoSamples, diffSamples, diffMs, audioSources[i].track);
            }
            // App-audio latency distribution over the whole recording (audio-behind-video delay). This
            // is the headline observability fix: it makes elevated/variable latency obvious at a glance
            // instead of needing manual reconstruction. A healthy source sits almost entirely in <50ms;
            // significant time in 300-600ms means the drain is not keeping latency near live.
            for (size_t i = 0; i < audioSources.size(); i++) {
                const AudioSource& s = audioSources[i];
                if (s.sourceType != AudioConfig::AppAudio ||
                    (s.appLatencySampleCount == 0 && s.appLatencyStopDrainSampleCount == 0)) {
                    continue;
                }
                const uint64_t n = s.appLatencySampleCount;
                const double avgMs = n > 0 ? static_cast<double>(s.appLatencySumMs) / static_cast<double>(n) : 0.0;
                const double targetAvgMs =
                    n > 0 ? static_cast<double>(s.appLatencyTargetSumMs) / static_cast<double>(n) : 0.0;
                const double excessAvgMs =
                    n > 0 ? static_cast<double>(s.appLatencyExcessSumMs) / static_cast<double>(n) : 0.0;
                const double stopDrainAvgMs =
                    s.appLatencyStopDrainSampleCount > 0
                        ? static_cast<double>(s.appLatencyStopDrainSumMs) /
                              static_cast<double>(s.appLatencyStopDrainSampleCount)
                        : 0.0;
                const double maxCompPercent = static_cast<double>(s.appLatencyMaxAbsCompDelta) * 100.0 /
                                              (static_cast<double>(kStopSampleRate) * 10.0);
                const double elevatedPct =
                    n > 0
                        ? 100.0 * static_cast<double>(s.appLatencyBuckets[2] + s.appLatencyBuckets[3] +
                                                     s.appLatencyBuckets[4]) /
                              static_cast<double>(n)
                        : 0.0;
                ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(i);
                const uint64_t queueOverrunPackets = routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                const uint64_t queueOverrunFrames = routedCapture ? routedCapture->GetQueueOverrunFrameCount() : 0;
                DLL_Log(
                    "[STOP AUDIO LATENCY] Source %zu track=%d appAudioDelay avg=%.0fms max=%ums "
                    "targetAvg=%.0fms excessAvg=%.0fms excessMax=%ums "
                    "buckets(<50/50-150/150-300/300-600/>600ms)=%.0f%%/%.0f%%/%.0f%%/%.0f%%/%.0f%% "
                    ">=150ms=%.0f%% drainObservations=%u/%llu transitions=%llu maxComp=%.4f%% "
                    "liveObservations=%llu stopDrainObservations=%llu stopDrainAvg=%.0fms stopDrainMax=%ums "
                    "queueOverrun=%llu/%llu underruns=%u trims(lat=%llu normal=%llu cat=%u/%llu). "
                    "Lower/more-uniform excess is better; high excess means audio content ran behind video.",
                    i, s.track, avgMs, s.appLatencyMaxMs, targetAvgMs, excessAvgMs, s.appLatencyExcessMaxMs,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    n > 0 ? 100.0 * s.appLatencyBuckets[0] / n : 0.0,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    n > 0 ? 100.0 * s.appLatencyBuckets[1] / n : 0.0,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    n > 0 ? 100.0 * s.appLatencyBuckets[2] / n : 0.0,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    n > 0 ? 100.0 * s.appLatencyBuckets[3] / n : 0.0,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    n > 0 ? 100.0 * s.appLatencyBuckets[4] / n : 0.0, elevatedPct, s.appLatencyDrainingSamples,
                    static_cast<unsigned long long>(n), static_cast<unsigned long long>(s.appLatencyDrainTransitions),
                    maxCompPercent, static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(s.appLatencyStopDrainSampleCount), stopDrainAvgMs,
                    s.appLatencyStopDrainMaxMs, static_cast<unsigned long long>(queueOverrunPackets),
                    static_cast<unsigned long long>(queueOverrunFrames), s.ringBufferUnderrunCount,
                    static_cast<unsigned long long>(s.latencyTrimSamples),
                    static_cast<unsigned long long>(s.latencyTrimSamples >= s.catastrophicResyncSamples
                                                        ? s.latencyTrimSamples - s.catastrophicResyncSamples
                                                        : 0),
                    s.catastrophicResyncEvents, static_cast<unsigned long long>(s.catastrophicResyncSamples));
            }
            DLL_Log("[StopAudio] Video: expectedDuration=%lld ms (%lld samples)", expectedVideoMs,
                    expectedVideoSamples);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(muxMutex);
            timingModeFrozenForSession = false;
            activeScreenGrab = false;
        }

        DLL_Log("[STOP SUMMARY] Recording finalized");
        DLL_Log("[AVSyncAuto] stop_summary: resolvedRenderLatencyMs=%.3f confidence=%s reason=%s usedAudioProbe=%d",
                static_cast<double>(config.avSyncResolvedRenderLatencyMs), config.avSyncConfidence.c_str(),
                config.avSyncReason.c_str(), config.avSyncUsedAudioProbe ? 1 : 0);
        if (videoEnc) {
            const int64_t finalVideoMs = videoEnc->GetExpectedFinalDurationUs() / 1000;
            const int64_t wallMs = videoElapsedMs.load();
            const int64_t encodedMs = videoEnc->GetEncodedDurationUs() / 1000;
            DLL_Log("[STOP SUMMARY] Video: duration=%lldms wall=%lldms encoded=%lldms pipelineLag=%lldms", finalVideoMs,
                    wallMs, encodedMs, wallMs - encodedMs);
        }
        {
            const int64_t expectedVideoSamples =
                ce::audio::ComputeDurationUsToSamples(expectedVideoUsForStop, kStopSampleRate);
            for (const auto& kv : cachedTrackToSources) {
                const int track = kv.first;
                const int64_t trackSamples = trackTimelineSamples.count(track) ? trackTimelineSamples[track] : 0;
                const int64_t diffSamples = trackSamples - expectedVideoSamples;
                const double diffMs = static_cast<double>(diffSamples) * 1000.0 / kStopSampleRate;
                std::string sourceList;
                for (size_t srcIdx : kv.second) {
                    if (!sourceList.empty()) {
                        sourceList += ",";
                    }
                    sourceList += std::to_string(srcIdx);
                }
                DLL_Log(
                    "[STOP AUDIO TRACK] Track %d: encoded=%lld expected=%lld diff=%+lld (%+.3f ms) realMixed=%llu "
                    "fullSilence=%llu partialSilence=%llu sources=[%s]",
                    track, trackSamples, expectedVideoSamples, diffSamples, diffMs,
                    (unsigned long long)trackRealMixedSamples[track],
                    (unsigned long long)trackFullSilenceSamples[track],
                    (unsigned long long)trackPartialSilenceSamples[track], sourceList.c_str());
            }
        }
        for (size_t i = 0; i < audioSources.size() && i < encodedSamplesPerSource.size(); i++) {
            auto& src = audioSources[i];
            const bool isApp = (src.sourceType == AudioConfig::AppAudio);
            const bool noAppData = isApp && !src.hasAlignedStart;
            const bool idleSystemLoopback = src.sourceType == AudioConfig::SystemAudio && !src.hasAlignedStart;
            if (noAppData && src.sawCaptureEpoch) {
                DLL_Log(
                    "[STOP AUDIO] Source %zu (app-active-no-data): track=%d process=%s; activation epoch was "
                    "ordered but no WASAPI data packet arrived, so the route contains expected timeline silence",
                    i, src.track, src.config.processName.empty() ? "<pid-mode>" : src.config.processName.c_str());
            } else if (noAppData) {
                DLL_Log("[STOP AUDIO] Source %zu (app-never-started): track=%d", i, src.track);
            } else if (idleSystemLoopback) {
                DLL_Log(
                    "[STOP AUDIO] Source %zu (system-inactive-no-data): track=%d encoded=%llu; the render endpoint "
                    "produced no packets, so its full recording timeline is expected silence",
                    i, src.track, static_cast<unsigned long long>(encodedSamplesPerSource[i]));
            } else {
                const double latencyTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.latencyTrimSamples, expectedVideoUsForStop);
                const double bootstrapTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.bootstrapTrimSamples, expectedVideoUsForStop);
                const double coverageTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.coverageLossTrimSamples, expectedVideoUsForStop);
                const double tier2TrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.tier2TrimSamples, expectedVideoUsForStop);
                const double retainedTrimPerMinute =
                    ce::audio::ComputeSamplesPerMinute(src.retainedNewestTrimSamples, expectedVideoUsForStop);
                const double ratePpm = ce::audio::ComputeClockMismatchPpm(src.currentRateDelta, kStopSampleRate);
                const uint64_t categorizedLatencyTrim =
                    std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                         src.coverageLossTrimSamples + src.tier2TrimSamples +
                                                         src.catastrophicResyncSamples);
                const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
                const uint64_t normalLatencyTrim = src.latencyTrimSamples >= src.catastrophicResyncSamples
                                                       ? src.latencyTrimSamples - src.catastrophicResyncSamples
                                                       : 0;
                DLL_Log(
                    "[STOP AUDIO] Source %zu: track=%d encoded=%llu trim=cov:%llu latTotal:%llu liveUncat:%llu "
                    "cat:%llu normal:%llu pad:%llu qgap:%llu qjoin:%llu qjoinKeep:%llu "
                    "ringPeak=%zu ringUnderruns=%u process=%s",
                    i, src.track, (unsigned long long)encodedSamplesPerSource[i],
                    (unsigned long long)src.coverageLossTrimSamples, (unsigned long long)src.latencyTrimSamples,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)src.catastrophicResyncSamples,
                    (unsigned long long)normalLatencyTrim, (unsigned long long)src.underrunPadSamples,
                    (unsigned long long)src.packetTimelineGapSamples,
                    (unsigned long long)src.lateAppJoinSuppressedGapSamples,
                    (unsigned long long)src.lateAppJoinPreservedGapSamples, src.ringBufferPeakSamples,
                    src.ringBufferUnderrunCount, src.config.processName.empty() ? "-" : src.config.processName.c_str());
                DLL_Log(
                    "[STOP AUDIO DETAIL] Source %zu: ratePpm=%+.2f compDelta=%d sat=%d trimRate(latTotal=%.1f/min "
                    "boot=%.1f/min cov=%.1f/min tier2=%.1f/min retain=%.1f/min) totals(boot=%llu tier2=%llu "
                    "retain=%llu cat=%llu catEvents=%u liveUncat=%llu post=%llu overlap=%llu ovf=%llu)",
                    i, ratePpm, src.currentRateDelta, src.targetRateSaturated ? 1 : 0, latencyTrimPerMinute,
                    bootstrapTrimPerMinute, coverageTrimPerMinute, tier2TrimPerMinute, retainedTrimPerMinute,
                    (unsigned long long)src.bootstrapTrimSamples, (unsigned long long)src.tier2TrimSamples,
                    (unsigned long long)src.retainedNewestTrimSamples,
                    (unsigned long long)src.catastrophicResyncSamples, src.catastrophicResyncEvents,
                    (unsigned long long)uncategorizedLatencyTrim, (unsigned long long)src.postResampleTrimSamples,
                    (unsigned long long)src.packetTimelineOverlapSamples, (unsigned long long)src.overflowDropSamples);
                // Consumer-overrun evidence. `starve` counts real captured samples destroyed
                // because the exported cursor ran past the live capture edge; a nonzero value
                // means audio content was lost even though every track length still matches.
                DLL_Log(
                    "[STOP AUDIO INGEST] Source %zu: track=%d starve=%llu resync=%llu/%u reservoirPeakMs=%lld "
                    "process=%s",
                    i, src.track, (unsigned long long)src.timelineStarvationDropSamples,
                    (unsigned long long)src.timelineResyncSuppressedSamples, src.timelineResyncEvents,
                    (long long)audioIngestReservoirPeakMs,
                    src.config.processName.empty() ? "-" : src.config.processName.c_str());
                if (src.timelineStarvationDropSamples > 0) {
                    DLL_Log(
                        "[STOP AUDIO INGEST] WARNING: Source %zu track=%d lost %llu real samples (%.1f ms) to "
                        "consumer overrun; the ingestion reservoir peaked at %lldms extra. Track lengths and PTS "
                        "stayed exact, but this much captured content never reached the file.",
                        i, src.track, (unsigned long long)src.timelineStarvationDropSamples,
                        static_cast<double>(src.timelineStarvationDropSamples) * 1000.0 /
                            static_cast<double>(kStopSampleRate),
                        (long long)audioIngestReservoirPeakMs);
                }
                DLL_Log(
                    "[AVSyncAuto] stop_audio_source: src=%zu track=%d codec=%s encodedSamples=%llu "
                    "captureLatencyMs=%.3f encoderReady=%d streamIndex=%d confidence=%s reason=%s",
                    i, src.track, src.config.codec.c_str(), (unsigned long long)encodedSamplesPerSource[i],
                    static_cast<double>(src.config.captureLatencyMs), (src.encoder && src.encoder->IsReady()) ? 1 : 0,
                    src.encoder ? src.encoder->GetStreamIndex() : -1, config.avSyncConfidence.c_str(),
                    config.avSyncReason.c_str());
            }
        }

        // Stop all audio sources
        for (auto& src : audioSources) {
            if (src.capture) {
                src.capture->Stop();
            }
            if (src.appCapture) {
                src.appCapture->Stop();
            }
            if (src.encoder) {
                src.encoder->SetExpectedSourceSilenceSamples(static_cast<int64_t>(trackFullSilenceSamples[src.track]));
                src.encoder->Stop();
            }

            // PULL MODEL: Clear ring buffers to prevent stale audio in next recording
            if (src.ringBuffer) {
                src.ringBuffer->Clear();
            }

            // DRIFT COMPENSATION: Clear sync buffers and reset counters
            if (src.syncResampler) {
                src.syncResampler->Reset();
            }
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
            src.epochResetRequested->store(0, std::memory_order_release);
            src.epochResetAcknowledged->store(0, std::memory_order_release);
            src.epochSyncTailFlushedGeneration = 0;
            src.lastEpochTransitionWaitLogTick = 0;
        }

        // Reset video frame tracking for next recording
        firstVideoFrameMs = 0;
        firstVideoFrameCommitted = false;
        lastVideoFrameMs = 0;
        recordingStartSystemQPCMs.store(0);
        recordingStartSystemQpc100ns.store(0);
        injectTimelineState.Reset();
        d3d11TimelineState.Reset();

        // Note: We don't need to update VideoEncoder audio context here anymore
        // since we're using AddAudioContext and the contexts are stored per-source

        // Stop video encoder (writes trailer)
        bool videoOutputPublished = false;
        if (videoEnc) {
            videoEnc->Stop();
            videoOutputPublished = videoEnc->WasLastOutputPublished();
        }
        return videoOutputPublished;

}

MediaEngine::~MediaEngine() {


        try {
        StopRecording();
        } catch (...) {
            DLL_Log("[MediaEngine] Suppressed exception during destruction");
        }

}

