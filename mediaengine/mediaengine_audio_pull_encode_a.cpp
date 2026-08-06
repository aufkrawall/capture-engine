#include "mediaengine_internal.h"

bool MediaEngine::PullTrackEncodeSourcesA(AudioPullState& s, int track, const std::vector<size_t>& srcIndices) {
    auto& isCfrRecording = s.isCfrRecording;
    auto& isWgcCfrRecording = s.isWgcCfrRecording;
    auto& baseTargetLatencySamples = s.baseTargetLatencySamples;
    auto& wallVideoMs = s.wallVideoMs;
    auto& timelineShortfallMs = s.timelineShortfallMs;
    auto& videoPipelineLagMs = s.videoPipelineLagMs;
    auto& wgcTargetFps = s.wgcTargetFps;
    auto& wgcDeliveredFps = s.wgcDeliveredFps;
    auto& wgcBufferedVideoContentLagMs = s.wgcBufferedVideoContentLagMs;
    auto& wgcCoverageLossActive = s.wgcCoverageLossActive;
    auto& wgcEncoderBottlenecked = s.wgcEncoderBottlenecked;
    auto& wgcSelectionBiasUs = s.wgcSelectionBiasUs;
    auto& wgcEncoderOnlyOverload = s.wgcEncoderOnlyOverload;
    auto& effectiveSourceClockDriftLagMs = s.effectiveSourceClockDriftLagMs;
    auto& wgcSelectedContentLeadMs = s.wgcSelectedContentLeadMs;
    auto& wgcVisualContentLagMs = s.wgcVisualContentLagMs;
    auto& maxWgcAudioLeadExcessSamples = s.maxWgcAudioLeadExcessSamples;
    auto& wallVideoLagMs = s.wallVideoLagMs;
    auto& cfrTimelineRecoveryActive = s.cfrTimelineRecoveryActive;
    auto& CHANNELS = s.CHANNELS;
    auto& trackStartupSettled = s.trackStartupSettled;
    auto& targetBufferedSamples = s.targetBufferedSamples;
    auto& samplesToEncode = s.samplesToEncode;
    auto& finalStopDrain = s.finalStopDrain;
    auto& totalFloats = s.totalFloats;
    auto& eligibleSources = s.eligibleSources;
    auto& isAppAudioSource = s.isAppAudioSource;
    auto& optionalUnstarted = s.optionalUnstarted;
    auto& appCaptureRouteEnded = s.appCaptureRouteEnded;
    auto& inactiveStartedAppSourceMaySilence = s.inactiveStartedAppSourceMaySilence;
    auto& sparseStartedSourceCanSilence = s.sparseStartedSourceCanSilence;
    auto& sparseStartedSourceMaySilence = s.sparseStartedSourceMaySilence;
    auto& expectedTimelineSilence = s.expectedTimelineSilence;
    auto& droppedFloats = s.droppedFloats;
    auto& retainedFloats = s.retainedFloats;
    auto& remainingStartupProtectionSamples = s.remainingStartupProtectionSamples;
    auto& startupTimelineProtected = s.startupTimelineProtected;
    auto& targetLatencySamples = s.targetLatencySamples;
    auto& MIN_POST_RESAMPLE_FLOATS = s.MIN_POST_RESAMPLE_FLOATS;
    auto& startupProtectedFloats = s.startupProtectedFloats;
    auto& MAX_POST_RESAMPLE_FLOATS = s.MAX_POST_RESAMPLE_FLOATS;
    auto& nowTick = s.nowTick;
    auto& it = s.it;
    auto& overflowDropped = s.overflowDropped;
    auto& categorizedLatencyTrim = s.categorizedLatencyTrim;
    auto& uncategorizedLatencyTrim = s.uncategorizedLatencyTrim;
    auto& forceDrain = s.forceDrain;
    constexpr int SAMPLE_RATE = AudioPullState::SAMPLE_RATE;
    constexpr int64_t kRuntimeMaxLeadSamples = AudioPullState::kRuntimeMaxLeadSamples;
    constexpr int64_t kRuntimeMaxDropPerCall = AudioPullState::kRuntimeMaxDropPerCall;
    constexpr int64_t kRuntimeDropFadeSamples = AudioPullState::kRuntimeDropFadeSamples;
    constexpr int64_t kLatencyTrimHysteresisSamples = AudioPullState::kLatencyTrimHysteresisSamples;
    constexpr int64_t kMinCompensationBufferSamples = AudioPullState::kMinCompensationBufferSamples;
    constexpr int64_t kWgcCfrLeadWarningSamples = AudioPullState::kWgcCfrLeadWarningSamples;
    constexpr bool kWgcPreferVideoRepeatsOverAudioCuts = AudioPullState::kWgcPreferVideoRepeatsOverAudioCuts;
    constexpr double kDefaultMaxCompensationPercent = AudioPullState::kDefaultMaxCompensationPercent;
    constexpr double kTier1MaxPitchPercent = AudioPullState::kTier1MaxPitchPercent;
    constexpr double kAppAudioDrainMaxPitchPercent = AudioPullState::kAppAudioDrainMaxPitchPercent;
    constexpr int64_t kAppAudioDrainSlackSamples = AudioPullState::kAppAudioDrainSlackSamples;
    constexpr int64_t kAppAudioDrainDeadbandSamples = AudioPullState::kAppAudioDrainDeadbandSamples;
    constexpr int64_t kTier2DriftThresholdMs = AudioPullState::kTier2DriftThresholdMs;
    constexpr int64_t kWgcCoverageLossLeadSlackSamples = AudioPullState::kWgcCoverageLossLeadSlackSamples;
    constexpr int64_t kWgcCoverageLossMaxDropPerCall = AudioPullState::kWgcCoverageLossMaxDropPerCall;
    constexpr int64_t kSparseStartedPartialSilenceThresholdSamples = AudioPullState::kSparseStartedPartialSilenceThresholdSamples;

            for (size_t srcIdx : srcIndices) {
                auto& src = audioSources[srcIdx];

                isAppAudioSource = (src.sourceType == AudioConfig::AppAudio);
                optionalUnstarted = ce::audio::IsOptionalUnstartedAppAudioSource(
                    isAppAudioSource, src.timelineValid, src.sawSyncPendingPackets);
                appCaptureRouteEnded = isAppAudioSource && src.appCaptureRouteEnded &&
                                                  src.appCaptureRouteEnded->load(std::memory_order_acquire);
                inactiveStartedAppSourceMaySilence =
                    ce::audio::ShouldTreatInactiveStartedAppCaptureAsSilence(
                        isCfrRecording, isAppAudioSource, src.timelineValid || src.sawSyncPendingPackets,
                        !appCaptureRouteEnded);
                sparseStartedSourceCanSilence = ce::audio::ShouldTreatSparseStartedSourceAsSilence(
                    isCfrRecording, src.timelineValid, src.bootstrapComplete, optionalUnstarted, finalStopDrain);
                sparseStartedSourceMaySilence =
                    ce::audio::ShouldTreatStartedTimelineSourceShortfallAsSilence(
                        sparseStartedSourceCanSilence, GetBufferedTimelineSamples(src), samplesToEncode,
                        kSparseStartedPartialSilenceThresholdSamples) ||
                    inactiveStartedAppSourceMaySilence;
                expectedTimelineSilence =
                    sparseStartedSourceMaySilence ||
                    ce::audio::IsExpectedSourceTimelineSilence(isAppAudioSource, appCaptureRouteEnded,
                                                               src.sourceType == AudioConfig::SystemAudio,
                                                               src.hasAlignedStart);
                if (optionalUnstarted) {
                    continue;
                }
                ++eligibleSources;

                droppedFloats = src.ringBuffer->GetAndClearDroppedSamples();
                if (droppedFloats > 0) {
                    size_t droppedSamples = droppedFloats / CHANNELS;
                    src.overflowDropSamples += droppedSamples;
                    DLL_Log("[PullAudio] WARNING: Ring buffer overflow - %zu samples dropped for src=%d",
                            droppedSamples, (int)srcIdx);
                    src.syncSamplesOutput += (int64_t)droppedSamples;
                }

                retainedFloats = src.ringBuffer->GetAndClearRetainedSamples();
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

                remainingStartupProtectionSamples = std::max<int64_t>(
                    0, static_cast<int64_t>(src.startupGapProtectionSamples) - encodedSamplesPerSource[srcIdx]);
                startupTimelineProtected = remainingStartupProtectionSamples > 0;

                targetLatencySamples = targetBufferedSamples;
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

                            src.pendingLatencyTrimEvents++;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] Audio latency cap: src %d ahead by %lld samples - trimming %lld "
                                    "(capped from %lld, target=%lld, pipelineLag=%lldms)",
                                    (int)srcIdx, (int64_t)rbAvailable - targetLatencySamples, trimmedSamples,
                                    dropSamplesTotal, targetLatencySamples, videoPipelineLagMs);
                            }
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        }
                    } else if (isWgcCfrRecording && !wgcEncoderOnlyOverload && !kWgcPreferVideoRepeatsOverAudioCuts) {
                        const int64_t expectedLeadSamplesForCap = expectedLeadSamplesForCorrection;
                        if (static_cast<int64_t>(rbAvailable) > expectedLeadSamplesForCap + kWgcCfrLeadWarningSamples) {
                            // WGC CFR lead is large.  Log diagnostics and do paced trimming
                            // to prevent unbounded lead growth when the PI controller can't
                            // keep up with source-clock drift.
                            constexpr int64_t kWgcLeadHardCapSamples = SAMPLE_RATE / 2;  // 500ms hard cap
                            const int64_t leadExcess = static_cast<int64_t>(rbAvailable) - expectedLeadSamplesForCap;

                            if (dropLogCounter++ % 500 == 0) {
                                const bool targetCompensationSaturated = src.targetRateSaturated;
                                const int32_t currentCompensationDelta = src.currentRateDelta;
                                const int32_t targetCompensationDelta = src.targetRateDelta;
                                const int32_t maxCompensationDelta = src.syncResampler->GetMaxCompensationDelta();
                                const double currentCompensationPercent = (double)currentCompensationDelta * 100.0 /
                                                                          (static_cast<double>(SAMPLE_RATE) * 10.0);
                                DLL_Log(
                                    "[PullAudio] WGC CFR lead warning: src %d ahead by %lld samples (target=%lld, "
                                    "pipelineLag=%lldms, encBottleneck=%d). Tier1 drift correction active "
                                    "(corr=%d/%d per 10s target=%d, %.4f%%, sat=%d, maxBudget=%.2f%%)%s",
                                    (int)srcIdx, leadExcess, expectedLeadSamplesForCap, videoPipelineLagMs,
                                    wgcEncoderBottlenecked ? 1 : 0, currentCompensationDelta, maxCompensationDelta,
                                    targetCompensationDelta, currentCompensationPercent,
                                    targetCompensationSaturated ? 1 : 0, maxCompensationPercent,
                                    targetCompensationSaturated ? " - source clock mismatch exceeds tier1 budget" : "");
                            }

                            // Paced lead trimming when lead exceeds hard cap (500ms).
                            // Prevents unbounded growth while keeping fades smooth.
                            if (!forceDrain && leadExcess > kWgcLeadHardCapSamples && src.ringBuffer) {
                                const int64_t excessAboveCap = leadExcess - kWgcLeadHardCapSamples;
                                const int64_t maxTrimThisCall =
                                    std::min(excessAboveCap, kWgcCoverageLossMaxDropPerCall);
                                if (maxTrimThisCall > 0) {
                                    CaptureDropFadeAnchor(src, CHANNELS);
                                    src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                                    size_t trimmedFloats = src.ringBuffer->Skip((size_t)maxTrimThisCall * CHANNELS);
                                    size_t trimmedSamples = trimmedFloats / CHANNELS;
                                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples,
                                                                               trimmedSamples);
                                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples,
                                                                               trimmedSamples);
                                    src.coverageLossTrimSamples += trimmedSamples;
                                    src.pendingCoverageLossTrimSamples += trimmedSamples;
                                    src.pendingCoverageLossTrimEvents++;
                                    src.latencyTrimSamples += trimmedSamples;
                                    src.pendingLatencyTrimSamples += trimmedSamples;
                                    src.pendingLatencyTrimEvents++;
                                    if (dropLogCounter++ % 100 == 0) {
                                        DLL_Log(
                                            "[PullAudio] WGC CFR lead cap trim: src %d lead=%lld (cap=%lld) - "
                                            "trimmed %lld samples",
                                            (int)srcIdx, leadExcess, kWgcLeadHardCapSamples, (long long)trimmedSamples);
                                    }
                                    rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                                }
                            }
                        }
                    }
                    if (isWgcCfrRecording) {
                        const int64_t expectedLeadForMax = expectedLeadSamplesForCorrection;
                        const int64_t audioLeadExcessSamples =
                            std::max<int64_t>(0, static_cast<int64_t>(rbAvailable) - expectedLeadForMax);
                        maxWgcAudioLeadExcessSamples = std::max<uint32_t>(
                            maxWgcAudioLeadExcessSamples,
                            static_cast<uint32_t>(std::min<int64_t>(audioLeadExcessSamples, INT32_MAX)));
                    }

                    // Non-CFR modes may still use legacy drift correction. CFR
                    // allows only tiny source-clock correction after startup. CFR app-audio backlog drain is
                    // the explicit exception: it pulls process-loopback content back toward the live target.
                    {
                        const int64_t rbLevel = static_cast<int64_t>(rbAvailable);
                        const int64_t compensationBufferedSamples = rbLevel;
                        if (src.sourceType == AudioConfig::AppAudio && src.captureFanoutOwnerIndex == srcIdx) {
                            const auto [groupMinSamples, groupMaxSamples] = GetCaptureGroupBufferedSampleRange(srcIdx);
                            constexpr int64_t kCaptureGroupDivergenceWarnSamples = SAMPLE_RATE / 20;
                            const int64_t groupSpreadSamples = groupMaxSamples - groupMinSamples;
                            const uint64_t nowGroupTick = GetTickCount64();
                            if (groupSpreadSamples >= kCaptureGroupDivergenceWarnSamples &&
                                nowGroupTick - src.lastCaptureGroupDivergenceWarnTick >= 5000) {
                                ProcessLoopbackCapture* routedCapture = GetAppCaptureForRoute(srcIdx);
                                DLL_Log(
                                    "[AudioRoute] WARNING: shared app capture route backlog diverged owner=%zu "
                                    "process=%s min=%lld max=%lld spread=%lld (%.1fms) captureActive=%d. "
                                    "Applying route-local compensation so a delayed sibling cannot starve this "
                                    "route.",
                                    srcIdx, src.config.processName.empty() ? "<none>" : src.config.processName.c_str(),
                                    static_cast<long long>(groupMinSamples), static_cast<long long>(groupMaxSamples),
                                    static_cast<long long>(groupSpreadSamples),
                                    static_cast<double>(groupSpreadSamples) * 1000.0 / SAMPLE_RATE,
                                    routedCapture && routedCapture->IsCapturing() ? 1 : 0);
                                src.lastCaptureGroupDivergenceWarnTick = nowGroupTick;
                            }
                        }
                        if (compensationBufferedSamples >= kMinCompensationBufferSamples) {
                            const int64_t expectedLead = expectedLeadSamplesForCorrection;
                            const int64_t trueDrift = compensationBufferedSamples - expectedLead;
                            const auto appAudioDrainDecision = ce::audio::ComputeCfrAppAudioBacklogDrainDecision(
                                isCfrRecording, src.sourceType == AudioConfig::AppAudio, forceDrain,
                                trackStartupSettled, startupTimelineProtected, cfrTimelineRecoveryActive,
                                compensationBufferedSamples, expectedLead, kMinCompensationBufferSamples,
                                static_cast<int64_t>(SAMPLE_RATE) * 10, kAppAudioDrainMaxPitchPercent,
                                kAppAudioDrainSlackSamples, kAppAudioDrainDeadbandSamples);
                            const double activeMaxCompensationPercent =
                                appAudioDrainDecision.active
                                    ? kAppAudioDrainMaxPitchPercent
                                    : (isCfrRecording ? kTier1MaxPitchPercent : kDefaultMaxCompensationPercent);
                            src.syncResampler->SetMaxCompensationPercent(activeMaxCompensationPercent);

                            if (src.sourceType == AudioConfig::AppAudio) {
                                const uint32_t newReason = static_cast<uint32_t>(appAudioDrainDecision.reason);
                                const bool stateChanged =
                                    !src.appAudioBacklogDrainInitialized ||
                                    src.appAudioBacklogDrainActive != appAudioDrainDecision.active ||
                                    src.appAudioBacklogDrainReason != newReason;
                                src.appAudioBacklogTargetSamples = appAudioDrainDecision.targetLeadSamples;
                                src.appAudioBacklogExcessSamples = appAudioDrainDecision.excessSamples;
                                src.appAudioBacklogCompensationDelta = appAudioDrainDecision.compensationDelta;
                                if (stateChanged) {
                                    if (src.appAudioBacklogDrainInitialized || appAudioDrainDecision.active) {
                                        const double compPct =
                                            static_cast<double>(appAudioDrainDecision.compensationDelta) * 100.0 /
                                            (static_cast<double>(SAMPLE_RATE) * 10.0);
                                        DLL_Log(
                                            "[AppDrain] state src=%zu track=%d active=%d reason=%s delayMs=%lld "
                                            "targetMs=%lld excessMs=%lld rb=%lld target=%lld delta=%d comp=%.4f%% "
                                            "forceDrain=%d startupSettled=%d startupProtected=%d",
                                            srcIdx, src.track, appAudioDrainDecision.active ? 1 : 0,
                                            ce::audio::CfrAppAudioBacklogDrainReasonName(appAudioDrainDecision.reason),
                                            (long long)(appAudioDrainDecision.backlogSamples * 1000 / SAMPLE_RATE),
                                            (long long)(appAudioDrainDecision.targetLeadSamples * 1000 / SAMPLE_RATE),
                                            (long long)(appAudioDrainDecision.excessSamples * 1000 / SAMPLE_RATE),
                                            (long long)appAudioDrainDecision.backlogSamples,
                                            (long long)appAudioDrainDecision.targetLeadSamples,
                                            appAudioDrainDecision.compensationDelta, compPct, forceDrain ? 1 : 0,
                                            trackStartupSettled ? 1 : 0, startupTimelineProtected ? 1 : 0);
                                    }
                                    if (src.appAudioBacklogDrainInitialized) {
                                        src.appLatencyDrainTransitions++;
                                    }
                                }
                                src.appAudioBacklogDrainInitialized = true;
                                src.appAudioBacklogDrainActive = appAudioDrainDecision.active;
                                src.appAudioBacklogDrainReason = newReason;
                            }

                            // Drift sanity check: detect extreme drift that indicates measurement error
                            if (std::abs(trueDrift) > SAMPLE_RATE * 2) {  // >2 seconds
                                const uint64_t nowWarnTick = GetTickCount64();
                                if (nowWarnTick - src.lastExtremeDriftWarnTick >= 1000) {
                                    if (forceDrain) {
                                        DLL_Log(
                                            "[PullAudio] Stop force-drain backlog: drift=%lld samples src=%d "
                                            "forceDrain=1 (post-target backlog is excluded from output)",
                                            trueDrift, (int)srcIdx);
                                    } else {
                                        DLL_Log(
                                            "[PullAudio] WARNING: Extreme drift detected (%lld samples src=%d) "
                                            "forceDrain=0 - may indicate sync issue",
                                            trueDrift, (int)srcIdx);
                                    }
                                    src.lastExtremeDriftWarnTick = nowWarnTick;
                                }
                            }

                            const int64_t nowVideoMs = (encodedSamplesPerSource[srcIdx] * 1000) / SAMPLE_RATE;

                            // Debug logging for drift calculation (periodic)
                            if (driftLogCounter++ % 2000 == 0) {
                                DLL_Log(
                                    "[PullAudio] Drift debug src=%d: syncOutput=%lld encoded=%lld nowVideo=%lld "
                                    "wallVideo=%lld trueDrift=%lld rbLevel=%lld",
                                    (int)srcIdx, src.syncSamplesOutput, encodedSamplesPerSource[srcIdx], nowVideoMs,
                                    wallVideoMs, trueDrift, rbLevel);
                            }

                            if (src.lastRateUpdateMs <= 0) {
                                src.lastRateUpdateMs = nowVideoMs;
                            } else {
                                const int64_t updateElapsed = nowVideoMs - src.lastRateUpdateMs;

                                if (updateElapsed >= 500) {
                                    // --- Tier 1: bounded source-clock correction ---
                                    // Keep a tiny correction lane for real device clock drift. It must not spend
                                    // buffered lead that only exists because the CFR video timeline is behind wall
                                    // clock or because WGC/source coverage is missing.
                                    int32_t tier1Delta = 0;

                                    if (isCfrRecording) {
                                        if (appAudioDrainDecision.active) {
                                            tier1Delta = appAudioDrainDecision.compensationDelta;
                                            const int32_t maxCompensationDelta =
                                                src.syncResampler->GetMaxCompensationDelta();
                                            src.targetRateSaturated =
                                                tier1Delta != 0 && std::abs(tier1Delta) >= maxCompensationDelta &&
                                                appAudioDrainDecision.excessSamples > maxCompensationDelta;
                                        } else {
                                            const bool allowCfrSourceClockCorrection =
                                                ce::audio::ShouldAllowCfrSourceClockDriftCompensation(
                                                    isCfrRecording, forceDrain, trackStartupSettled,
                                                    startupTimelineProtected, wgcEncoderBottlenecked,
                                                    timelineShortfallMs, wgcCoverageLossActive);
                                            if (allowCfrSourceClockCorrection) {
                                                tier1Delta = ce::audio::ComputeTier1CompensationDeltaWithDeadband(
                                                    trueDrift, static_cast<int64_t>(SAMPLE_RATE) * 10,
                                                    kTier1MaxPitchPercent, SAMPLE_RATE / 12);
                                                const int32_t maxCompensationDelta =
                                                    src.syncResampler->GetMaxCompensationDelta();
                                                src.targetRateSaturated =
                                                    tier1Delta != 0 && std::abs(tier1Delta) >= maxCompensationDelta &&
                                                    std::abs(trueDrift) > maxCompensationDelta;
                                                if (isWgcCfrRecording && tier1Delta > 0) {
                                                    const int64_t positiveCompensationHysteresisSamples =
                                                        ce::audio::ComputeWgcPositiveCompensationHysteresisSamples(
                                                            targetLatencySamples, kWgcCfrLeadWarningSamples);
                                                    const bool allowSteadyStatePositiveCompensation =
                                                        ce::audio::ShouldAllowWgcSteadyStateDriftCompensation(
                                                            trackStartupSettled, videoPipelineLagMs, rbLevel,
                                                            targetLatencySamples, kWgcCfrLeadWarningSamples);
                                                    if (ce::audio::ShouldClearWgcPositiveDriftCompensation(
                                                            allowSteadyStatePositiveCompensation, rbLevel, expectedLead,
                                                            positiveCompensationHysteresisSamples)) {
                                                        tier1Delta = 0;
                                                    } else {
                                                        tier1Delta = static_cast<int32_t>(
                                                            ce::audio::ClampWgcPositiveDriftCorrection(
                                                                tier1Delta, positiveCompensationHysteresisSamples));
                                                    }
                                                }
                                            } else {
                                                src.targetRateSaturated = false;
                                            }
                                        }
                                    } else {
                                        const int32_t maxDelta = src.syncResampler->GetMaxCompensationDelta();
                                        tier1Delta =
                                            static_cast<int32_t>(std::clamp(trueDrift, static_cast<int64_t>(-maxDelta),
                                                                            static_cast<int64_t>(maxDelta)));
                                        src.targetRateSaturated = false;
                                    }
                                    src.targetRateDelta = tier1Delta;

                                    constexpr int32_t kMaxRateChange = 1500;
                                    int32_t newDelta = static_cast<int32_t>(
                                        std::clamp(static_cast<int64_t>(tier1Delta),
                                                   static_cast<int64_t>(src.currentRateDelta) - kMaxRateChange,
                                                   static_cast<int64_t>(src.currentRateDelta) + kMaxRateChange));

                                    src.currentRateDelta = newDelta;
                                    if (src.sourceType == AudioConfig::AppAudio) {
                                        src.appLatencyMaxAbsCompDelta =
                                            std::max<uint32_t>(src.appLatencyMaxAbsCompDelta,
                                                               static_cast<uint32_t>(std::abs(src.currentRateDelta)));
                                        src.appAudioBacklogCompensationDelta = newDelta;
                                    }

                                    if (newDelta != 0 || src.rateCompActive) {
                                        int ret = swr_set_compensation(src.syncResampler->GetSwrContext(), -newDelta,
                                                                       SAMPLE_RATE * 10);
                                        if (ret >= 0) {
                                            src.rateCompActive = (newDelta != 0);
                                        }
                                    }

                                    // --- Tier 2: Ring buffer trim with crossfade ---
                                    // Activates when drift exceeds what Tier 1 can handle alone.
                                    // Normally suppressed in WGC CFR mode (prefer video repeats over
                                    // audio cuts), and enabled for non-CFR modes when the wall-clock
                                    // audio anchor is active (encoder severely stalled).
                                    const bool wallClockAnchorActive = ce::audio::ShouldAllowWallClockAudioAnchor(
                                        isCfrRecording, forceDrain, wallVideoLagMs);
                                    if (isWgcCfrRecording && !wgcEncoderOnlyOverload && !startupTimelineProtected &&
                                        (!kWgcPreferVideoRepeatsOverAudioCuts || wallClockAnchorActive) &&
                                        ce::audio::ShouldActivateTier2Trim(trueDrift, SAMPLE_RATE,
                                                                           kTier2DriftThresholdMs) &&
                                        src.ringBuffer) {
                                        const int64_t tier2TrimBudget = ce::audio::ComputeTier2TrimBudget(
                                            trueDrift, SAMPLE_RATE, kRuntimeMaxDropPerCall);
                                        const int64_t tier2MaxTrim = std::min(tier2TrimBudget, std::abs(trueDrift));
                                        if (tier2MaxTrim > 0 && static_cast<int64_t>(rbAvailable) >
                                                                    targetLatencySamples + kRuntimeDropFadeSamples) {
                                            CaptureDropFadeAnchor(src, CHANNELS);
                                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                                            size_t trimmedFloats =
                                                src.ringBuffer->Skip((size_t)tier2MaxTrim * CHANNELS);
                                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples,
                                                                                       trimmedSamples);
                                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples,
                                                                                       trimmedSamples);
                                            src.latencyTrimSamples += trimmedSamples;
                                            src.tier2TrimSamples += trimmedSamples;
                                            src.pendingLatencyTrimSamples += trimmedSamples;
                                            src.pendingLatencyTrimEvents++;
                                            src.pendingTier2TrimSamples += trimmedSamples;
                                            src.pendingTier2TrimEvents++;
                                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                                        }
                                    }

                                    if (driftLogCounter++ % 500 == 0) {
                                        const double compensationPercent = (double)src.currentRateDelta * 100.0 /
                                                                           (static_cast<double>(SAMPLE_RATE) * 10.0);
                                        const bool tier2WouldActivate = ce::audio::ShouldActivateTier2Trim(
                                            trueDrift, SAMPLE_RATE, kTier2DriftThresholdMs);
                                        // Whether the tier2 trim path is actually permitted to run this pull. In WGC
                                        // CFR it is intentionally suppressed (prefer video repeats over audio cuts),
                                        // so the common "tier2=1 tier2Applied=0" pair means the drift threshold is
                                        // exceeded but the standing buffer is deliberately PRESERVED, not trimmed -
                                        // benign extra latency that stays out of the encoded timeline (content is
                                        // placed by packet QPC), NOT an audio cut. Logged distinctly so the standing
                                        // startup backlog is not misread as an active trim/convergence failure.
                                        const bool tier2TrimEnabled =
                                            isWgcCfrRecording && !wgcEncoderOnlyOverload && !startupTimelineProtected &&
                                            (!kWgcPreferVideoRepeatsOverAudioCuts || wallClockAnchorActive);
                                        DLL_Log(
                                            "[PullAudio] Src %zu drift: "
                                            "trueDrift=%lld "
                                            "(rb=%lld expected=%lld "
                                            "pipelineLag=%lldms) "
                                            "tier1=%d (%.4f%%) "
                                            "tier2=%d tier2Applied=%d encBottleneck=%d",
                                            srcIdx, trueDrift, rbLevel, expectedLead, effectiveSourceClockDriftLagMs,
                                            newDelta, compensationPercent, tier2WouldActivate ? 1 : 0,
                                            (tier2WouldActivate && tier2TrimEnabled) ? 1 : 0,
                                            wgcEncoderBottlenecked ? 1 : 0);
                                    }

                                    src.lastRateUpdateMs = nowVideoMs;
                                }
                            }
                        }
                    }

                    // Overflow protection. Non-CFR modes keep the historical short cap. CFR logs pressure instead
                    // of proactively trimming; if the ring actually overflows, validation must fail the recording.
                    if (!forceDrain && src.ringBuffer && !startupTimelineProtected) {
                        constexpr int64_t kMaxOverflowSamples = SAMPLE_RATE / 2;            // 500ms max overflow
                        constexpr int64_t kWgcCfrEmergencyRingMarginSamples = SAMPLE_RATE;  // 1s before full
                        rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                        const int64_t rbCapacitySamples =
                            static_cast<int64_t>(src.ringBuffer->GetCapacity() / CHANNELS);

                        const int64_t overflowCapSamples = ce::audio::ComputeRuntimeOverflowCapSamples(
                            isCfrRecording, targetLatencySamples, rbCapacitySamples, kMaxOverflowSamples,
                            kWgcCfrEmergencyRingMarginSamples);
                        const int64_t overflowExcess =
                            static_cast<int64_t>(rbAvailable) - targetLatencySamples - overflowCapSamples;
                        if (overflowExcess > kRuntimeDropFadeSamples && !isCfrRecording) {
                            CaptureDropFadeAnchor(src, CHANNELS);
                            src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                            size_t trimmedFloats = src.ringBuffer->Skip((size_t)overflowExcess * CHANNELS);
                            size_t trimmedSamples = trimmedFloats / CHANNELS;
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticRingSamples, trimmedSamples);
                            ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, trimmedSamples);
                            src.latencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimSamples += trimmedSamples;
                            src.pendingLatencyTrimEvents++;
                            rbAvailable = src.ringBuffer->GetAvailable() / CHANNELS;
                            if (dropLogCounter++ % 500 == 0) {
                                DLL_Log(
                                    "[PullAudio] Ring buffer overflow protection%s: src %d trimmed %lld samples "
                                    "(rb was %lld samples, target %lld, overflow cap %lld)",
                                    isCfrRecording ? " (CFR emergency capacity guard)" : "", (int)srcIdx,
                                    (long long)trimmedSamples, (long long)(rbAvailable + trimmedSamples),
                                    (long long)targetLatencySamples, (long long)overflowCapSamples);
                            }
                        } else if (overflowExcess > kRuntimeDropFadeSamples && dropLogCounter++ % 500 == 0) {
                            DLL_Log(
                                "[PullAudio] WARNING: CFR audio ring near capacity - src %d excess=%lld "
                                "rb=%lld target=%lld cap=%lld capacity=%lld. Preserving audio; any overflow/drop is "
                                "a validation failure.",
                                (int)srcIdx, (long long)overflowExcess, (long long)rbAvailable,
                                (long long)targetLatencySamples, (long long)overflowCapSamples,
                                (long long)rbCapacitySamples);
                        }
                    }
                }

                if (src.syncResampler && src.syncResampler->IsReady()) {
                    const size_t MAX_CHUNK_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 10);
                    while (src.postResampleBuffer.size() < totalFloats) {
                        size_t rbFloats = src.ringBuffer->GetAvailable();
                        if (rbFloats == 0) {
                            if (!expectedTimelineSilence) {
                                src.ringBufferUnderrunCount++;
                            }
                            break;
                        }
                        size_t rbSamples = rbFloats / CHANNELS;
                        if (rbSamples > src.ringBufferPeakSamples) {
                            src.ringBufferPeakSamples = rbSamples;
                        }

                        size_t needFloats = totalFloats - src.postResampleBuffer.size();
                        size_t chunkFloats = std::min(rbFloats, needFloats);
                        chunkFloats = std::min(chunkFloats, MAX_CHUNK_FLOATS);
                        chunkFloats -= (chunkFloats % CHANNELS);
                        if (chunkFloats == 0) {
                            break;
                        }

                        std::vector<float> rbData(chunkFloats);
                        size_t actualRead = src.ringBuffer->Read(rbData.data(), chunkFloats);
                        if (actualRead == 0) {
                            break;
                        }

                        size_t actualReadSamples = actualRead / CHANNELS;
                        uint64_t syntheticReadSamples = ce::audio::ConsumeSyntheticBufferedSamples(
                            src.startupSyntheticRingSamples, actualReadSamples);
                        src.startupSyntheticResamplerSamples += syntheticReadSamples;

                        uint8_t** resampledData = nullptr;
                        int outSamples = 0;
                        if (src.syncResampler->Process((uint8_t*)rbData.data(), (int)(actualRead * sizeof(float)),
                                                       &resampledData, &outSamples)) {
                            uint64_t syntheticPostSamples = ce::audio::ConsumeSyntheticBufferedSamples(
                                src.startupSyntheticResamplerSamples, (uint64_t)std::max(outSamples, 0));
                            src.startupSyntheticPostSamples += syntheticPostSamples;
                            if (outSamples > 0 && resampledData && resampledData[0]) {
                                float* outFloats = (float*)resampledData[0];
                                if (src.dropFadeSamplesRemaining > 0) {
                                    const int kDropFadeSamples = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    int blendSamples = std::min(src.dropFadeSamplesRemaining, outSamples);
                                    int blendStart = kDropFadeSamples - src.dropFadeSamplesRemaining;
                                    for (int s = 0; s < blendSamples; s++) {
                                        float alpha = (float)(blendStart + s + 1) / kDropFadeSamples;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            const size_t idx = static_cast<size_t>(s) * CHANNELS + ch;
                                            const float anchor = GetDropFadeAnchor(src, ch);
                                            outFloats[idx] = anchor + (outFloats[idx] - anchor) * alpha;
                                        }
                                    }
                                    if (blendSamples > 0) {
                                        src.dropFadeStart.assign(static_cast<size_t>(CHANNELS), 0.0f);
                                        const size_t base = static_cast<size_t>(blendSamples - 1) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            src.dropFadeStart[static_cast<size_t>(ch)] = outFloats[base + ch];
                                        }
                                        src.dropFadeStartL = src.dropFadeStart[0];
                                        src.dropFadeStartR = CHANNELS > 1 ? src.dropFadeStart[1] : src.dropFadeStart[0];
                                    }
                                    src.dropFadeSamplesRemaining -= blendSamples;
                                }
                                if (src.packetBoundaryFadeInSamplesRemaining > 0) {
                                    const int blendSamples =
                                        std::min(src.packetBoundaryFadeInSamplesRemaining, outSamples);
                                    for (int s = 0; s < blendSamples; ++s) {
                                        const float alpha = ComputeRaisedCosineFade(
                                            static_cast<size_t>(s),
                                            static_cast<size_t>(std::max(src.packetBoundaryFadeInSamplesRemaining, 1)));
                                        const size_t base = static_cast<size_t>(s) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            outFloats[base + ch] *= alpha;
                                        }
                                    }
                                    src.packetBoundaryFadeInSamplesRemaining -= blendSamples;
                                }
                                if (src.pendingUnderrunRecoveryFade) {
                                    src.underrunFadeSamplesRemaining = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    src.pendingUnderrunRecoveryFade = false;
                                }
                                if (src.underrunFadeSamplesRemaining > 0) {
                                    const int kUnderrunFadeSamples = SAMPLE_RATE / 40;  // 25ms - smoother transitions
                                    int blendSamples = std::min(src.underrunFadeSamplesRemaining, outSamples);
                                    int blendStart = kUnderrunFadeSamples - src.underrunFadeSamplesRemaining;
                                    for (int s = 0; s < blendSamples; s++) {
                                        float alpha = (float)(blendStart + s + 1) / kUnderrunFadeSamples;
                                        const size_t base = static_cast<size_t>(s) * CHANNELS;
                                        for (int ch = 0; ch < CHANNELS; ++ch) {
                                            outFloats[base + ch] *= alpha;
                                        }
                                    }
                                    src.underrunFadeSamplesRemaining -= blendSamples;
                                }
                                int numFloats = outSamples * CHANNELS;
                                src.postResampleBuffer.insert(src.postResampleBuffer.end(), outFloats,
                                                              outFloats + numFloats);
                                src.syncSamplesOutput += outSamples;

                                // Log sample trim stats periodically
                                if (dropLogCounter++ % 500 == 0 &&
                                    (src.overflowDropSamples > 0 || src.latencyTrimSamples > 0 ||
                                     src.postResampleTrimSamples > 0)) {
                                    const uint64_t categorizedLatencyTrim =
                                        std::min(src.latencyTrimSamples,
                                                 src.bootstrapTrimSamples + src.retainedNewestTrimSamples +
                                                     src.coverageLossTrimSamples + src.tier2TrimSamples);
                                    const uint64_t uncategorizedLatencyTrim =
                                        src.latencyTrimSamples - categorizedLatencyTrim;
                                    DLL_Log(
                                        "[PullAudio] Sample trim stats src=%d: overflowDropped=%llu "
                                        "latencyTrimTotal=%llu bootstrapTrim=%llu retainedTrim=%llu "
                                        "coverageTrim=%llu tier2Trim=%llu uncategorizedLiveTrim=%llu "
                                        "postResampleTrim=%llu",
                                        (int)srcIdx, (unsigned long long)src.overflowDropSamples,
                                        (unsigned long long)src.latencyTrimSamples,
                                        (unsigned long long)src.bootstrapTrimSamples,
                                        (unsigned long long)src.retainedNewestTrimSamples,
                                        (unsigned long long)src.coverageLossTrimSamples,
                                        (unsigned long long)src.tier2TrimSamples,
                                        (unsigned long long)uncategorizedLatencyTrim,
                                        (unsigned long long)src.postResampleTrimSamples);
                                }
                            }
                        }
                        AudioResampler::FreeOutputBuffer(resampledData);
                    }
                }

                MIN_POST_RESAMPLE_FLOATS = (size_t)(SAMPLE_RATE * CHANNELS / 20);
                startupProtectedFloats =
                    static_cast<size_t>(std::max<int64_t>(0, remainingStartupProtectionSamples)) * CHANNELS;
                MAX_POST_RESAMPLE_FLOATS =
                    std::max(totalFloats * 4, MIN_POST_RESAMPLE_FLOATS + startupProtectedFloats);
                if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS && !isCfrRecording) {
                    size_t excess = src.postResampleBuffer.size() - MAX_POST_RESAMPLE_FLOATS;
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupSyntheticPostSamples, excess / CHANNELS);
                    ce::audio::ConsumeSyntheticBufferedSamples(src.startupGapProtectionSamples, excess / CHANNELS);
                    src.postResampleTrimSamples += excess / CHANNELS;

                    CaptureDropFadeAnchor(src, CHANNELS);
                    src.dropFadeSamplesRemaining = (int)kRuntimeDropFadeSamples;

                    if (dropLogCounter++ % 100 == 0) {
                        DLL_Log(
                            "[PullAudio] WARNING: Post-resample buffer trim - src %d dropping %zu samples (buffer=%zu "
                            "cap=%zu)",
                            (int)srcIdx, excess / CHANNELS, src.postResampleBuffer.size() / CHANNELS,
                            MAX_POST_RESAMPLE_FLOATS / CHANNELS);
                    }
                    src.postResampleBuffer.erase(src.postResampleBuffer.begin(),
                                                 src.postResampleBuffer.begin() + (std::ptrdiff_t)excess);
                } else if (src.postResampleBuffer.size() > MAX_POST_RESAMPLE_FLOATS && dropLogCounter++ % 500 == 0) {
                    DLL_Log(
                        "[PullAudio] WARNING: CFR post-resample backlog exceeded guard - src %d backlog=%zu cap=%zu. "
                        "Preserving audio; underrun/overflow validation will report any real failure.",
                        (int)srcIdx, src.postResampleBuffer.size() / CHANNELS, MAX_POST_RESAMPLE_FLOATS / CHANNELS);
                }

                if (!PullTrackEncodeSourcesB(s, track, srcIdx))
                    continue;
                if (!PullTrackEncodeSourcesC1(s, track, srcIdx))
                    continue;
                if (!PullTrackEncodeSourcesC2(s, track, srcIdx))
                    continue;
            }
    return true;
}
