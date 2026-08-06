#include "mediaengine_internal.h"

bool MediaEngine::PullTrackEncodeSourcesC1(AudioPullState& s, int track, size_t srcIdx) {
    auto& effectiveSourceClockDriftLagMs = s.effectiveSourceClockDriftLagMs;
    auto& CHANNELS = s.CHANNELS;
    auto& targetLatencySamples = s.targetLatencySamples;
    auto& realCopiedSamples = s.realCopiedSamples;
    auto& it = s.it;
    auto& forceDrain = s.forceDrain;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kBaseTargetLatencySamples = AudioPullState::kBaseTargetLatencySamples;
    constexpr int64_t kAppAudioLatencyWarnExcessSamples = AudioPullState::kAppAudioLatencyWarnExcessSamples;
        auto& src = audioSources[srcIdx];
        auto& trackCursorSamples = trackTimelineSamples[track];
                if (src.sourceType == AudioConfig::AppAudio) {
                    const uint64_t nowConsumeTick = GetTickCount64();
                    const size_t rbAvailSamples = src.ringBuffer ? src.ringBuffer->GetAvailable() / CHANNELS : 0;
                    const bool starvedWithRingData = (realCopiedSamples == 0 && rbAvailSamples > 0);
                    const int64_t appTargetSamples =
                        std::max<int64_t>(targetLatencySamples,
                                          kBaseTargetLatencySamples +
                                              (effectiveSourceClockDriftLagMs * SAMPLE_RATE / 1000));
                    const int64_t appExcessSamples =
                        std::max<int64_t>(0, static_cast<int64_t>(rbAvailSamples) - appTargetSamples);
                    const uint32_t appTargetMs =
                        static_cast<uint32_t>(std::max<int64_t>(0, appTargetSamples) * 1000 / SAMPLE_RATE);
                    const uint32_t appExcessMs = static_cast<uint32_t>(appExcessSamples * 1000 / SAMPLE_RATE);
                    const double appCompPct =
                        static_cast<double>(src.currentRateDelta) * 100.0 / (static_cast<double>(SAMPLE_RATE) * 10.0);
                    const auto appDrainReason =
                        static_cast<ce::audio::CfrAppAudioBacklogDrainReason>(src.appAudioBacklogDrainReason);
                    src.appAudioBacklogTargetSamples = appTargetSamples;
                    src.appAudioBacklogExcessSamples = appExcessSamples;
                    src.appLatencyMaxAbsCompDelta = std::max<uint32_t>(
                        src.appLatencyMaxAbsCompDelta, static_cast<uint32_t>(std::abs(src.currentRateDelta)));

                    // Latency observability: the ring backlog at consume time IS the audio-behind-video
                    // delay (buffered audio waiting to be emitted). Sample it EVERY pull so the
                    // recording-wide distribution and any elevated/variable latency are obvious in the
                    // logs rather than needing manual reconstruction from raw cursor values.
                    const uint32_t appDelayMs =
                        static_cast<uint32_t>(static_cast<uint64_t>(rbAvailSamples) * 1000ull / SAMPLE_RATE);
                    if (forceDrain) {
                        // Stop finalization intentionally consumes the remaining route backlog in large
                        // pulls. Keep it visible, but do not mix those artificial observations into the
                        // live distribution or emit a live-capture latency warning.
                        ++src.appLatencyStopDrainSampleCount;
                        src.appLatencyStopDrainSumMs += appDelayMs;
                        src.appLatencyStopDrainMaxMs = std::max(src.appLatencyStopDrainMaxMs, appDelayMs);
                    } else {
                        const int latBucket = appDelayMs < 50    ? 0
                                              : appDelayMs < 150 ? 1
                                              : appDelayMs < 300 ? 2
                                              : appDelayMs < 600 ? 3
                                                                 : 4;
                        src.appLatencyBuckets[latBucket]++;
                        src.appLatencySampleCount++;
                        src.appLatencySumMs += appDelayMs;
                        src.appLatencyTargetSumMs += appTargetMs;
                        src.appLatencyExcessSumMs += appExcessMs;
                        if (appDelayMs > src.appLatencyMaxMs) {
                            src.appLatencyMaxMs = appDelayMs;
                        }
                        if (appExcessMs > src.appLatencyExcessMaxMs) {
                            src.appLatencyExcessMaxMs = appExcessMs;
                        }
                        if (src.appAudioBacklogDrainActive) {
                            src.appLatencyDrainingSamples++;
                        }

                        // Flag clearly-elevated latency loudly WHILE it happens (the signal that was missing).
                        // Post-fix the drain should keep this rare; frequent firing means latency is not draining.
                        constexpr uint32_t kAppLatencyWarnMs = 250;
                        const bool appLatencyElevated =
                            appExcessSamples >= kAppAudioLatencyWarnExcessSamples || appDelayMs >= kAppLatencyWarnMs;
                        const bool appLatencyWarnChanged = appLatencyElevated != src.appLatencyWarnActive;
                        if (appLatencyWarnChanged) {
                            ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                            const size_t pendingPackets = routedCapture ? routedCapture->PendingPacketCount() : 0;
                            const uint64_t queueOverrunPackets =
                                routedCapture ? routedCapture->GetQueueOverrunPacketCount() : 0;
                            const uint64_t queueOverrunFrames =

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
    return true;
}
bool MediaEngine::PullTrackEncodeSourcesC2(AudioPullState& s, int track, size_t srcIdx) {
    auto& CHANNELS = s.CHANNELS;
    auto& totalFloats = s.totalFloats;
    auto& mixBuffer = s.mixBuffer;
    auto& expectedTimelineSilence = s.expectedTimelineSilence;
    auto& srcData = s.srcData;
    auto& available = s.available;
    auto& toCopy = s.toCopy;
    auto& realCopiedSamples = s.realCopiedSamples;
    auto& it = s.it;
    auto& fadeStart = s.fadeStart;
    auto& forceDrain = s.forceDrain;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
        auto& src = audioSources[srcIdx];
        auto& trackCursorSamples = trackTimelineSamples[track];
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
    return true;
}
