#include "mediaengine_internal.h"


bool MediaEngine::ServiceAudioEpochResetOnPull(AudioSource& src,  size_t srcIdx) {


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

void MediaEngine::FlushAudioOnlyResamplerTails() {


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
