#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopEmit() {
        consumesCfrTick =
            !config.video.useVFR && ((media_main_g_EncoderRunning && media_main_g_Recording) || drainingOutstandingLiveTicks);
        isDrainPhase = !media_main_g_Recording.load(std::memory_order_acquire);
        isLivePhase =
            recordingOutputLive && (media_main_g_Recording.load(std::memory_order_acquire) || drainingOutstandingLiveTicks);
        scheduledLiveCfrTick = consumesCfrTick && isLivePhase;
        if (scheduledLiveCfrTick) {
            encoderGridTickCount = selectionGridTick;
            outputShortfallTicks = ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);
            ++wgcQueueTickSampleCount;
            if (useScreenGrab) {
                const uint32_t bufferedAtTick =
                    wgcTelemetryTickArmed ? wgcBufferedAtTickStart : static_cast<uint32_t>(bufferedWgcFrames.size());
                wgcBufferedAtTickSum += bufferedAtTick;
                wgcBufferedAtTickMin = std::min(wgcBufferedAtTickMin, bufferedAtTick);
                if (wgcTelemetryTickArmed && !wgcFreshAvailableAtTickStart) {
                    ++wgcNoFreshTickCount;
                }
                if (wgcTelemetryTickArmed && !wgcReserveAvailableAtTickStart) {
                    ++wgcNoReserveTickCount;
                    if (isWgcEffectiveContentDelayActive()) {
                        ++wgcDelayReservoirLowWaterTickCount;
                        ++wgcDelayReservoirLowWaterTickTotal;
                    }
                }
            }
            wgcNoFreshTickPermille = wgcQueueTickSampleCount > 0
                                         ? SaturatingToUint32((static_cast<uint64_t>(wgcNoFreshTickCount) * 1000ull) /
                                                              static_cast<uint64_t>(wgcQueueTickSampleCount))
                                         : 0u;
        }
        if (scheduledLiveCfrTick && !useScreenGrab) {
            // Timer rebases keep the worker wake cadence near wall time, while liveTicksOutput owns the
            // immutable CFR media grid. Keeping those clocks separate lets inject recovery submit an
            // overdue extra slot without postponing the next normal 120 Hz wake by another tick.
            scheduledOutputQpc = ce::capture_policy::GetNextInjectCfrOutputQpc(
                liveStartQpc.QuadPart, liveTicksOutput, targetIntervalTicks, scheduledSampleQpc);
        }


        if (!frameToProcess && useScreenGrab && config.video.useVFR && recordingOutputLive &&
            privacyRuntime.IsEnabled()) {
            const auto privacyDecision = evaluateScreenGrabPrivacy(nullptr);
            if (privacyDecision.useBlackFrame && media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
                LARGE_INTEGER privacyVfrQpc = {};
                QueryPerformanceCounter(&privacyVfrQpc);
                const bool blackSucceeded =
                    submitPrivacyBlackFrame(media_main_g_LastFrame, privacyVfrQpc.QuadPart, privacyVfrQpc.QuadPart, -1);
                if (blackSucceeded && media_main_g_pSharedMem) {
                    media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }
            continueMainLoop = true;
            return;
            }
        }

        if ((!frameToProcess || wantsTrueRepeatLastFrame) && scheduledLiveCfrTick && hasRepeatLastFramePath) {
            LARGE_INTEGER repeatStartEnc, repeatEndEnc;
            QueryPerformanceCounter(&repeatStartEnc);
            const bool duplicateFromDrain = isDrainPhase;
            bool repeatDuplicateFromDeferred = false;
            const bool repeatDuplicateFromTimerRebase = encoderLateTickCount >= 2;
            const InjectFrameLineage duplicateLineage =
                !useScreenGrab && lastSuccessfullyEncodedInjectLineage.IsValid()
                    ? lastSuccessfullyEncodedInjectLineage
                    : (media_main_g_HasLastFrame ? MakeInjectFrameLineage(media_main_g_LastFrame) : InjectFrameLineage{});
            bool encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledOutputQpc);
            bool encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
            QueryPerformanceCounter(&repeatEndEnc);

            if (encodeSucceeded && !encodeDeferred) {
                double currentEncodeMs =
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    (double)(repeatEndEnc.QuadPart - repeatStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                if (smoothedEncodeMs == 0.0) {
                    smoothedEncodeMs = pureEncodeMs;
                } else {
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - media_main_kEncodeEmaAlpha) + pureEncodeMs * media_main_kEncodeEmaAlpha;
                }
                if (useScreenGrab) {
                    ce::capture_policy::UpdateWgcServiceTimeEma(
                        currentEncodeMs, pureEncodeMs, media_main_kEncodeEmaAlpha, smoothedWgcRepeatServiceMs,
                        wgcRepeatServiceSamples);
                } else {
                    observeInjectRepeatService(currentEncodeMs, pureEncodeMs);
                    if (injectProactiveOverloadRepeatThisTick) {
                        ++injectOverloadRepeatRuntime.pacer.emittedRepeats;
                    }
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));
                if (media_main_g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    media_main_g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                } else if (media_main_g_pSharedMem) {
                    media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (useScreenGrab) {
                    if (wgcProactiveOverloadRepeatThisTick) {
                        ++wgcOverloadRepeatPacer.emittedRepeats;
                    }
                    if (repeatDuplicateFromTimerRebase) {
                        ++wgcRepeatTimerLateCount;
                    } else if ((!frameToProcess && !wantsTrueRepeatLastFrame) ||
                               (wantsTrueRepeatLastFrame && !wgcProactiveOverloadRepeatThisTick)) {
                        ++wgcRepeatNoFreshCount;
                    }
                }
                recordDuplicate(nullptr, duplicateLineage.IsValid() ? &duplicateLineage : nullptr, duplicateFromDrain,
                                repeatDuplicateFromDeferred, repeatDuplicateFromTimerRebase, false,
                                wgcProactiveOverloadRepeatThisTick || injectProactiveOverloadRepeatThisTick);
                cadenceCounters.liveTickEmitCount++;
                cadenceCounters.liveTickDuplicateCount++;
                cadenceCounters.holdTicksRunning++;
                ++liveTicksOutput;
                emitCatchupRepeats(duplicateLineage.IsValid() ? &duplicateLineage : nullptr);
            } else {
                cadenceCounters.liveTickMissCount++;
            }
            continueMainLoop = true;
            return;
        }
}

