#include "mediaengine_internal.h"

bool MediaEngine::ComputeAudioPullTargets(AudioPullState& s, int64_t videoTimelineUs, bool forceDrain) {
    auto& isCfrRecording = s.isCfrRecording;
    auto& isWgcCfrRecording = s.isWgcCfrRecording;
    auto& pullTick = s.pullTick;
    auto& effectiveAudioPullLatencyMs = s.effectiveAudioPullLatencyMs;
    auto& baseTargetLatencySamples = s.baseTargetLatencySamples;
    auto& reservoirPassBaseMs = s.reservoirPassBaseMs;
    auto& wallVideoMs = s.wallVideoMs;
    auto& encodedVideoMs = s.encodedVideoMs;
    auto& audioTargetUs = s.audioTargetUs;
    auto& audioTargetMs = s.audioTargetMs;
    auto& now = s.now;
    auto& steadyElapsedUs = s.steadyElapsedUs;
    auto& timelineShortfallMs = s.timelineShortfallMs;
    auto& videoPipelineLagMs = s.videoPipelineLagMs;
    auto& configuredWgcOutputFps = s.configuredWgcOutputFps;
    auto& wgcTargetFps = s.wgcTargetFps;
    auto& wgcDeliveredFps = s.wgcDeliveredFps;
    auto& wgcDeliveredMin250Fps = s.wgcDeliveredMin250Fps;
    auto& wgcDeliveredMin500Fps = s.wgcDeliveredMin500Fps;
    auto& wgcBufferedVideoContentLagMs = s.wgcBufferedVideoContentLagMs;
    auto& wgcCoverageLossActive = s.wgcCoverageLossActive;
    auto& wgcOverloadFlags = s.wgcOverloadFlags;
    auto& wgcEncoderBottlenecked = s.wgcEncoderBottlenecked;
    auto& wgcQueueEmptyTickPermille = s.wgcQueueEmptyTickPermille;
    auto& wgcBufferedAtTickMin = s.wgcBufferedAtTickMin;
    auto& wgcSingleFrameTickCount = s.wgcSingleFrameTickCount;
    auto& wgcSelectionBiasUs = s.wgcSelectionBiasUs;
    auto& wgcRecordingCadenceFps = s.wgcRecordingCadenceFps;
    auto& wgcAudioLagTargets = s.wgcAudioLagTargets;
    auto& wgcSteadyStateBufferedAudioLagMs = s.wgcSteadyStateBufferedAudioLagMs;
    auto& effectiveDeliveredFpsForAudioContinuity = s.effectiveDeliveredFpsForAudioContinuity;
    auto& wgcEncoderOnlyOverload = s.wgcEncoderOnlyOverload;
    auto& wgcEncoderShortfallBufferedLagMs = s.wgcEncoderShortfallBufferedLagMs;
    auto& screenGrabDriftLagMs = s.screenGrabDriftLagMs;
    auto& effectiveSourceClockDriftLagMs = s.effectiveSourceClockDriftLagMs;
    auto& wgcSelectedContentLeadMs = s.wgcSelectedContentLeadMs;
    auto& wgcSelectedContentLagMs = s.wgcSelectedContentLagMs;
    auto& wgcVisualContentLagMs = s.wgcVisualContentLagMs;
    auto& screenGrabTargetBufferLagMs = s.screenGrabTargetBufferLagMs;
    auto& effectiveAudioTargetBufferLagMs = s.effectiveAudioTargetBufferLagMs;
    auto& targetBufferedLagCapMs = s.targetBufferedLagCapMs;
    auto& maxWgcAudioLeadExcessSamples = s.maxWgcAudioLeadExcessSamples;
    auto& wallVideoLagMs = s.wallVideoLagMs;
    auto& cfrTimelineRecoveryActive = s.cfrTimelineRecoveryActive;
    auto& reservoirTick = s.reservoirTick;
    auto& reservoirActive = s.reservoirActive;
    auto& elapsedSinceEvalMs = s.elapsedSinceEvalMs;
    auto& observedHeadroomSamples = s.observedHeadroomSamples;
    auto& headroomObserved = s.headroomObserved;
    auto& observedHeadroomMs = s.observedHeadroomMs;
    auto& reservoirDecision = s.reservoirDecision;
    auto& reservoirChanged = s.reservoirChanged;
    auto& encodedVideoUs = s.encodedVideoUs;
    auto& telemetryTargetFps = s.telemetryTargetFps;
    auto& steadyElapsedMs = s.steadyElapsedMs;
    auto& it = s.it;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kSteadyAudioPullLatencyMs = AudioPullState::kSteadyAudioPullLatencyMs;
    constexpr int64_t kMaxPipelineLagContributionMs = AudioPullState::kMaxPipelineLagContributionMs;
    constexpr int64_t kWgcCoverageLossMaxBufferedLagMs = AudioPullState::kWgcCoverageLossMaxBufferedLagMs;
    constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = AudioPullState::kWgcPreferVideoRepeatsOverAudioCuts;
    constexpr int64_t kWgcEncoderShortfallBufferedLagMaxMs = AudioPullState::kWgcEncoderShortfallBufferedLagMaxMs;
    constexpr uint32_t kWgcEncoderHealthyDeliveryMarginFps = AudioPullState::kWgcEncoderHealthyDeliveryMarginFps;
    constexpr int64_t kWgcVisualSyncMaxBufferedLagMs = AudioPullState::kWgcVisualSyncMaxBufferedLagMs;


        // CFR app-audio latency drain. Process-loopback sources can sit above the video-derived live
        // target because of a route-local delivery backlog. Drain that backlog through resampler
        // compensation only when the CFR timeline itself is healthy; wall-time video debt naturally
        // buffers every live audio source and must be repaid by video holds, not app-only pitch change.
            ce::audio::kDefaultAudioPullQuantumSamples;  // 5ms paced overload trim quantum

        for (size_t srcIdx = 0; srcIdx < audioSources.size(); ++srcIdx) {
            ServiceAudioEpochResetOnPull(audioSources[srcIdx], srcIdx);
        }

        // Audio-only recording follows its wall-clock timeline. Video CFR

        // continuity policies (encoder-lag reservoirs, CFR source waits, and
        // overload trimming) do not apply when there is no video encoder.
        isCfrRecording = !audioOnly && IsCfrRecording();
        isWgcCfrRecording = !audioOnly && IsWgcCfrRecording();

        pullTick = GetTickCount64();

        // Adaptive ingestion reservoir. Deepening the pull lookahead is the only
        // recovery for a consumer that overran the live capture edge: it freezes the
        // pull target so the producers overtake the cursor again, with zero content
        // loss and zero timeline change. Inactive for VFR/audio-only. The stop drain
        // leaves the state untouched: it pulls at zero latency to the exact endpoint,
        // and clearing the reservoir there would make the retained lead look like drift.
        if (!forceDrain) {
            reservoirTick = pullTick;
            reservoirActive = isCfrRecording;
            elapsedSinceEvalMs = audioIngestReservoirEvalTick == 0
                                                   ? 0
                                                   : static_cast<int64_t>(reservoirTick - audioIngestReservoirEvalTick);
            audioIngestReservoirEvalTick = reservoirTick;
            observedHeadroomSamples =
                audioIngestWorstHeadroomSamples.exchange(kNoAudioIngestHeadroom, std::memory_order_acq_rel);
            headroomObserved = observedHeadroomSamples != kNoAudioIngestHeadroom;
            observedHeadroomMs = headroomObserved ? (observedHeadroomSamples * 1000) / SAMPLE_RATE : 0;
            reservoirDecision = ce::audio::ComputeAudioIngestReservoir(
                audioIngestReservoir, reservoirActive, headroomObserved, observedHeadroomMs, elapsedSinceEvalMs);
            audioIngestReservoir.extraMs = reservoirDecision.extraMs;
            audioIngestReservoir.healthyElapsedMs = reservoirDecision.healthyElapsedMs;
            audioIngestReservoirExtraMs = reservoirDecision.extraMs;
            audioIngestReservoirPeakMs = std::max(audioIngestReservoirPeakMs, reservoirDecision.extraMs);
            reservoirChanged = reservoirDecision.extraMs != audioIngestReservoirLoggedMs;
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
        effectiveAudioPullLatencyMs = kSteadyAudioPullLatencyMs + audioIngestReservoirExtraMs;
        baseTargetLatencySamples = (effectiveAudioPullLatencyMs * SAMPLE_RATE) / 1000;
        // Every track in this pass shares one target, so a per-track hold must request its
        // raise against the same baseline; otherwise N tracks with the same shortfall would
        // stack N raises and overshoot the reservoir.
        reservoirPassBaseMs = audioIngestReservoirExtraMs;
        wallVideoMs = this->videoElapsedMs.load();
        encodedVideoMs = 0;
        audioTargetUs = videoTimelineUs;
        if (videoEnc && !isCfrRecording) {
            encodedVideoUs = videoEnc->GetEncodedDurationUs();
            if (encodedVideoUs > 0) {
                encodedVideoMs = encodedVideoUs / 1000;
                audioTargetUs = encodedVideoUs;
            }
        }
        if (isCfrRecording && videoEnc) {
            encodedVideoUs = videoEnc->GetEncodedDurationUs();
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
            return false;
        }
        audioTargetMs = audioTargetUs / 1000;

        now = std::chrono::steady_clock::now();
        steadyElapsedUs = 0;
        if (this->recordingStartTime.time_since_epoch().count() > 0) {
            steadyElapsedUs =
                std::chrono::duration_cast<std::chrono::microseconds>(now - this->recordingStartTime).count();
        }
        timelineShortfallMs = std::max<int64_t>(0, steadyElapsedUs - audioTargetUs) / 1000;

        videoPipelineLagMs = ce::audio::ComputeVideoPipelineLagMs(wallVideoMs, encodedVideoMs);
        configuredWgcOutputFps =
            (isWgcCfrRecording && config.video.fps > 0) ? static_cast<uint32_t>(config.video.fps) : 0u;
        wgcTargetFps = isWgcCfrRecording ? (configuredWgcOutputFps > 0 ? configuredWgcOutputFps : 1u) : 0u;
        wgcDeliveredFps = 0u;
        wgcDeliveredMin250Fps = 0u;
        wgcDeliveredMin500Fps = 0u;
        wgcBufferedVideoContentLagMs = 0;
        wgcCoverageLossActive = false;
        wgcOverloadFlags = 0u;
        wgcEncoderBottlenecked = false;
        wgcQueueEmptyTickPermille = 0u;
        wgcBufferedAtTickMin = 0u;
        wgcSingleFrameTickCount = 0u;
        wgcSelectionBiasUs = 0;
        if (isWgcCfrRecording && sharedMemLayout) {
            const auto& runtimeState = sharedMemLayout->runtimeState;
            wgcOverloadFlags = runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
            wgcEncoderBottlenecked = runtimeState.encoderBottlenecked.load(std::memory_order_relaxed) != 0;
            telemetryTargetFps = runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
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
        wgcRecordingCadenceFps =
            ce::audio::GetWgcRecordingCadenceFps(configuredWgcOutputFps, wgcTargetFps);
        if (isWgcCfrRecording) {
            wgcCoverageLossActive = ce::audio::HasWgcUnrecoverableCoverageLoss(
                wgcRecordingCadenceFps, videoPipelineLagMs, wgcBufferedVideoContentLagMs, wgcEncoderBottlenecked,
                wgcDeliveredFps);
        }
        wgcAudioLagTargets = ce::audio::ComputeWgcAudioLagTargets(
            videoPipelineLagMs, wgcBufferedVideoContentLagMs, isWgcCfrRecording && wgcCoverageLossActive,
            kWgcCoverageLossMaxBufferedLagMs);
        wgcSteadyStateBufferedAudioLagMs =
            (isWgcCfrRecording && !wgcCoverageLossActive)
                ? ce::audio::ComputeWgcSteadyStateBufferedAudioLagMs(
                      wgcTargetFps, wgcDeliveredFps, wgcDeliveredMin250Fps, wgcDeliveredMin500Fps,
                      wgcEncoderBottlenecked, wgcQueueEmptyTickPermille, wgcBufferedAtTickMin, wgcSingleFrameTickCount)
                : 0;
        effectiveDeliveredFpsForAudioContinuity =
            ce::audio::ComputeEffectiveDeliveredFpsForAudioContinuity(wgcDeliveredFps, wgcDeliveredMin250Fps,
                                                                      wgcDeliveredMin500Fps);
        wgcEncoderOnlyOverload =
            isWgcCfrRecording && ce::audio::ShouldProtectWgcAudioContinuityDuringEncoderOverload(
                                     wgcEncoderBottlenecked, wgcCoverageLossActive, wgcRecordingCadenceFps,
                                     effectiveDeliveredFpsForAudioContinuity, kWgcEncoderHealthyDeliveryMarginFps);
        wgcEncoderShortfallBufferedLagMs = 0;
        if (isWgcCfrRecording && wgcEncoderOnlyOverload && !kWgcPreferVideoRepeatsOverAudioCuts) {
            wgcEncoderShortfallBufferedLagMs =
                std::clamp<int64_t>(timelineShortfallMs, 0, kWgcEncoderShortfallBufferedLagMaxMs);
        }

        // In WGC CFR mode the scheduled audio timeline is authoritative. When repeated
        // video frames are the preferred recovery mechanism, keep audio targets tied to
        // actual buffered-video lag rather than raw wall-clock encoder lag.
        screenGrabDriftLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcCfrDriftLagMs(wgcAudioLagTargets, kWgcPreferVideoRepeatsOverAudioCuts,
                                                     wgcEncoderShortfallBufferedLagMs)
                : 0;
        effectiveSourceClockDriftLagMs = ce::audio::ResolveAudioSourceClockDriftLagMs(
            isCfrRecording, isWgcCfrRecording, screenGrabDriftLagMs, videoPipelineLagMs, timelineShortfallMs);
        wgcSelectedContentLeadMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLeadMs(wgcSelectionBiasUs) : 0;
        wgcSelectedContentLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcSelectedContentLagMs(wgcSelectionBiasUs) : 0;
        wgcVisualContentLagMs =
            isWgcCfrRecording
                ? ce::audio::ComputeWgcVisualContentLagMs(timelineShortfallMs, wgcSelectedContentLeadMs,
                                                          wgcSelectedContentLagMs, kWgcVisualSyncMaxBufferedLagMs)
                : 0;
        screenGrabTargetBufferLagMs =
            isWgcCfrRecording ? ce::audio::ComputeWgcCfrTargetBufferLagMs(
                                    wgcAudioLagTargets, wgcSteadyStateBufferedAudioLagMs,
                                    kWgcPreferVideoRepeatsOverAudioCuts, wgcEncoderShortfallBufferedLagMs)
                              : 0;
        effectiveAudioTargetBufferLagMs = ce::audio::ResolveAudioTargetBufferLagMs(
            isCfrRecording, isWgcCfrRecording, screenGrabTargetBufferLagMs, videoPipelineLagMs);
        targetBufferedLagCapMs =
            isWgcCfrRecording ? std::max<int64_t>(kWgcCoverageLossMaxBufferedLagMs, effectiveAudioTargetBufferLagMs)
                              : kMaxPipelineLagContributionMs;
        maxWgcAudioLeadExcessSamples = 0;

        // Wall-clock audio anchor: when the video timeline has fallen behind real time
        // (shortfall >500ms), use wall-clock elapsed time as the audio pull target instead
        // of the stalled video PTS. Only for non-CFR modes. In CFR mode, video PTS advances
        // at exactly the target framerate regardless of encoder wall-clock speed, so the
        // "shortfall" between wall clock and PTS is expected and harmless. Activating the
        // anchor in CFR mode would pull audio to wall clock, making it advance faster than
        // video PTS and causing unbounded desync.
        wallVideoLagMs = timelineShortfallMs;
        if (ce::audio::ShouldAllowWallClockAudioAnchor(isCfrRecording, forceDrain, wallVideoLagMs)) {
            steadyElapsedMs = steadyElapsedUs / 1000;
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
        cfrTimelineRecoveryActive =
            ce::audio::ShouldSuppressCfrPositiveDriftCorrectionDuringLiveShortfall(
                isCfrRecording, forceDrain, timelineShortfallMs, wgcEncoderBottlenecked);

        if (encodedSamplesPerSource.size() != audioSources.size()) {
            encodedSamplesPerSource.resize(audioSources.size(), 0);
        }

    return true;
}
