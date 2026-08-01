                    "[FinalizationLattice] ERROR: failed to append repeat %lld/%lld at timelineUs=%lld; "
                    "decoded endpoint verification must reject any resulting mismatch",
                    static_cast<long long>(index + 1), static_cast<long long>(extensionFrames),
                    static_cast<long long>(nextTimelineUs));
                break;
            }
        }
        DLL_Log("[FinalizationLattice] CFR endpoint committed frames=%lld durationUs=%lld",
                static_cast<long long>(videoEnc->GetAssignedCfrFrameCount()),
                static_cast<long long>(videoEnc->GetExpectedFinalDurationUs()));
    }

    bool IsWgcCfrRecording() const {
        return SessionUsesScreenGrab() && !SessionUsesVfr();
    }

    bool IsCfrRecording() const {
        return ce::audio::ShouldUseCfrAudioContinuityPolicy(SessionUsesVfr());
    }

    double GetMaxAudioCaptureLatencyMs() const {
        double maxLatencyMs = 0.0;
        for (const auto& src : audioSources) {
            maxLatencyMs = std::max(maxLatencyMs, static_cast<double>(src.config.captureLatencyMs));
        }
        return maxLatencyMs;
    }

    int64_t GetMaxAudioCaptureLatencyQpc() const {
        const double maxLatencyMs = GetMaxAudioCaptureLatencyMs();
        if (qpcFreq <= 0 || maxLatencyMs <= 0.0) {
            return 0;
        }
        return static_cast<int64_t>(std::llround((maxLatencyMs / 1000.0) * static_cast<double>(qpcFreq)));
    }

    void SetWgcStartupExtraDelayQpc(int64_t delayQpc) {
        const int64_t clampedDelayQpc = std::max<int64_t>(0, delayQpc);
        wgcStartupExtraDelayQpc.store(clampedDelayQpc, std::memory_order_release);
        if (clampedDelayQpc > 0 && IsWgcCfrRecording()) {
            preservePendingStartupAudioPackets.store(true, std::memory_order_release);
        }
        if (qpcFreq > 0 || clampedDelayQpc > 0) {
            const double delayMs =
                qpcFreq > 0 ? (static_cast<double>(clampedDelayQpc) * 1000.0) / static_cast<double>(qpcFreq) : 0.0;
            DLL_Log("[AVSyncApply] wgc_start_extra_delay: smoothExtraDelayMs=%.3f smoothExtraDelayQpc=%lld", delayMs,
                    clampedDelayQpc);
        }
    }

    bool PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording || !texture || width == 0 || height == 0) {
            DLL_Log("[VideoPrewarm] D3D11 prepare rejected: encoder=%d recording=%d texture=%d dimensions=%ux%u",
                    videoEnc ? 1 : 0, recording ? 1 : 0, texture ? 1 : 0, width, height);
            return false;
        }

        const auto start = std::chrono::steady_clock::now();
        const bool prepared = videoEnc->PrepareFrameD3D11(static_cast<ID3D11Texture2D*>(texture), width, height, isHDR);
        const int64_t elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        DLL_Log(
            "[VideoPrewarm] D3D11 deferred encoder/mux prepare %s: dimensions=%ux%u hdr=%d elapsed=%lldus "
            "firstFrameCommitted=%d",
            prepared ? "complete" : "FAILED", width, height, isHDR ? 1 : 0, static_cast<long long>(elapsedUs),
            firstVideoFrameCommitted ? 1 : 0);
        return prepared;
    }

    // Direct D3D11 texture processing for screengrab mode (zero-copy)
    bool ProcessFrameD3D11(void* texture, int64_t timestampQPC, uint32_t width, uint32_t height, bool isHDR,
                           int32_t captureLeft, int32_t captureTop, int64_t timelineElapsedUs,
                           const ce::cursor::CaptureState* cursorState) {
        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc || !recording)
            return false;

        auto now = std::chrono::steady_clock::now();
        int64_t debugTimestamp = (qpcFreq > 0) ? (timestampQPC * 1000) / qpcFreq : timestampQPC;

        const bool commitsFirstVideoFrame = !this->firstVideoFrameCommitted;
        int64_t firstAnchorQpc = timestampQPC;
        int64_t firstAnchorMs = debugTimestamp;
        int64_t firstStartQpc100ns = 0;
        bool firstPreservePendingPackets = false;
        if (commitsFirstVideoFrame) {
            if (IsWgcCfrRecording()) {
                const double renderDelayMs = GetMaxAudioCaptureLatencyMs();
                const int64_t renderDelayQpc = GetMaxAudioCaptureLatencyQpc();
                const int64_t smoothExtraDelayQpc =
                    std::max<int64_t>(0, wgcStartupExtraDelayQpc.load(std::memory_order_acquire));
                const bool delayWouldOverflow =
                    renderDelayQpc > 0 && smoothExtraDelayQpc > 0 &&
                    renderDelayQpc > (std::numeric_limits<int64_t>::max() - smoothExtraDelayQpc);
                const int64_t startupDelayQpc =
                    delayWouldOverflow ? renderDelayQpc : (renderDelayQpc + smoothExtraDelayQpc);
                const double smoothExtraDelayMs =
                    qpcFreq > 0 ? (static_cast<double>(smoothExtraDelayQpc) * 1000.0) / static_cast<double>(qpcFreq)
                                : 0.0;
                const double startupDelayMs =
                    qpcFreq > 0 ? (static_cast<double>(startupDelayQpc) * 1000.0) / static_cast<double>(qpcFreq)
                                : renderDelayMs;
                firstAnchorQpc = ce::capture_policy::GetWgcStartupAudioAnchorQpc(timestampQPC, renderDelayQpc);
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                const int64_t nowQPC = qpcNow.QuadPart;
                const int64_t frameAgeUs =
                    (qpcFreq > 0 && nowQPC > timestampQPC) ? ((nowQPC - timestampQPC) * 1000000) / qpcFreq : 0;
                const int64_t audioAnchorDelayUs = (qpcFreq > 0 && firstAnchorQpc > timestampQPC)
                                                       ? ((firstAnchorQpc - timestampQPC) * 1000000) / qpcFreq
                                                       : 0;
                const int64_t startupContentDelayUs =
                    qpcFreq > 0 ? (startupDelayQpc * 1000000) / qpcFreq : audioAnchorDelayUs;
                DLL_Log(
                    "MediaEngine: WGC CFR startup anchor candidate from timeline origin plus render delay "
                    "(videoQPC=%lld anchorQPC=%lld nowQPC=%lld timelineAge=%lldus startupContentDelay=%lldus "
                    "audioAnchorDelay=%lldus contentDelayMs=%.3f renderDelayMs=%.3f smoothExtraDelayMs=%.3f "
                    "confidence=%s reason=%s)",
                    timestampQPC, firstAnchorQpc, nowQPC, frameAgeUs, startupContentDelayUs, audioAnchorDelayUs,
                    startupDelayMs, renderDelayMs, smoothExtraDelayMs, config.avSyncConfidence.c_str(),
                    config.avSyncReason.c_str());
                DLL_Log(
                    "[AVSyncApply] wgc_start_anchor_candidate: videoQpc=%lld audioAnchorQpc=%lld "
                    "audioAnchorDelayUs=%lld "
                    "startupContentDelayUs=%lld contentDelayMs=%.3f renderDelayMs=%.3f smoothExtraDelayMs=%.3f "
                    "confidence=%s reason=%s",
                    timestampQPC, firstAnchorQpc, audioAnchorDelayUs, startupContentDelayUs, startupDelayMs,
                    renderDelayMs, smoothExtraDelayMs, config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
            }

            firstAnchorMs = (qpcFreq > 0 && firstAnchorQpc > 0) ? (firstAnchorQpc * 1000) / qpcFreq : debugTimestamp;
            firstStartQpc100ns = (qpcFreq > 0 && firstAnchorQpc > 0)
                                     ? static_cast<int64_t>(ce::audio::RawQpcToHundredNanoseconds(
                                           static_cast<uint64_t>(firstAnchorQpc), static_cast<uint64_t>(qpcFreq)))
                                     : 0;
            firstPreservePendingPackets = ce::audio::ShouldPreservePendingAudioPacketsForStartupSync(
                IsWgcCfrRecording(), wgcStartupExtraDelayQpc.load(std::memory_order_acquire));
        }

        int64_t realElapsedUs = 0;
        const int64_t steadyElapsedUs =
            commitsFirstVideoFrame
                ? 0
                : std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        if (SessionUsesVfr()) {
            realElapsedUs = ComputeSourceDrivenElapsedUs(qpcFreq, timestampQPC, steadyElapsedUs, d3d11TimelineState);
        } else {
            // CFR output cadence is already driven by the encoder thread's fixed-rate
            // sample loop. Feeding the video encoder WGC source timestamps here causes
            // avoidable skip/dup churn when callback cadence jitters around that output
            // grid, so prefer the sample-clock timeline instead.  Catch-up paths can
            // provide an explicit CFR slot time so buffered fresh frames land on the
            // intended output grid instead of collapsing onto the current wall-clock.
            realElapsedUs = ResolveAuthoritativeCfrTimelineElapsedUs(steadyElapsedUs, timelineElapsedUs,
                                                                     d3d11TimelineState.lastElapsedUs);
        }
        const bool useExplicitWgcCfrTimeline = IsWgcCfrRecording() && timelineElapsedUs >= 0;
        if (cursorState) {
            videoEnc->SetCursorCaptureState(*cursorState);
        }
        if (!videoEnc->EncodeFrameD3D11((ID3D11Texture2D*)texture, realElapsedUs, width, height, isHDR, captureLeft,
                                        captureTop, useExplicitWgcCfrTimeline)) {
            DLL_Log("MediaEngine: D3D11 frame encode failed at ts=%lld", debugTimestamp);
            return false;
        }
        if (commitsFirstVideoFrame) {
            this->firstVideoFrameMs = debugTimestamp;
            this->firstVideoFrameCommitted = true;
            this->recordingStartTime = now;
            videoElapsedMs.store(0);
            DLL_Log("MediaEngine: First successfully encoded D3D11 frame at %lld ms (QPC: %lld) (StartQPC: %lld)",
                    debugTimestamp, timestampQPC, firstAnchorMs);
            SyncAudioToFirstVideoFrame(firstAnchorMs, firstStartQpc100ns, firstPreservePendingPackets);
        }
        // Keep the WGC live timeline anchored to the scheduled CFR wall clock even
        // when the encoder has fallen behind. Audio diagnostics and buffering logic
        // need to see that shortfall rather than the shortened encoded duration.
        CommitVideoElapsedUs(d3d11TimelineState, realElapsedUs);

        // Update audio stream index for all sources
        for (size_t i = 0; i < audioSources.size(); i++) {
            auto& src = audioSources[i];
            int idx = videoEnc->GetAudioStreamIndex(src.track);
            if (idx >= 0 && src.encoder) {
                src.encoder->SetStreamIndex(idx);
            }
        }

        if (screengrabFrameLogCount++ % 600 == 0) {
            DLL_Log("MediaEngine: ScreenGrab frame ts=%lld %dx%d", debugTimestamp, width, height);
        }

        // PULL MODEL: WGC CFR audio follows the PTS-based scheduled timeline
        // (GetExpectedFinalDurationUs) rather than the encoder grid time, which has
        // a 1-tick offset that would leave audio 8ms short by the end of recording.
        const int64_t audioTargetUs = IsWgcCfrRecording() ? videoEnc->GetExpectedFinalDurationUs() : realElapsedUs;
        PullAndEncodeAudio(audioTargetUs);
        return true;
    }

    void AppendSyncResamplerOutput(AudioSource& src, size_t srcIdx, int channels, uint8_t** resampledData,
                                   int outSamples) {
        if (outSamples <= 0) {
            return;
        }

        const uint64_t syntheticPostSamples = ce::audio::ConsumeSyntheticBufferedSamples(
            src.startupSyntheticResamplerSamples, static_cast<uint64_t>(outSamples));
        src.startupSyntheticPostSamples += syntheticPostSamples;
        if (!resampledData || !resampledData[0]) {
            DLL_Log("[PullAudio] ERROR: sync resampler returned %d samples without data for src=%zu", outSamples,
                    srcIdx);
            return;
        }

        constexpr int kMixerSampleRate = 48000;
        float* outFloats = reinterpret_cast<float*>(resampledData[0]);
        if (src.dropFadeSamplesRemaining > 0) {
            const int kDropFadeSamples = kMixerSampleRate / 40;
            const int blendSamples = std::min(src.dropFadeSamplesRemaining, outSamples);
            const int blendStart = kDropFadeSamples - src.dropFadeSamplesRemaining;
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha = static_cast<float>(blendStart + sample + 1) / kDropFadeSamples;
                for (int channel = 0; channel < channels; ++channel) {
                    const size_t index = static_cast<size_t>(sample) * channels + channel;
                    const float anchor = GetDropFadeAnchor(src, channel);
                    outFloats[index] = anchor + (outFloats[index] - anchor) * alpha;
                }
            }
            if (blendSamples > 0) {
                src.dropFadeStart.assign(static_cast<size_t>(channels), 0.0f);
                const size_t base = static_cast<size_t>(blendSamples - 1) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    src.dropFadeStart[static_cast<size_t>(channel)] = outFloats[base + channel];
                }
                src.dropFadeStartL = src.dropFadeStart[0];
                src.dropFadeStartR = channels > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
            }
            src.dropFadeSamplesRemaining -= blendSamples;
        }

        if (src.packetBoundaryFadeInSamplesRemaining > 0) {
            const int blendSamples = std::min(src.packetBoundaryFadeInSamplesRemaining, outSamples);
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha =
                    ComputeRaisedCosineFade(static_cast<size_t>(sample),
                                            static_cast<size_t>(std::max(src.packetBoundaryFadeInSamplesRemaining, 1)));
                const size_t base = static_cast<size_t>(sample) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    outFloats[base + channel] *= alpha;
                }
            }
            src.packetBoundaryFadeInSamplesRemaining -= blendSamples;
        }

        if (src.pendingUnderrunRecoveryFade) {
            src.underrunFadeSamplesRemaining = kMixerSampleRate / 40;
            src.pendingUnderrunRecoveryFade = false;
        }
        if (src.underrunFadeSamplesRemaining > 0) {
            const int kUnderrunFadeSamples = kMixerSampleRate / 40;
            const int blendSamples = std::min(src.underrunFadeSamplesRemaining, outSamples);
            const int blendStart = kUnderrunFadeSamples - src.underrunFadeSamplesRemaining;
            for (int sample = 0; sample < blendSamples; ++sample) {
                const float alpha = static_cast<float>(blendStart + sample + 1) / kUnderrunFadeSamples;
                const size_t base = static_cast<size_t>(sample) * channels;
                for (int channel = 0; channel < channels; ++channel) {
                    outFloats[base + channel] *= alpha;
                }
            }
            src.underrunFadeSamplesRemaining -= blendSamples;
        }

        const int numFloats = outSamples * channels;
        src.postResampleBuffer.insert(src.postResampleBuffer.end(), outFloats, outFloats + numFloats);
        src.syncSamplesOutput += outSamples;

        if (dropLogCounter++ % 500 == 0 &&
            (src.overflowDropSamples > 0 || src.latencyTrimSamples > 0 || src.postResampleTrimSamples > 0)) {
            const uint64_t categorizedLatencyTrim =
                std::min(src.latencyTrimSamples, src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                     src.coverageLossTrimSamples + src.tier2TrimSamples);
            const uint64_t uncategorizedLatencyTrim = src.latencyTrimSamples - categorizedLatencyTrim;
            DLL_Log(
                "[PullAudio] Sample trim stats src=%zu: overflowDropped=%llu latencyTrimTotal=%llu "
                "bootstrapTrim=%llu retainedTrim=%llu coverageTrim=%llu tier2Trim=%llu "
                "uncategorizedLiveTrim=%llu postResampleTrim=%llu",
                srcIdx, static_cast<unsigned long long>(src.overflowDropSamples),
                static_cast<unsigned long long>(src.latencyTrimSamples),
                static_cast<unsigned long long>(src.bootstrapTrimSamples),
                static_cast<unsigned long long>(src.retainedNewestTrimSamples),
                static_cast<unsigned long long>(src.coverageLossTrimSamples),
                static_cast<unsigned long long>(src.tier2TrimSamples),
                static_cast<unsigned long long>(uncategorizedLatencyTrim),
                static_cast<unsigned long long>(src.postResampleTrimSamples));
        }
    }

    bool PumpSourceRingThroughSyncResampler(AudioSource& src, size_t srcIdx, int channels, size_t maxFloats) {
        if (!src.ringBuffer || !src.syncResampler || !src.syncResampler->IsReady() || channels <= 0) {
            return false;
        }

        size_t chunkFloats = std::min(src.ringBuffer->GetAvailable(), maxFloats);
        chunkFloats -= chunkFloats % static_cast<size_t>(channels);
        if (chunkFloats == 0) {
            return false;
        }

        const size_t bufferedSamples = src.ringBuffer->GetAvailable() / static_cast<size_t>(channels);
        src.ringBufferPeakSamples = std::max(src.ringBufferPeakSamples, static_cast<uint64_t>(bufferedSamples));
        std::vector<float> ringData(chunkFloats);
        const size_t actualRead = src.ringBuffer->Read(ringData.data(), chunkFloats);
        if (actualRead == 0) {
            return false;
        }

        const size_t actualReadSamples = actualRead / static_cast<size_t>(channels);
        const uint64_t syntheticReadSamples =
            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, actualReadSamples);
        src.startupSyntheticResamplerSamples += syntheticReadSamples;

        uint8_t** resampledData = nullptr;
        int outSamples = 0;
        const bool processed =
            src.syncResampler->Process(reinterpret_cast<uint8_t*>(ringData.data()),
                                       static_cast<int>(actualRead * sizeof(float)), &resampledData, &outSamples);
        if (processed) {
            AppendSyncResamplerOutput(src, srcIdx, channels, resampledData, outSamples);
        } else {
            DLL_Log("[PullAudio] ERROR: sync resampler rejected %zu samples for src=%zu", actualReadSamples, srcIdx);
        }
        AudioResampler::FreeOutputBuffer(resampledData);
        return true;
    }

    bool FlushCaptureResamplerForEpoch(AudioSource& src, size_t srcIdx, uint64_t oldEpoch, uint64_t newEpoch) {
        if (!src.resampler || !src.resampler->IsReady() || !src.ringBuffer) {
            return true;
        }

        const size_t channels = static_cast<size_t>(std::clamp(src.mixChannels, 1, 8));
        const int64_t delayedSamples = std::max<int64_t>(0, src.resampler->GetDelay());
        if (delayedSamples > static_cast<int64_t>(std::numeric_limits<size_t>::max() / channels)) {
            DLL_Log(
                "[AudioEpoch] ERROR: Capture-resampler tail size overflow src=%zu track=%d epoch=%llu->%llu "
                "delay=%lld channels=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(oldEpoch), static_cast<unsigned long long>(newEpoch),
                static_cast<long long>(delayedSamples), channels);
            return false;
        }

        const size_t requiredFloats = static_cast<size_t>(delayedSamples) * channels;
        const size_t freeFloats = src.ringBuffer->GetFree();
        if (requiredFloats > freeFloats) {
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - src.lastEpochTransitionWaitLogTick >= 1000) {
                DLL_Log(
                    "[AudioEpoch] Waiting for capture-tail ring capacity src=%zu track=%d epoch=%llu->%llu "
                    "requiredFloats=%zu freeFloats=%zu bufferedSamples=%zu",
                    srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                    static_cast<unsigned long long>(newEpoch), requiredFloats, freeFloats,
                    src.ringBuffer->GetAvailable() / channels);
                src.lastEpochTransitionWaitLogTick = nowTick;
            }
            return false;
        }

        size_t flushedSamples = 0;
        constexpr size_t kMaxFlushPasses = 8;
        bool flushComplete = false;
        for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
            uint8_t** tailData = nullptr;
            int tailSamples = 0;
            const AudioResampler::FlushResult flushResult = src.resampler->Flush(&tailData, &tailSamples);
            if (flushResult == AudioResampler::FlushResult::Error) {
                AudioResampler::FreeOutputBuffer(tailData);
                DLL_Log(
                    "[AudioEpoch] ERROR: Capture-resampler flush failed src=%zu track=%d epoch=%llu->%llu "
                    "flushed=%zu",
                    srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                    static_cast<unsigned long long>(newEpoch), flushedSamples);
                return false;
            }
            if (flushResult == AudioResampler::FlushResult::Complete) {
                flushComplete = true;
                break;
            }
            if (tailSamples > 0 && tailData && tailData[0]) {
                const size_t tailFloats = static_cast<size_t>(tailSamples) * channels;
                const size_t writtenFloats =
                    src.ringBuffer->Write(reinterpret_cast<const float*>(tailData[0]), tailFloats);
                if (writtenFloats != tailFloats) {
                    DLL_Log(
                        "[AudioEpoch] ERROR: Capture tail could not be preserved src=%zu track=%d "
                        "epoch=%llu->%llu requestedFloats=%zu writtenFloats=%zu",
                        srcIdx, src.track, static_cast<unsigned long long>(oldEpoch),
                        static_cast<unsigned long long>(newEpoch), tailFloats, writtenFloats);
                    AudioResampler::FreeOutputBuffer(tailData);
                    return false;
                }
                const size_t writtenSamples = writtenFloats / channels;
                src.qpcAlignedWrittenSamples += writtenSamples;
                flushedSamples += writtenSamples;
            }
            AudioResampler::FreeOutputBuffer(tailData);
        }

        if (!flushComplete) {
            DLL_Log(
                "[AudioEpoch] Capture-resampler flush pass bound reached src=%zu track=%d epoch=%llu->%llu "
                "flushed=%zu reportedDelay=%lld; continuing on the next owner pass",
                srcIdx, src.track, static_cast<unsigned long long>(oldEpoch), static_cast<unsigned long long>(newEpoch),
                flushedSamples, static_cast<long long>(std::max<int64_t>(0, src.resampler->GetDelay())));
            return false;
        }

        DLL_Log(
            "[AudioEpoch] Capture owner published epoch reset src=%zu track=%d epoch=%llu->%llu "
            "captureTail=%zu ringBuffered=%zu",
            srcIdx, src.track, static_cast<unsigned long long>(oldEpoch), static_cast<unsigned long long>(newEpoch),
            flushedSamples, src.ringBuffer->GetAvailable() / channels);
        return true;
    }

    bool ServiceAudioEpochResetOnPull(AudioSource& src, size_t srcIdx) {
        if (!src.epochResetRequested || !src.epochResetAcknowledged) {
            return false;
        }
        const uint64_t requested = src.epochResetRequested->load(std::memory_order_acquire);
        const uint64_t acknowledged = src.epochResetAcknowledged->load(std::memory_order_acquire);
        if (requested == 0 || requested == acknowledged) {
            return false;
        }

        const int channels = std::clamp(src.mixChannels, 1, 8);
        if (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
            return false;
        }

        if (src.epochSyncTailFlushedGeneration != requested) {
            size_t syncTailSamples = 0;
            constexpr size_t kMaxFlushPasses = 8;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                bool flushComplete = false;
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult == AudioResampler::FlushResult::Error) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        DLL_Log(
                            "[AudioEpoch] ERROR: Sync-resampler flush failed src=%zu track=%d generation=%llu "
                            "flushed=%zu",
                            srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples);
                        return false;
                    }
                    if (flushResult == AudioResampler::FlushResult::Complete) {
                        flushComplete = true;
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
                if (!flushComplete) {
                    DLL_Log(
                        "[AudioEpoch] Sync-resampler flush pass bound reached src=%zu track=%d generation=%llu "
                        "flushed=%zu reportedDelay=%lld; continuing on the next pull pass",
                        srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                        static_cast<long long>(std::max<int64_t>(0, src.syncResampler->GetDelay())));
                    return false;
                }
            }
            src.epochSyncTailFlushedGeneration = requested;
            DLL_Log(
                "[AudioEpoch] Pull owner drained sync tail src=%zu track=%d generation=%llu syncTail=%zu "
                "postBuffered=%zu",
                srcIdx, src.track, static_cast<unsigned long long>(requested), syncTailSamples,
                src.postResampleBuffer.size() / static_cast<size_t>(channels));
        }

        if (!src.postResampleBuffer.empty()) {
            return false;
        }
        if (src.syncResampler && src.syncResampler->IsReady() && !src.syncResampler->Reset()) {
            DLL_Log("[AudioEpoch] ERROR: Pull-owner sync reset failed src=%zu track=%d generation=%llu", srcIdx,
                    src.track, static_cast<unsigned long long>(requested));
            return false;
        }

        src.startupSyntheticResamplerSamples = 0;
        src.startupSyntheticPostSamples = 0;
        src.dropFadeSamplesRemaining = 0;
        src.dropFadeStartL = 0.0f;
        src.dropFadeStartR = 0.0f;
        src.dropFadeStart.clear();
        src.underrunFadeSamplesRemaining = 0;
        src.pendingUnderrunRecoveryFade = true;
        src.appAudioBacklogDrainInitialized = false;
        src.appAudioBacklogDrainActive = false;
        src.appAudioBacklogDrainReason =
            static_cast<uint32_t>(ce::audio::CfrAppAudioBacklogDrainReason::EpochRejoinPending);
        src.appAudioBacklogTargetSamples = 0;
        src.appAudioBacklogExcessSamples = 0;
        src.appAudioBacklogCompensationDelta = 0;
        src.prevLeadSamples = 0;
        src.prevLeadSnapshotMs = 0;
        src.lastRateUpdateMs = 0;
        src.currentRateDelta = 0;
        src.targetRateDelta = 0;
        src.rateCompActive = false;
        src.targetRateSaturated = false;
        src.epochResetAcknowledged->store(requested, std::memory_order_release);
        audioDrainCv.notify_all();
        DLL_Log(
            "[AudioEpoch] Pull owner acknowledged reset src=%zu track=%d generation=%llu ring=0 post=0 "
            "trackCursor=%lld sourceEncoded=%lld",
            srcIdx, src.track, static_cast<unsigned long long>(requested),
            static_cast<long long>(trackTimelineSamples[src.track]),
            srcIdx < encodedSamplesPerSource.size() ? static_cast<long long>(encodedSamplesPerSource[srcIdx]) : -1ll);
        return true;
    }

    void FlushAudioOnlyResamplerTails() {
        constexpr size_t kMaxFlushPasses = 8;
        constexpr size_t kSyncPumpSamples = 4800;
        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            auto& src = audioSources[srcIdx];
            const TrackAudioFormat trackFormat = GetTrackAudioFormat(src.track);
            const int channels = std::clamp(trackFormat.channels, 1, 8);

            size_t captureTailSamples = 0;
            if (src.resampler && src.resampler->IsReady() && src.ringBuffer) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.resampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Capture-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    if (tailSamples > 0 && tailData && tailData[0]) {
                        float* tailFloats = reinterpret_cast<float*>(tailData[0]);
                        if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                            const size_t fadeSamples = static_cast<size_t>(src.packetBoundaryFadeInSamplesRemaining);
                            ApplyPacketBoundaryFadeIn(tailFloats, static_cast<size_t>(tailSamples),
                                                      static_cast<size_t>(channels), fadeSamples);
                            src.packetBoundaryFadeInSamplesRemaining =
                                fadeSamples > static_cast<size_t>(tailSamples)
                                    ? static_cast<int>(fadeSamples - static_cast<size_t>(tailSamples))
                                    : 0;
                        }
                        const size_t writtenFloats = src.ringBuffer->WriteRetainNew(
                            tailFloats, static_cast<size_t>(tailSamples) * static_cast<size_t>(channels));
                        const size_t writtenSamples = writtenFloats / static_cast<size_t>(channels);
                        src.qpcAlignedWrittenSamples += writtenSamples;
                        captureTailSamples += writtenSamples;
                    }
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            size_t syncInputSamples = 0;
            while (src.ringBuffer && src.ringBuffer->GetAvailable() >= static_cast<size_t>(channels)) {
                const size_t before = src.ringBuffer->GetAvailable();
                if (!PumpSourceRingThroughSyncResampler(src, srcIdx, channels,
                                                        kSyncPumpSamples * static_cast<size_t>(channels))) {
                    break;
                }
                syncInputSamples += (before - src.ringBuffer->GetAvailable()) / static_cast<size_t>(channels);
            }

            size_t syncTailSamples = 0;
            if (src.syncResampler && src.syncResampler->IsReady()) {
                for (size_t pass = 0; pass < kMaxFlushPasses; ++pass) {
                    uint8_t** tailData = nullptr;
                    int tailSamples = 0;
                    const AudioResampler::FlushResult flushResult = src.syncResampler->Flush(&tailData, &tailSamples);
                    if (flushResult != AudioResampler::FlushResult::Output) {
                        AudioResampler::FreeOutputBuffer(tailData);
                        if (flushResult == AudioResampler::FlushResult::Error) {
                            DLL_Log("[StopAudio] ERROR: Sync-resampler tail flush failed src=%zu track=%d", srcIdx,
                                    src.track);
                        }
                        break;
                    }
                    AppendSyncResamplerOutput(src, srcIdx, channels, tailData, tailSamples);
                    syncTailSamples += static_cast<size_t>(std::max(tailSamples, 0));
                    AudioResampler::FreeOutputBuffer(tailData);
                }
            }

            if (captureTailSamples > 0 || syncTailSamples > 0) {
                DLL_Log(
                    "[StopAudio] Flushed audio-only resampler tails: src=%zu track=%d captureTail=%zu "
                    "syncInput=%zu syncTail=%zu postBuffered=%zu",
                    srcIdx, src.track, captureTailSamples, syncInputSamples, syncTailSamples,
                    src.postResampleBuffer.size() / static_cast<size_t>(channels));
            }
        }
    }

    // PULL MODEL: Pull audio from RingBuffers and encode against the relative
    // recording timeline that also drives CFR video emission.
    void PullAndEncodeAudio(int64_t videoTimelineUs, bool forceDrain = false) {
        if (!recording || audioSources.empty())
            return;

        constexpr int SAMPLE_RATE = 48000;
        constexpr int64_t kSteadyAudioPullLatencyMs = ce::audio::kDefaultSteadyAudioPullLatencyMs;
        constexpr int64_t kPrimedSourceCushionSamples = SAMPLE_RATE / 40;  // 25ms
        constexpr int64_t kBaseTargetLatencySamples = (kSteadyAudioPullLatencyMs * SAMPLE_RATE) / 1000;
        constexpr int64_t kMaxPipelineLagContributionMs = 10000;
        constexpr int64_t kWgcCoverageLossMaxBufferedLagMs = 300;
        constexpr int64_t kRuntimeMaxLeadSamples = SAMPLE_RATE / 10;            // 100ms above target
        constexpr int64_t kRuntimeMaxDropPerCall = SAMPLE_RATE / 200;           // 5ms
        constexpr int64_t kRuntimeDropFadeSamples = SAMPLE_RATE / 100;          // 10ms
        constexpr int64_t kOverloadAudioPullQuantumSamples = SAMPLE_RATE / 50;  // 20ms
        constexpr int64_t kLatencyTrimHysteresisSamples = ce::audio::kDefaultAudioPullQuantumSamples;
        constexpr int64_t kMinCompensationBufferSamples = kBaseTargetLatencySamples / 4;
        constexpr int64_t kWgcCfrLeadWarningSamples = SAMPLE_RATE / 10;  // 100ms
        constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = true;
        constexpr double kDefaultMaxCompensationPercent = 1.0;
        constexpr double kTier1MaxPitchPercent = 0.05;  // Keep WGC source-clock correction below audible pitch shift.
        // CFR app-audio latency drain. Process-loopback sources can sit above the video-derived live
        // target because of a route-local delivery backlog. Drain that backlog through resampler
        // compensation only when the CFR timeline itself is healthy; wall-time video debt naturally
        // buffers every live audio source and must be repaid by video holds, not app-only pitch change.
        constexpr double kAppAudioDrainMaxPitchPercent = 0.5;
        constexpr int64_t kAppAudioDrainSlackSamples = SAMPLE_RATE / 50;         // 20ms above target
        constexpr int64_t kAppAudioDrainDeadbandSamples = SAMPLE_RATE / 100;     // 10ms compensation deadband
        constexpr int64_t kAppAudioLatencyWarnExcessSamples = SAMPLE_RATE / 20;  // 50ms above target
        constexpr int64_t kTier2DriftThresholdMs = 20;
        constexpr int64_t kWgcEncoderShortfallBufferedLagMaxMs = 4000;
        constexpr uint32_t kWgcEncoderHealthyDeliveryMarginFps = 4;
        constexpr int64_t kWgcCoverageLossLeadSlackSamples = SAMPLE_RATE / 25;  // 40ms above target
        constexpr int64_t kWgcCoverageLossMaxDropPerCall =
            ce::audio::kDefaultAudioPullQuantumSamples;  // 5ms paced overload trim quantum
        constexpr int64_t kWgcVisualSyncMaxBufferedLagMs = 4000;

        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }

        // Audio-only recording follows its wall-clock timeline. Video CFR