void MediaEncoderSession::recordDuplicate(const QueuedFrame* duplicateFrame, const InjectFrameLineage* duplicateLineage, bool duplicateFromDrainReason, bool duplicateFromDeferredReason, bool duplicateFromTimerRebaseReason, bool duplicateFromCatchupReason, bool duplicateFromCapacityPacerReason) {

cadenceCounters.consecutiveDuplicateFrames++;
cadenceCounters.maxConsecutiveDuplicateFrames =
    std::max(cadenceCounters.maxConsecutiveDuplicateFrames, cadenceCounters.consecutiveDuplicateFrames);
// Session-wide contiguous run: survives the per-window cadence reset so a >1s freeze is
// measured as one run (the real visible-freeze metric), not split per logging window.
++captureSessionSummary.currentContiguousDupTicks;
captureSessionSummary.longestContiguousDupTicks =
    std::max(captureSessionSummary.longestContiguousDupTicks,
             static_cast<uint64_t>(captureSessionSummary.currentContiguousDupTicks));
if (media_main_g_pSharedMem) {
    media_main_g_pSharedMem->runtimeState.duplicateFrames.fetch_add(1, std::memory_order_relaxed);
    if (duplicateFromDrainReason) {
        media_main_g_pSharedMem->runtimeState.duplicateFramesDrain.fetch_add(1, std::memory_order_relaxed);
    } else if (duplicateFromDeferredReason ||
               (duplicateFrame && MatchesInjectFrameLineage(*duplicateFrame, lastDeferredLineage)) ||
               (duplicateLineage && MatchesInjectFrameLineage(*duplicateLineage, lastDeferredLineage))) {
        media_main_g_pSharedMem->runtimeState.duplicateFramesDeferred.fetch_add(1, std::memory_order_relaxed);
    } else if (duplicateFromTimerRebaseReason) {
        media_main_g_pSharedMem->runtimeState.duplicateFramesTimerRebase.fetch_add(1, std::memory_order_relaxed);
    } else if (!duplicateFromCapacityPacerReason) {
        media_main_g_pSharedMem->runtimeState.duplicateFramesNoSource.fetch_add(1, std::memory_order_relaxed);
    }
}
static uint64_t s_lastDupLogTick = 0;
uint64_t nowTick = GetTickCount64();
if (nowTick - s_lastDupLogTick >= 1000) {
    const uint32_t logFrameIndex =
        duplicateFrame ? duplicateFrame->frameIndex : (duplicateLineage ? duplicateLineage->frameIndex : 0);
    const int32_t logTextureIndex = duplicateFrame
                                        ? duplicateFrame->textureIndex
                                        : (duplicateLineage ? duplicateLineage->textureIndex : -1);
    const uint32_t logRingIndex =
        duplicateFrame ? duplicateFrame->ringIndex : (duplicateLineage ? duplicateLineage->ringIndex : 0);
    const uint64_t logFenceValue =
        duplicateFrame ? duplicateFrame->fenceValue : (duplicateLineage ? duplicateLineage->fenceValue : 0);
    LogInfo(
        "[EncoderThread] Duplicate frame=%u tex=%d ring=%u fence=%llu: credit=%.3f rate=%.3f bufferedI=%zu "
        "bufferedW=%zu",
        logFrameIndex, logTextureIndex, logRingIndex, static_cast<unsigned long long>(logFenceValue),
        frameCreditAccumulator, smoothedInputPerTick, bufferedInjectFrames.size(),
        bufferedWgcFrames.size());
    s_lastDupLogTick = nowTick;
}

}

void MediaEncoderSession::advanceWakeDeadlineForCatchupTick() {

if (ce::capture_policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(useScreenGrab,
                                                                   injectCfrRecoveryActive)) {
    nextSampleTime.QuadPart += targetIntervalTicks;
}

}

void MediaEncoderSession::emitCatchupRepeats(const InjectFrameLineage* duplicateLineage) {

if (!scheduledLiveCfrTick || catchupTicksThisLoop <= 1 || !media_main_g_HasLastFrame) {
    return;
}

const size_t freshCatchupReserveFrames =
    useScreenGrab && isWgcEffectiveContentDelayActive() ? getWgcDelayReservoirLowWaterFrames() : 0u;
const double freshCatchupServiceMs = std::max(smoothedWgcFreshServiceMs, smoothedEncodeMs);
const bool freshCatchupServiceTooSlow = ce::capture_policy::IsEncoderTooSlowForTargetFps(
    freshCatchupServiceMs, frameIntervalMs, outputFps);
uint32_t remainingFreshCatchupBudget =
    useScreenGrab && !config.video.useVFR
        ? ce::capture_policy::GetWgcFreshCatchupBudgetThisLoop(
              catchupTicksThisLoop, media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed),
              freshCatchupServiceTooSlow, freshCatchupServiceMs, frameIntervalMs, bufferedWgcFrames.size(),
              freshCatchupReserveFrames)
        : 0u;

for (uint32_t extraTick = 1; extraTick < catchupTicksThisLoop; ++extraTick) {
    if (useScreenGrab && config.video.useVFR &&
        !ce::capture_policy::ShouldAllowWgcExtraCatchupTicks(
            encoderTooSlowForTargetCurrent, bufferedWgcFrames.size(), frameCreditAccumulator,
            outputShortfallTicks)) {
        break;
    }

    // Time budget check: if the tick budget is already exhausted,
    // skip further catchup to avoid cascading latency.
    LARGE_INTEGER budgetNow;
    QueryPerformanceCounter(&budgetNow);
    const double elapsedFromTickStartMs =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        static_cast<double>(budgetNow.QuadPart - cycleStartQpc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
    const bool allowWgcCatchupBudget = useScreenGrab && catchupTicksThisLoop > 1;
    const bool allowForceCatchupBudget =
        useScreenGrab &&
        outputShortfallTicks >= ce::capture_policy::kCfrShortfallForceCatchupThresholdTicks;
    const double shortfallDurationMs =
        ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
    const double catchupBudgetMs =
        allowForceCatchupBudget
            ? (frameIntervalMs *
               ce::capture_policy::GetWgcForceCatchupBudgetFrameMultiplier(shortfallDurationMs))
        : allowWgcCatchupBudget ? (frameIntervalMs * 2.0)
                                : frameIntervalMs;

    // For CFR recording, video smoothness is paramount. We have a 32-frame deep queue
    // (~266ms at 120fps) to absorb temporary encoder spikes. We only force duplicate frames
    // if we are meaningfully behind (e.g. > 50ms delay) to prevent runaway latency.
    // Otherwise, we process the fresh frame to preserve the correct visual pacing.
    const double cfrSmoothnessToleranceMs = (!config.video.useVFR && useScreenGrab) ? 50.0 : 0.0;
    bool allowFreshCatchup = remainingFreshCatchupBudget > 0u;

    if (elapsedFromTickStartMs > catchupBudgetMs + cfrSmoothnessToleranceMs) {
        static uint64_t s_lastBudgetLog = 0;
        uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastBudgetLog >= 1000) {
            LogInfo(
                "[EncoderThread] Catchup budget exceeded at extraTick=%u (elapsed=%.2fms > budget=%.2fms + "
                "tol=%.2fms). %s",
                extraTick, elapsedFromTickStartMs, catchupBudgetMs, cfrSmoothnessToleranceMs,
                useScreenGrab ? "Switching to duplicate frames to preserve CFR timeline without stalling."
                              : "Inject fresh catch-up remains gated by encoder health and target coverage.");
            s_lastBudgetLog = nowTick;
        }
        if (config.video.useVFR || outputShortfallTicks == 0) {
            break;
        } else if (useScreenGrab) {
            // CFR must not break to avoid timeline holes, but we must stop using expensive fresh frames!
            allowFreshCatchup = false;
        }
    }

    const int64_t repeatScheduledQpc =
        scheduledOutputQpc + static_cast<int64_t>(extraTick) * targetIntervalTicks;

    if (!useScreenGrab && !config.video.useVFR && MediaEngine_ProcessFrame) {
        const size_t catchupInjectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
            config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
        const size_t catchupMinBufferedInjectFrames = ce::capture_policy::GetMinBufferedInjectFrames(
            catchupInjectReserveFrames, recordingOutputLive);
        const bool encoderBottleneckedNow = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
        const bool allowFreshInjectCatchup = ce::capture_policy::ShouldUseFreshInjectCatchup(
            config.video.useVFR, encoderBottleneckedNow, injectEncoderServiceTooSlowCurrent,
            bufferedInjectFrames.size(), catchupMinBufferedInjectFrames, outputShortfallTicks,
            injectCfrRecoveryActive);
        if (allowFreshInjectCatchup) {
            size_t availableCount = bufferedInjectFrames.size() - catchupMinBufferedInjectFrames;
            const int64_t baseCatchupPlayoutTargetQpc =
                ComputeDelayedContentGridStartQpc(repeatScheduledQpc, avContentDelayQpc);
            const int64_t catchupPhaseReferenceQpc =
                bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
            const int64_t catchupPlayoutTargetQpc = applyCaptureSyncPhaseTarget(
                "inject", injectCfrPhaseLock, baseCatchupPlayoutTargetQpc,
                catchupPhaseReferenceQpc);
            const int64_t catchupLeadToleranceQpc =
                ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
            auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                               lastEmittedInjectSourceQpc);
            };
            auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                return isFreshInjectCandidate(candidate) &&
                       !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
            };
            size_t bestIdx = SelectFrameClosestToTimestampIf(
                bufferedInjectFrames, availableCount, catchupPlayoutTargetQpc, isAllowedCandidate);
            if (bestIdx >= availableCount) {
                bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                          catchupPlayoutTargetQpc,
                                                          isFreshInjectCandidate);
            }

            const bool catchupTargetCovered =
                bestIdx < availableCount && isFreshInjectCandidate(bufferedInjectFrames[bestIdx]) &&
                ce::capture_policy::DecideCfrNearestPlayout(
                    bufferedInjectFrames[bestIdx].timestamp, catchupPlayoutTargetQpc,
                    catchupLeadToleranceQpc, lastEmittedInjectSourceQpc)
                    .emit;
            if (catchupTargetCovered) {
                for (size_t i = 0; i < bestIdx; ++i) {
                    QueuedFrame stale = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(stale);
                    media_main_g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1,
                                                                                std::memory_order_relaxed);
                    }
                    ++injectTargetSupersededThisWindow;
                    ++injectTargetSupersededTotal;
                }

                QueuedFrame catchupFrame = std::move(bufferedInjectFrames.front());
                bufferedInjectFrames.pop_front();
                const InjectFrameLineage catchupLineage = MakeInjectFrameLineage(catchupFrame);

                LARGE_INTEGER catchupStartEnc, catchupEndEnc;
                QueryPerformanceCounter(&catchupStartEnc);
                uint64_t frameAgeUs = 0;
                if (catchupFrame.timestamp > 0 && catchupStartEnc.QuadPart > catchupFrame.timestamp) {
                    frameAgeUs = static_cast<uint64_t>((catchupStartEnc.QuadPart - catchupFrame.timestamp) *
                                                       1000000 / qpcFreq.QuadPart);
                }
                cadenceCounters.frameAgeAccumUs += frameAgeUs;
                cadenceCounters.frameAgeSamples++;
                cadenceCounters.frameAgeMaxUs =
                    std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
                if (repeatScheduledQpc > 0) {
                    const int64_t signedOutputScheduleErrorUs =
                        ((catchupStartEnc.QuadPart - repeatScheduledQpc) * 1000000) / qpcFreq.QuadPart;
                    cadenceCounters.RecordOutputScheduleError(signedOutputScheduleErrorUs);
                }

                const bool catchupEncodeSucceeded = MediaEngine_ProcessFrame(
                    (uint64_t)catchupFrame.sharedHandle, (uint64_t)catchupFrame.fenceHandle,
                    catchupFrame.fenceValue, catchupFrame.timestamp, catchupFrame.luidLow,
                    catchupFrame.luidHigh, catchupFrame.sourcePid, catchupFrame.width, catchupFrame.height,
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    catchupFrame.format, catchupFrame.isHDR, catchupFrame.isShmem, catchupFrame.shmemSlot,
                    &catchupFrame.cursorState);
                const bool catchupEncodeDeferred =
                    MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                QueryPerformanceCounter(&catchupEndEnc);

                const double currentEncodeMs =
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    (double)(catchupEndEnc.QuadPart - catchupStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                const double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                if (pureEncodeMs > 0.0) {
                    if (smoothedEncodeMs == 0.0) {
                        smoothedEncodeMs = pureEncodeMs;
                    } else {
                        smoothedEncodeMs =
                            smoothedEncodeMs * (1.0 - media_main_kEncodeEmaAlpha) + pureEncodeMs * media_main_kEncodeEmaAlpha;
                    }
                }
                if (catchupEncodeSucceeded && !catchupEncodeDeferred) {
                    observeInjectFreshService(currentEncodeMs, pureEncodeMs);
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));

                if (catchupEncodeSucceeded && !catchupEncodeDeferred) {
                    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
                        ReleaseQueuedFrameTexture(media_main_g_LastFrame);
                    }

                    const double currentFenceMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
                    if (smoothedInjectFenceMs == 0.0) {
                        smoothedInjectFenceMs = currentFenceMs;
                    } else {
                        smoothedInjectFenceMs = smoothedInjectFenceMs * 0.90 + currentFenceMs * 0.10;
                    }

                    if (catchupFrame.frameIndex != 0) {
                        if (lastEncodedInjectFrameIndex != 0 &&
                            catchupFrame.frameIndex < lastEncodedInjectFrameIndex) {
                            LogWarn(
                                "[EncoderThread] Inject lineage regression during catch-up: encoded "
                                "frame=%u after frame=%u (ring=%u tex=%d ts=%lld)",
                                catchupFrame.frameIndex, lastEncodedInjectFrameIndex,
                                catchupFrame.ringIndex, catchupFrame.textureIndex,
                                static_cast<long long>(catchupFrame.timestamp));
                            if (media_main_g_pSharedMem) {
                                media_main_g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                        lastEncodedInjectFrameIndex = catchupFrame.frameIndex;
                    }
                    if (IsInjectTextureIndexValid(catchupFrame.textureIndex)) {
                        uint32_t& lastTextureFrame =
                            lastEncodedFrameByTextureIndex[static_cast<size_t>(catchupFrame.textureIndex)];
                        if (lastTextureFrame != 0 && catchupFrame.frameIndex != 0 &&
                            catchupFrame.frameIndex <= lastTextureFrame) {
                            LogWarn(
                                "[EncoderThread] Texture slot reuse anomaly during catch-up: tex=%d "
                                "frame=%u previous=%u ring=%u fence=%llu ts=%lld",
                                catchupFrame.textureIndex, catchupFrame.frameIndex, lastTextureFrame,
                                catchupFrame.ringIndex,
                                static_cast<unsigned long long>(catchupFrame.fenceValue),
                                static_cast<long long>(catchupFrame.timestamp));
                            if (media_main_g_pSharedMem) {
                                media_main_g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                        lastTextureFrame = catchupFrame.frameIndex;
                    }

                    if (media_main_g_pSharedMem) {
                        if (currentEncodeMs > frameIntervalMs * 1.10) {
                            media_main_g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                        media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                        media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1,
                                                                               std::memory_order_relaxed);
                    }

                    catchupFrame.injectRingLease.Reset();
                    media_main_g_LastFrame = std::move(catchupFrame);
                    media_main_g_HasLastFrame = true;
                    lastSuccessfullyEncodedInjectLineage = catchupLineage;
                    if (media_main_g_LastFrame.timestamp > 0) {
                        lastEmittedInjectSourceQpc = media_main_g_LastFrame.timestamp;
                    }
                    lastDeferredLineage = {};
                    ++injectTargetSelectThisWindow;
                    ++injectTargetSelectTotal;
                    if (qpcFreq.QuadPart > 0) {
                        const uint64_t residualUs =
                            ce::capture_policy::GetCfrTimestampDistanceQpc(
                                media_main_g_LastFrame.timestamp, catchupPlayoutTargetQpc) *
                            1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                        injectTargetResidualMaxUs =
                            std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                    }
                    cadenceCounters.consecutiveDeferredFrames = 0;
                    cadenceCounters.consecutiveDuplicateFrames = 0;
                    captureSessionSummary.currentContiguousDupTicks = 0;
                    cadenceCounters.liveTickEmitCount++;
                    cadenceCounters.liveTickUniqueCount++;
                    cadenceCounters.CommitHoldRun();
                    cadenceCounters.holdTicksRunning = 1;
                    ++liveTicksOutput;
                    ++encoderGridTickCount;
                    ++cfrCatchupTicksExecuted;
                    ++injectFreshCatchupThisWindow;
                    ++injectFreshCatchupTotal;
                    advanceWakeDeadlineForCatchupTick();
                    continue;
                }

                if (catchupEncodeDeferred) {
                    media_main_g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                    cadenceCounters.consecutiveDeferredFrames++;
                    cadenceCounters.maxConsecutiveDeferredFrames =
                        std::max(cadenceCounters.maxConsecutiveDeferredFrames,
                                 cadenceCounters.consecutiveDeferredFrames);
                    lastDeferredLineage = catchupLineage;
                    catchupFrame.deferCount++;
                    if (!media_main_g_RejectInjectFrames.load(std::memory_order_acquire) &&
                        catchupFrame.deferCount <= ce::capture_policy::kMaxInjectDeferredFrameRetries) {
                        bufferedInjectFrames.push_front(std::move(catchupFrame));
                        ++injectDeferredRequeuedThisWindow;
                        ++injectDeferredRequeuedTotal;
                    } else {
                        DiscardQueuedFrame(catchupFrame);
                        ++injectDeferredDroppedThisWindow;
                        ++injectDeferredDroppedTotal;
                    }
                } else {
                    DiscardQueuedFrame(catchupFrame);
                }
            }
        }
    }


    if (allowFreshCatchup && useScreenGrab && MediaEngine_ProcessFrameD3D11 && !bufferedWgcFrames.empty()) {
        const int64_t catchupGridTick = encoderGridTickCount + 1;
        int64_t catchupSelectionTargetQpc = computeWgcSelectionTargetForTick(
            repeatScheduledQpc, catchupGridTick, wgcSelectionDelayAppliedThisTick);
        // Reuse the phase already learned by ordinary selection without
        // learning from buffered future history during debt recovery.
        // This keeps delayed and non-delayed recovery on exactly the same
        // content grid as their ordinary source-selection paths.
        catchupSelectionTargetQpc = ce::capture_policy::ApplyCfrCaptureSyncPhaseLock(
            wgcCfrPhaseLock, catchupSelectionTargetQpc, 0, captureSyncSourceIntervalTicks,
            captureSyncPhaseLockEnabled);
        QueuedFrame catchupFrame;
        const size_t spendableCatchupFrames =
            bufferedWgcFrames.size() > freshCatchupReserveFrames
                ? bufferedWgcFrames.size() - freshCatchupReserveFrames
                : 0u;
        size_t catchupFrameIndex = spendableCatchupFrames;
        uint64_t catchupFrameDistance = std::numeric_limits<uint64_t>::max();
        for (size_t candidateIndex = 0; candidateIndex < spendableCatchupFrames; ++candidateIndex) {
            const QueuedFrame& candidate = bufferedWgcFrames[candidateIndex];
            if (!ce::capture_policy::ShouldUseFreshWgcCatchupFrame(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    candidate.timestamp, catchupSelectionTargetQpc, lastEmittedWgcSelectionQpc,
                    lastEmittedWgcSourceQpc, targetIntervalTicks)) {
                continue;
            }
            const uint64_t candidateDistance = ce::capture_policy::GetCfrTimestampDistanceQpc(
                GetFrameSelectionTimestamp(candidate), catchupSelectionTargetQpc);
            if (candidateDistance < catchupFrameDistance) {
                catchupFrameIndex = candidateIndex;
                catchupFrameDistance = candidateDistance;
            }
        }
        if (catchupFrameIndex < spendableCatchupFrames) {
            for (size_t staleIndex = 0; staleIndex < catchupFrameIndex; ++staleIndex) {
                QueuedFrame stale = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(stale);
                ++wgcDropObsoleteCount;
            }
            catchupFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            if (catchupFrame.duplicateSourceTimestamp) {
                ++wgcSelectDuplicateSourceCount;
            } else {
                ++wgcSelectFreshCount;
            }
            LARGE_INTEGER catchupStartEnc, catchupEndEnc;
            QueryPerformanceCounter(&catchupStartEnc);
            const uint64_t frameAgeUs =
                catchupFrame.timestamp > 0 && catchupStartEnc.QuadPart > catchupFrame.timestamp
                    ? static_cast<uint64_t>((catchupStartEnc.QuadPart - catchupFrame.timestamp) * 1000000 /
                                            qpcFreq.QuadPart)
                    : 0u;
            if (repeatScheduledQpc > 0) {
                const int64_t signedErrorUs =
                    ((catchupStartEnc.QuadPart - repeatScheduledQpc) * 1000000) / qpcFreq.QuadPart;
                cadenceCounters.RecordOutputScheduleError(signedErrorUs);
            }

            const int64_t catchupTimelineElapsedUs = computeLiveTimelineElapsedUs(repeatScheduledQpc);
            const auto privacyDecision = evaluateScreenGrabPrivacy(&catchupFrame);
            bool freshCatchupEncodeSucceeded = false;
            if (privacyDecision.useBlackFrame) {
                freshCatchupEncodeSucceeded =
                    submitPrivacyBlackFrame(catchupFrame, catchupFrame.timestamp, repeatScheduledQpc,
                                            catchupTimelineElapsedUs);
            } else {
                SyncDuplicationCursorSuppression(catchupFrame.wgcCursorEmbedded);
                const ce::cursor::CaptureState catchupCursorState =
                    selectCursorStateForScheduledQpc(repeatScheduledQpc, catchupFrame, "fresh-catchup");
                freshCatchupEncodeSucceeded = MediaEngine_ProcessFrameD3D11(
                    catchupFrame.texture, catchupFrame.timestamp, catchupFrame.width, catchupFrame.height,
                    catchupFrame.isHDR, catchupFrame.captureLeft, catchupFrame.captureTop,
                    catchupTimelineElapsedUs, &catchupCursorState);
                if (freshCatchupEncodeSucceeded && privacyRuntime.IsEnabled()) {
                    privacyRuntime.CommitRealOutput();
                }
            }
            const bool recoveredCatchupEncodeFailure =
                !freshCatchupEncodeSucceeded &&
                recoverScheduledFreshEncodeFailure(true, false, false, repeatScheduledQpc, &catchupFrame,
                                                   "WGC grid-matched fresh-catchup");
            if (!freshCatchupEncodeSucceeded && !recoveredCatchupEncodeFailure) {
                ReleaseQueuedFrameTexture(catchupFrame);
                ++cadenceCounters.liveTickMissCount;
                break;
            }
            releaseWgcLeaseAfterMediaEngineCopy(
                catchupFrame, recoveredCatchupEncodeFailure ? "fresh-catchup encode-failure repeat"
                                                            : "grid-matched fresh-catchup");
            QueryPerformanceCounter(&catchupEndEnc);
            const double currentEncodeMs =
                static_cast<double>(catchupEndEnc.QuadPart - catchupStartEnc.QuadPart) * 1000.0 /
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                qpcFreq.QuadPart;
            const double pureEncodeMs = static_cast<double>(MediaEngine_GetLastFrameEncodeTimeUs()) / 1000.0;
            if (pureEncodeMs > 0.0) {
                smoothedEncodeMs = smoothedEncodeMs == 0.0
                                       ? pureEncodeMs
                                       : smoothedEncodeMs * (1.0 - media_main_kEncodeEmaAlpha) +
                                             pureEncodeMs * media_main_kEncodeEmaAlpha;
            }
            if (freshCatchupEncodeSucceeded) {
                ce::capture_policy::UpdateWgcServiceTimeEma(
                    currentEncodeMs, pureEncodeMs, media_main_kEncodeEmaAlpha, smoothedWgcFreshServiceMs,
                    wgcFreshServiceSamples);
            }
            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                        ce::capture_policy::IsEncoderStartupWindow(
                                            recordingOutputLive, recordingLiveTick, GetTickCount64()));
            if (media_main_g_pSharedMem) {
                if (currentEncodeMs > frameIntervalMs * 1.10) {
                    media_main_g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                }
                media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
            }

            if (recoveredCatchupEncodeFailure) {
                ReleaseQueuedFrameTexture(catchupFrame);
                recordDuplicate(nullptr, nullptr, false, false, false, true);
                ++wgcRepeatCatchupCount;
                ++wgcRepeatCatchupTotal;
                ++cadenceCounters.liveTickEmitCount;
                ++cadenceCounters.liveTickDuplicateCount;
                ++cadenceCounters.holdTicksRunning;
                ++liveTicksOutput;
                ++encoderGridTickCount;
                ++cfrCatchupTicksExecuted;
                remainingFreshCatchupBudget = 0;
                advanceWakeDeadlineForCatchupTick();
                continue;
            }

            const int64_t selectedQpc = GetFrameSelectionTimestamp(catchupFrame);
            const int64_t rawSelectedQpc = getWgcRawSelectionTimestamp(catchupFrame);
            const int64_t signedSelectionErrorUs =
                ((selectedQpc - catchupSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart;
            const int64_t signedRawSelectionErrorUs =
                rawSelectedQpc > 0
                    ? ((rawSelectedQpc - catchupSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart
                    : signedSelectionErrorUs;
            const uint64_t absoluteSelectionErrorUs = static_cast<uint64_t>(
                signedSelectionErrorUs >= 0 ? signedSelectionErrorUs : -signedSelectionErrorUs);
            cadenceCounters.RecordSelectionError(signedSelectionErrorUs);
            wgcSelectionErrorAccumUs += absoluteSelectionErrorUs;
            wgcSelectionErrorSignedAccumUs += signedSelectionErrorUs;
            ++wgcSelectionErrorSamples;
            wgcSelectionErrorMaxUs =
                std::max(wgcSelectionErrorMaxUs, SaturatingToUint32(absoluteSelectionErrorUs));
            if (signedSelectionErrorUs < 0) {
                const uint32_t earlyUs =
                    SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs));
                wgcSelectionEarlyMaxUs = std::max(wgcSelectionEarlyMaxUs, earlyUs);
            } else {
                const uint32_t lateUs = SaturatingToUint32(absoluteSelectionErrorUs);
                wgcSelectionLateMaxUs = std::max(wgcSelectionLateMaxUs, lateUs);
            }
            if (wgcSelectionDelayAppliedThisTick) {
                recordWgcDelayRealization(signedSelectionErrorUs, signedRawSelectionErrorUs);
            }
            if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
                ReleaseQueuedFrameTexture(media_main_g_LastFrame);
            }
            media_main_g_LastFrame = std::move(catchupFrame);
            media_main_g_HasLastFrame = true;
            cadenceCounters.frameAgeAccumUs += frameAgeUs;
            ++cadenceCounters.frameAgeSamples;
            cadenceCounters.frameAgeMaxUs =
                std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
            lastEmittedWgcSourceQpc = media_main_g_LastFrame.timestamp;
            lastEmittedWgcSelectionQpc = selectedQpc;
            lastSuccessfulWgcCursorEmbedded = media_main_g_LastFrame.wgcCursorEmbedded;
            hasSuccessfulWgcCursorMetadata = true;

            static uint64_t s_lastGridCatchupLogTick = 0;
            const uint64_t gridCatchupNowTick = GetTickCount64();
            if (gridCatchupNowTick - s_lastGridCatchupLogTick >= 1000) {
                LogInfo(
                    "[EncoderThread] WGC CFR grid-matched recovery frame: residual=%lldus raw=%lldus "
                    "shortfall=%u buffered=%zu reserve=%zu freshSvcEma=%.2fms",
                    static_cast<long long>(signedSelectionErrorUs),
                    static_cast<long long>(signedRawSelectionErrorUs), outputShortfallTicks,
                    bufferedWgcFrames.size(), freshCatchupReserveFrames, smoothedWgcFreshServiceMs);
                s_lastGridCatchupLogTick = gridCatchupNowTick;
            }

            cadenceCounters.consecutiveDuplicateFrames = 0;
            captureSessionSummary.currentContiguousDupTicks = 0;
            ++cadenceCounters.liveTickEmitCount;
            ++cadenceCounters.liveTickUniqueCount;
            cadenceCounters.CommitHoldRun();
            cadenceCounters.holdTicksRunning = 1;
            ++liveTicksOutput;
            ++encoderGridTickCount;
            ++cfrCatchupTicksExecuted;
            ++wgcFreshCatchupCount;
            ++wgcFreshCatchupTotal;
            --remainingFreshCatchupBudget;
            if (media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
                smoothedWgcFreshServiceMs >=
                    frameIntervalMs * ce::capture_policy::kWgcFreshCatchupServiceBudgetRatio) {
                remainingFreshCatchupBudget = 0;
            }
            advanceWakeDeadlineForCatchupTick();
            continue;
        }
        // Without a sync-safe historical surplus frame, hold the prior frame for this exact CFR slot.
        if (!(!config.video.useVFR && outputShortfallTicks > 0)) {
            break;
        }
    }

    if (!hasRepeatLastFramePath) {
        cadenceCounters.liveTickMissCount++;
        break;
    }

    LARGE_INTEGER repeatStartEnc, repeatEndEnc;
    QueryPerformanceCounter(&repeatStartEnc);
    bool repeatSucceeded = repeatLastFrameForScheduledQpc(repeatScheduledQpc);
    bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
    QueryPerformanceCounter(&repeatEndEnc);
    if (!repeatSucceeded || repeatDeferred) {
        cadenceCounters.liveTickMissCount++;
        break;
    }

    const double currentEncodeMs =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        (double)(repeatEndEnc.QuadPart - repeatStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
    const double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
    if (pureEncodeMs > 0.0) {
        if (smoothedEncodeMs == 0.0) {
            smoothedEncodeMs = pureEncodeMs;
        } else {
            smoothedEncodeMs = smoothedEncodeMs * (1.0 - media_main_kEncodeEmaAlpha) + pureEncodeMs * media_main_kEncodeEmaAlpha;
        }
    }
    if (useScreenGrab) {
        ce::capture_policy::UpdateWgcServiceTimeEma(
            currentEncodeMs, pureEncodeMs, media_main_kEncodeEmaAlpha, smoothedWgcRepeatServiceMs,
            wgcRepeatServiceSamples);
    } else {
        observeInjectRepeatService(currentEncodeMs, pureEncodeMs);
    }
    UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                ce::capture_policy::IsEncoderStartupWindow(
                                    recordingOutputLive, recordingLiveTick, GetTickCount64()));

    if (media_main_g_pSharedMem) {
        if (currentEncodeMs > frameIntervalMs * 1.10) {
            media_main_g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
        }
        media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
        media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
    }

    recordDuplicate(&media_main_g_LastFrame, duplicateLineage, false, false, false, true);
    if (useScreenGrab) {
        ++wgcRepeatCatchupCount;
        ++wgcRepeatCatchupTotal;
    } else {
        ++injectRepeatCatchupThisWindow;
        ++injectRepeatCatchupTotal;
    }
    cadenceCounters.liveTickEmitCount++;
    cadenceCounters.liveTickDuplicateCount++;
    cadenceCounters.holdTicksRunning++;
    ++liveTicksOutput;
    ++encoderGridTickCount;
    ++cfrCatchupTicksExecuted;
    advanceWakeDeadlineForCatchupTick();
}

}
