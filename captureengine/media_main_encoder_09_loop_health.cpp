#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopHealth() {
        if (media_main_g_pSharedMem && GetTickCount() - lastHealthLog >= 1000) {
            auto& state = media_main_g_pSharedMem->runtimeState;
            const uint32_t avgFrameAgeUs =
                cadenceCounters.frameAgeSamples > 0
                    ? SaturatingToUint32(cadenceCounters.frameAgeAccumUs / cadenceCounters.frameAgeSamples)
                    : 0;
            const uint32_t avgSelectionErrorUs = cadenceCounters.outputScheduleErrorSamples > 0
                                                     ? SaturatingToUint32(cadenceCounters.outputScheduleErrorAccumUs /
                                                                          cadenceCounters.outputScheduleErrorSamples)
                                                     : 0;
            const int32_t avgSignedSelectionErrorUs =
                cadenceCounters.outputScheduleErrorSamples > 0
                    ? static_cast<int32_t>(cadenceCounters.outputScheduleErrorSignedAccumUs /
                                           static_cast<int64_t>(cadenceCounters.outputScheduleErrorSamples))
                    : 0;
            const uint32_t avgWgcSelectionErrorUs =
                wgcSelectionErrorSamples > 0 ? SaturatingToUint32(wgcSelectionErrorAccumUs / wgcSelectionErrorSamples)
                                             : 0;
            const int32_t avgSignedWgcSelectionErrorUs =
                wgcSelectionErrorSamples > 0 ? static_cast<int32_t>(wgcSelectionErrorSignedAccumUs /
                                                                    static_cast<int64_t>(wgcSelectionErrorSamples))
                                             : 0;
            state.frameAgeAvgUs.store(avgFrameAgeUs, std::memory_order_relaxed);
            state.frameAgeMaxUs.store(cadenceCounters.frameAgeMaxUs, std::memory_order_relaxed);
            state.selectionErrorAvgUs.store(avgSelectionErrorUs, std::memory_order_relaxed);
            state.selectionErrorMaxUs.store(cadenceCounters.outputScheduleErrorMaxUs, std::memory_order_relaxed);
            state.selectionErrorSignedAvgUs.store(avgSignedSelectionErrorUs, std::memory_order_relaxed);
            state.selectionEarlyMaxUs.store(cadenceCounters.outputScheduleEarlyMaxUs, std::memory_order_relaxed);
            state.selectionLateMaxUs.store(cadenceCounters.outputScheduleLateMaxUs, std::memory_order_relaxed);
            state.wgcSelectionErrorAvgUs.store(avgWgcSelectionErrorUs, std::memory_order_relaxed);
            state.wgcSelectionErrorMaxUs.store(wgcSelectionErrorMaxUs, std::memory_order_relaxed);
            state.wgcSelectionErrorSignedAvgUs.store(avgSignedWgcSelectionErrorUs, std::memory_order_relaxed);
            state.wgcSelectionEarlyMaxUs.store(wgcSelectionEarlyMaxUs, std::memory_order_relaxed);
            state.wgcSelectionLateMaxUs.store(wgcSelectionLateMaxUs, std::memory_order_relaxed);
            state.consecutiveDeferredFrames.store(cadenceCounters.consecutiveDeferredFrames, std::memory_order_relaxed);
            state.maxConsecutiveDeferredFrames.store(cadenceCounters.maxConsecutiveDeferredFrames,
                                                     std::memory_order_relaxed);
            state.consecutiveDuplicateFrames.store(cadenceCounters.consecutiveDuplicateFrames,
                                                   std::memory_order_relaxed);
            state.maxConsecutiveDuplicateFrames.store(cadenceCounters.maxConsecutiveDuplicateFrames,
                                                      std::memory_order_relaxed);

            const uint32_t dupNoSource = state.duplicateFramesNoSource.load(std::memory_order_relaxed);
            const uint32_t dupDeferred = state.duplicateFramesDeferred.load(std::memory_order_relaxed);
            const uint32_t dupTimer = state.duplicateFramesTimerRebase.load(std::memory_order_relaxed);
            const uint32_t dupDrain = state.duplicateFramesDrain.load(std::memory_order_relaxed);
            const uint32_t invalidMeta = state.invalidFrameMetadata.load(std::memory_order_relaxed);
            const uint32_t invalidHandle = state.invalidSharedHandles.load(std::memory_order_relaxed);
            const uint32_t tsRegress = state.sourceTimestampRegressions.load(std::memory_order_relaxed);
            const uint32_t tsStall = state.sourceTimestampStalls.load(std::memory_order_relaxed);
            const uint32_t timerRebases = state.timerRebases.load(std::memory_order_relaxed);
            const uint32_t packetClamps = state.packetDurationClamps.load(std::memory_order_relaxed);
            const uint32_t negativePts = state.negativePtsCount.load(std::memory_order_relaxed);
            const uint32_t nonMonotonicPts = state.nonMonotonicPtsCount.load(std::memory_order_relaxed);
            const uint32_t overloadFlags = state.encoderOverloadFlags.load(std::memory_order_relaxed);
            const uint32_t muxQueueBytes = state.muxQueueBytes.load(std::memory_order_relaxed);
            const uint32_t muxQueuePackets = state.muxQueuePackets.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureCount = state.muxBackpressureCount.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureWaitUs = state.muxBackpressureWaitUs.load(std::memory_order_relaxed);
            const uint32_t muxBackpressureMaxWaitUs = state.muxBackpressureMaxWaitUs.load(std::memory_order_relaxed);
            const uint32_t oldestBufferedFrameAgeUs = state.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed);
            uint64_t liveWallElapsedUs = 0;
            if (recordingOutputLive && liveStartQpc.QuadPart > 0 && targetIntervalTicks > 0 && liveTicksScheduled > 0) {
                LARGE_INTEGER nowQpc;
                QueryPerformanceCounter(&nowQpc);
                if (nowQpc.QuadPart > liveStartQpc.QuadPart) {
                    liveWallElapsedUs =
                        static_cast<uint64_t>((nowQpc.QuadPart - liveStartQpc.QuadPart) * 1000000 / qpcFreq.QuadPart);
                    outputShortfallTicks = updateLiveCfrShortfall(nowQpc.QuadPart);
                }
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            const double sustainableOutputFps = ce::capture_policy::GetEncoderSustainableOutputFps(smoothedEncodeMs);
            state.encoderSustainFpsX100.store(
                static_cast<uint32_t>(std::clamp(sustainableOutputFps * 100.0, 0.0, 4294967295.0)),
                std::memory_order_relaxed);
            const uint32_t encoderBudgetUtilizationPermille =
                ce::capture_policy::GetEncoderBudgetUtilizationPermille(smoothedEncodeMs, frameIntervalMs);
            const bool encoderTooSlowForTarget =
                ce::capture_policy::IsEncoderTooSlowForTargetFps(smoothedEncodeMs, frameIntervalMs, outputFps);
            const double oldestBufferedFrameAgeMs = static_cast<double>(oldestBufferedFrameAgeUs) / 1000.0;
            if (wgcStarvedEpisode.active) {
                wgcStarvedEpisode.maxEncodeEmaMs = std::max(wgcStarvedEpisode.maxEncodeEmaMs, smoothedEncodeMs);
                wgcStarvedEpisode.maxMuxBackpressureCount =
                    std::max(wgcStarvedEpisode.maxMuxBackpressureCount, muxBackpressureCount);
                wgcStarvedEpisode.maxMuxBackpressureWaitUs =
                    std::max(wgcStarvedEpisode.maxMuxBackpressureWaitUs, muxBackpressureMaxWaitUs);
                wgcStarvedEpisode.maxMuxQueueKb =
                    std::max(wgcStarvedEpisode.maxMuxQueueKb, (muxQueueBytes + 1023u) / 1024u);
                wgcStarvedEpisode.peakOverloadFlags |= overloadFlags;
                wgcStarvedEpisode.maxFenceUs =
                    std::max(wgcStarvedEpisode.maxFenceUs, SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(
                                                               0, MediaEngine_GetLastFrameFenceWaitUs()))));
                if (media_main_g_WgcCap) {
                    wgcStarvedEpisode.maxCallbackGapUs = std::max(
                        wgcStarvedEpisode.maxCallbackGapUs, SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(
                                                                0, media_main_g_WgcCap->GetCallbackGapMaxUs()))));
                    wgcStarvedEpisode.maxCopyUs = std::max(
                        wgcStarvedEpisode.maxCopyUs,
                        SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, media_main_g_WgcCap->GetLastCopyTimeUs()))));
                }
            }

            const uint32_t bufferedAtTickAvgPermille =
                wgcQueueTickSampleCount > 0
                    ? SaturatingToUint32((static_cast<uint64_t>(wgcBufferedAtTickSum) * 1000ull) /
                                         static_cast<uint64_t>(wgcQueueTickSampleCount))
                    : 0u;
            const uint32_t bufferedAtTickMinValue = (wgcBufferedAtTickMin == UINT32_MAX) ? 0u : wgcBufferedAtTickMin;
            const uint32_t delayReservoirLowWaterFrames = getWgcDelayReservoirLowWaterFrames();
            const uint32_t delayReservoirTargetFrames = getWgcDelayReservoirTargetFrames();
            const uint32_t delayResidualAvgUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const uint32_t delayResidualP95Us = wgcDelayResidualP95Us();
            const int32_t delayResidualSignedAvgUs =
                wgcDelayResidualSamples > 0 ? static_cast<int32_t>(wgcDelayResidualSignedAccumUs /
                                                                   static_cast<int64_t>(wgcDelayResidualSamples))
                                            : 0;
            const uint32_t delayResidualWindowAvgUs =
                wgcDelayResidualWindowSamples > 0
                    ? SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs / wgcDelayResidualWindowSamples)
                    : 0u;
            const uint32_t delayResidualWindowP95Us = wgcDelayResidualWindowP95Us();
            const int32_t delayResidualWindowSignedAvgUs =
                wgcDelayResidualWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayResidualWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayResidualWindowSamples))
                    : 0;
            const uint32_t rawResidualAvgUs =
                wgcDelayRawResidualSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                    : 0u;
            const int32_t rawResidualSignedAvgUs =
                wgcDelayRawResidualSamples > 0 ? static_cast<int32_t>(wgcDelayRawResidualSignedAccumUs /
                                                                      static_cast<int64_t>(wgcDelayRawResidualSamples))
                                               : 0;
            const uint32_t rawResidualWindowAvgUs =
                wgcDelayRawResidualWindowSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs / wgcDelayRawResidualWindowSamples)
                    : 0u;
            const int32_t rawResidualWindowSignedAvgUs =
                wgcDelayRawResidualWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawResidualWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawResidualWindowSamples))
                    : 0;
            const uint32_t rawResidualP95Us = wgcDelayRawResidualP95Us();
            const uint32_t rawResidualWindowP95Us = wgcDelayRawResidualWindowP95Us();
            const int32_t rawMinusPredictedAvgUs =
                wgcDelayRawMinusPredictedSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedSamples))
                    : 0;
            const int32_t rawMinusPredictedWindowAvgUs =
                wgcDelayRawMinusPredictedWindowSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedWindowSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedWindowSamples))
                    : 0;
            state.wgcQueueEmptyTickPermille.store(wgcNoFreshTickPermille, std::memory_order_relaxed);
            state.wgcBufferedAtTickAvgPermille.store(bufferedAtTickAvgPermille, std::memory_order_relaxed);
            state.wgcBufferedAtTickMin.store(bufferedAtTickMinValue, std::memory_order_relaxed);
            state.wgcStarvedTickCount.store(wgcNoFreshTickCount, std::memory_order_relaxed);
            state.wgcSingleFrameTickCount.store(wgcNoReserveTickCount, std::memory_order_relaxed);
            uint32_t wgcCaptureHealthFlags = 0;
            if (wgcSourceStarvedCurrent ||
                (wgcNoFreshTickPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille &&
                 wgcRecentInputMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps)) {
                wgcCaptureHealthFlags |= ce::capture_policy::kWgcCaptureHealthFlagSourceStarved;
            }
            if (wgcSchedulerLimitedCurrent) {
                wgcCaptureHealthFlags |= ce::capture_policy::kWgcCaptureHealthFlagSchedulerLimited;
            }
            state.wgcCaptureHealthFlags.store(wgcCaptureHealthFlags, std::memory_order_relaxed);
            state.wgcCaptureHealthFps.store(wgcRecentInputMin250Fps, std::memory_order_relaxed);

            const bool rawRecordingEncoderPressure =
                media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
                (overloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
                smoothedEncodeMs >= frameIntervalMs;
            const bool recordingMuxPressure =
                (overloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0;
            const uint32_t recordingDebtMs = SaturatingToUint32(
                static_cast<uint64_t>(std::max(0.0, std::ceil(shortfallDurationMs))));
            const uint64_t recordingHealthNow = GetTickCount64();
            const bool startupPressureOnly =
                ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick,
                                                            recordingHealthNow) &&
                recordingDebtMs < ce::capture_policy::kRecordingHealthCausalDebtMs;
            const ce::capture_policy::RecordingHealthObservation recordingHealthObservation = {
                recordingOutputLive, !config.video.useVFR,
                rawRecordingEncoderPressure && !startupPressureOnly, recordingMuxPressure, recordingDebtMs};
            const uint32_t previousRecordingHealthFlags = recordingHealthState.flags;
            recordingHealthState =
                ce::capture_policy::UpdateRecordingHealth(recordingHealthState, recordingHealthObservation);
            PublishRecordingHealth(recordingHealthState);
            constexpr uint32_t recordingHealthTransitionMask =
                ce::capture_policy::kRecordingHealthCauseMask |
                ce::capture_policy::kRecordingHealthFlagRecovering |
                ce::capture_policy::kRecordingHealthFlagVideoDegraded |
                ce::capture_policy::kRecordingHealthFlagSevere;
            if (((previousRecordingHealthFlags ^ recordingHealthState.flags) & recordingHealthTransitionMask) != 0) {
                LogWarn(
                    "[RECORDING CAPACITY] status=%s cause=%s flags=0x%X debtMs=%u peakDebtMs=%u capacityDebtMs=%u "
                    "target=%ufps sustain=%.1ffps encode=%.2fms budget=%.2fms overload=0x%X "
                    "sourceHealth=0x%X settingsChanged=0 policy=observe_and_preserve_cfr_audio",
                    ce::capture_policy::GetRecordingHealthStatus(recordingHealthState.flags),
                    ce::capture_policy::GetRecordingHealthCause(recordingHealthState.flags),
                    recordingHealthState.flags, recordingHealthState.currentDebtMs, recordingHealthState.peakDebtMs,
                    recordingHealthState.capacityAttributedDebtMs, outputFps, sustainableOutputFps,
                    smoothedEncodeMs, frameIntervalMs, overloadFlags, wgcCaptureHealthFlags);
            }

            // Flush the in-progress hold run into the histogram before logging,
            // but preserve the running count so it continues into the next interval.
            const uint32_t savedHoldTicks = cadenceCounters.holdTicksRunning;
            cadenceCounters.CommitHoldRun();

            // Compute input frame rate predictor diagnostics
            const InputFrameRatePredictor& activeInputPredictor =
                useScreenGrab ? wgcInputPredictor : injectInputPredictor;
            const uint32_t srcFpsX100Val =
                activeInputPredictor.IsCalibrated()
                    ? static_cast<uint32_t>(activeInputPredictor.GetPredictedFps(qpcFreq.QuadPart) * 100.0)
                    : 0u;
            const uint32_t srcJitterUsVal =
                activeInputPredictor.IsCalibrated()
                    ? static_cast<uint32_t>(activeInputPredictor.GetJitterUs(qpcFreq.QuadPart))
                    : 0u;
            const uint32_t dupTsPerSec = dupTimestampCount;
            const uint32_t currentWgcDuplicateTimestampSkipCount =
                media_main_g_WgcCap ? media_main_g_WgcCap->GetDuplicateTimestampSkipCount() : lastWgcDuplicateTimestampSkipCountForCadence;
            const uint32_t dupTsSkippedPerSec =
                currentWgcDuplicateTimestampSkipCount >= lastWgcDuplicateTimestampSkipCountForCadence
                    ? currentWgcDuplicateTimestampSkipCount - lastWgcDuplicateTimestampSkipCountForCadence
                    : currentWgcDuplicateTimestampSkipCount;
            lastWgcDuplicateTimestampSkipCountForCadence = currentWgcDuplicateTimestampSkipCount;
            dupTimestampCount = 0;
            encCycleMaxMs = 0;

            accumulateCaptureSummarySample(useScreenGrab, srcFpsX100Val, srcJitterUsVal, dupNoSource, dupDeferred,
                                           dupTimer, dupDrain, oldestBufferedFrameAgeUs, shortfallDurationMs,
                                           sustainableOutputFps);

            if (useScreenGrab && recordingOutputLive && media_main_g_WgcCap) {
                const uint32_t currentIngressAccepted = media_main_g_WgcCap->GetIngressAcceptedCount();
                if (!wgcRollingSourceWindowPrimed || currentIngressAccepted < wgcRollingSourceLastIngressAccepted) {
                    wgcRollingSourceLastIngressAccepted = currentIngressAccepted;
                    wgcRollingSourceAcceptedSlots.fill(0);
                    wgcRollingSourceCfrTickSlots.fill(0);
                    wgcRollingSourceSlotIndex = 0;
                    wgcRollingSourceSlotCount = 0;
                    wgcRollingSourceAcceptedSum = 0;
                    wgcRollingSourceCfrTickSum = 0;
                    wgcRollingSourceWindowPrimed = true;
                }
                wgcRollingSourceAcceptedWindow = currentIngressAccepted - wgcRollingSourceLastIngressAccepted;
                wgcRollingSourceLastIngressAccepted = currentIngressAccepted;
                wgcRollingSourceCfrTicksWindow = cadenceCounters.liveTickEmitCount;

                wgcRollingSourceAcceptedSum -= wgcRollingSourceAcceptedSlots[wgcRollingSourceSlotIndex];
                wgcRollingSourceCfrTickSum -= wgcRollingSourceCfrTickSlots[wgcRollingSourceSlotIndex];
                wgcRollingSourceAcceptedSlots[wgcRollingSourceSlotIndex] = wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickSlots[wgcRollingSourceSlotIndex] = wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceAcceptedSum += wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickSum += wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceSlotIndex = (wgcRollingSourceSlotIndex + 1u) % kWgcRollingSourceWindowSlots;
                wgcRollingSourceSlotCount = std::min(wgcRollingSourceSlotCount + 1u, kWgcRollingSourceWindowSlots);
                wgcRollingSourceAcceptedTotal += wgcRollingSourceAcceptedWindow;
                wgcRollingSourceCfrTickTotal += wgcRollingSourceCfrTicksWindow;
                wgcRollingSourceDeficitFrames = wgcRollingSourceCfrTickSum > wgcRollingSourceAcceptedSum
                                                    ? (wgcRollingSourceCfrTickSum - wgcRollingSourceAcceptedSum)
                                                    : 0u;
                wgcRollingSourceSurplusFrames = wgcRollingSourceAcceptedSum > wgcRollingSourceCfrTickSum
                                                    ? (wgcRollingSourceAcceptedSum - wgcRollingSourceCfrTickSum)
                                                    : 0u;
            } else {
                wgcRollingSourceWindowPrimed = false;
                wgcRollingSourceAcceptedWindow = 0;
                wgcRollingSourceCfrTicksWindow = 0;
                wgcRollingSourceDeficitFrames = 0;
                wgcRollingSourceSurplusFrames = 0;
                wgcRollingSourceAcceptedSum = 0;
                wgcRollingSourceCfrTickSum = 0;
                wgcRollingSourceSlotIndex = 0;
                wgcRollingSourceSlotCount = 0;
                wgcRollingSourceAcceptedSlots.fill(0);
                wgcRollingSourceCfrTickSlots.fill(0);
            }

            LogInfo(
                "[Cadence Health] Phase=%s | AgeAvg=%uus AgeMax=%uus | SelAvg=%uus SelMax=%uus SelBias=%dus "
                "EarlyMax=%uus LateMax=%uus | WgcSelAvg=%uus WgcSelMax=%uus WgcSelBias=%dus WgcEarly=%uus WgcLate=%uus "
                "Hold=%u HoldFresh=%u Delay=%u Spend=%u CatchUp=%u CatchFresh=%u InjectCatch=%u/%u "
                "InjectAgeTrim=%u PathMismatch=%u/%llu LiveClamp=%u/%uus | DefStreak=%u/%u "
                "DupStreak=%u/%u | DupSrc=%u "
                "DupDef=%u "
                "DupTimer=%u DupDrain=%u InjectDefReQ=%u InjectDefDrop=%u | TickEmit=%u TickUnique=%u TickDup=%u "
                "TickMiss=%u SourceWin=%u/%u SourceRoll=%u/%u SourceDef=%u SourceSur=%u | "
                "HoldHist=%u/%u/%u/%u/%u/%u | LiveWall=%lluus LiveTicks=%llu Shortfall=%u/%.1fms FreshMiss=%upm "
                "BufAvg=%upm BufMin=%u BufNow=%zu NoFresh=%u NoReserve=%u DelayRes=%u/%u LowTicks=%u "
                "DelayResidualAvg=%d/%uus DelayResidualMax=%uus DelayResidualP95=%uus DelayResidualLateMax=%uus "
                "DelayResidualWin=%d/%uus/%uus/%uus "
                "RawResidualAvg=%d/%uus RawResidualMax=%uus RawResidualP95=%uus RawResidualLateMax=%uus "
                "RawResidualWin=%d/%uus/%uus/%uus RawMinusPred=%dus/%uus RawMinusPredWin=%dus/%uus "
                "Oldest=%.1fms LeadExcess=%.1fms | "
                "WgcAct Fresh=%u "
                "DupSrc=%u DropObs=%u "
                "DropDebt=%u/%llu DebtMax=%uus SelMiss=%u StaleUni=%u "
                "Ancient=%u RepFreshMiss=%u RepHold=%u SyncHold=%u SyncHoldSrc=%u SyncHoldPolicy=%u "
                "TooNewLead=%uus RepCov=%u CovDelay=%u "
                "RepLate=%u RepCatch=%u | TsReg=%u "
                "TsStall=%u "
                "TimerRebase=%u WgcDebtMax=%llu WgcLiveRebase=%u/%llu/%u | "
                "EncLowBypass=%u/%llu ModeMis=%u/%llu SrcBack=%u/%llu | "
                "InvalidMeta=%u InvalidHandle=%u | PktClamp=%u NegPTS=%u NonMonoPTS=%u | WgcThr=%u Adj=%u | Over=0x%X "
                "MuxQ=%uKB/%u MuxBp=%u Wait=%uus Max=%uus | EncEma=%.2fms FreshSvcEma=%.2fms Budget=%upm "
                "Sust=%.1ffps TooSlow=%d "
                "Bottleneck=%d | LowSrc=%d Recover=%d Cause=S%d/D%d/E%d | SrcFps=%.2f SrcJitter=%uus "
                "DupTs=%u DupTsSkip=%u TsSmoothDev=%u/%u/%uus TsSmoothSnap=%u EncCycle=%.2fms EncSpike=%u",
                CapturePipelinePhaseToString(state.capturePhase.load(std::memory_order_relaxed)), avgFrameAgeUs,
                cadenceCounters.frameAgeMaxUs, avgSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs,
                avgSignedSelectionErrorUs, cadenceCounters.outputScheduleEarlyMaxUs,
                cadenceCounters.outputScheduleLateMaxUs, avgWgcSelectionErrorUs, wgcSelectionErrorMaxUs,
                avgSignedWgcSelectionErrorUs, wgcSelectionEarlyMaxUs, wgcSelectionLateMaxUs, wgcHoldForNextTickCount,
                wgcHeldFreshFrameTickCount, wgcSelectionDelayTickCount, wgcReserveSpendTickCount,
                cfrCatchupTicksExecuted, wgcFreshCatchupCount, injectFreshCatchupThisWindow,
                injectRepeatCatchupThisWindow, injectLiveStaleTrimThisWindow, activePathMismatchDiscardThisWindow,
                static_cast<unsigned long long>(media_main_g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
                wgcSelectionTargetClampCount, wgcSelectionTargetClampMaxUs, cadenceCounters.consecutiveDeferredFrames,
                cadenceCounters.maxConsecutiveDeferredFrames, cadenceCounters.consecutiveDuplicateFrames,
                cadenceCounters.maxConsecutiveDuplicateFrames, dupNoSource - lastDuplicateReasonNoSource,
                dupDeferred - lastDuplicateReasonDeferred, dupTimer - lastDuplicateReasonTimerRebase,
                dupDrain - lastDuplicateReasonDrain, injectDeferredRequeuedThisWindow, injectDeferredDroppedThisWindow,
                cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount,
                cadenceCounters.liveTickDuplicateCount, cadenceCounters.liveTickMissCount,
                wgcRollingSourceAcceptedWindow, wgcRollingSourceCfrTicksWindow, wgcRollingSourceAcceptedSum,
                wgcRollingSourceCfrTickSum, wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames,
                cadenceCounters.holdHist[0], cadenceCounters.holdHist[1], cadenceCounters.holdHist[2],
                cadenceCounters.holdHist[3], cadenceCounters.holdHist[4], cadenceCounters.holdHist[5],
                static_cast<unsigned long long>(liveWallElapsedUs), static_cast<unsigned long long>(liveTicksOutput),
                outputShortfallTicks, shortfallDurationMs, wgcNoFreshTickPermille, bufferedAtTickAvgPermille,
                bufferedAtTickMinValue, bufferedWgcFrames.size(), wgcNoFreshTickCount, wgcNoReserveTickCount,
                delayReservoirLowWaterFrames, delayReservoirTargetFrames, wgcDelayReservoirLowWaterTickCount,
                delayResidualSignedAvgUs, delayResidualAvgUs, wgcDelayResidualAbsMaxUs, delayResidualP95Us,
                wgcDelayResidualLateMaxUs, delayResidualWindowSignedAvgUs, delayResidualWindowAvgUs,
                delayResidualWindowP95Us, wgcDelayResidualWindowLateMaxUs, rawResidualSignedAvgUs, rawResidualAvgUs,
                wgcDelayRawResidualAbsMaxUs, rawResidualP95Us, wgcDelayRawResidualLateMaxUs,
                rawResidualWindowSignedAvgUs, rawResidualWindowAvgUs, rawResidualWindowP95Us,
                wgcDelayRawResidualWindowLateMaxUs, rawMinusPredictedAvgUs, wgcDelayRawMinusPredictedAbsMaxUs,
                rawMinusPredictedWindowAvgUs, wgcDelayRawMinusPredictedWindowAbsMaxUs, oldestBufferedFrameAgeMs,
                wgcAudioLeadExcessMsCurrent, wgcSelectFreshCount, wgcSelectDuplicateSourceCount, wgcDropObsoleteCount,
                wgcDropStaleDebtCount, static_cast<unsigned long long>(wgcDropStaleDebtTotal), wgcDropStaleDebtMaxUs,
                wgcFreshSelectionMissCount, wgcStaleUniqueFallbackCount, wgcAncientSelectionCount,
                wgcRepeatNoFreshCount, wgcRepeatPolicyHoldCount, wgcSyncDelayHoldCount,
                wgcSyncDelaySourceLimitedHoldCount, wgcSyncDelayPolicyHoldCount, wgcTooNewLeadMaxUs,
                wgcCoverageRepeatHoldCount, wgcCoverageDelayTicksCurrent, wgcRepeatTimerLateCount,
                wgcRepeatCatchupCount, tsRegress - lastTimestampRegressionCount, tsStall - lastTimestampStallCount,
                timerRebases, static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                wgcLiveSchedulerRebaseThisWindow, static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal),
                wgcLiveSchedulerRebaseMaxTicks, wgcEncoderLimitedSuppressedByLowSourceThisWindow,
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                wgcCapacityPressureModeMismatchThisWindow,
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                wgcSelectedSourceBacktrackThisWindow, static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal),
                invalidMeta - lastInvalidMetaCount, invalidHandle - lastInvalidHandleCount,
                packetClamps - lastPacketClampCount, negativePts - lastNegativePtsCount,
                nonMonotonicPts - lastNonMonotonicPtsCount, media_main_g_WgcProducerTargetFps.load(std::memory_order_relaxed),
                wgcProducerRateRetuneCount, overloadFlags, (muxQueueBytes + 1023u) / 1024u, muxQueuePackets,
                muxBackpressureCount, muxBackpressureWaitUs, muxBackpressureMaxWaitUs, smoothedEncodeMs,
                smoothedWgcFreshServiceMs, encoderBudgetUtilizationPermille, sustainableOutputFps,
                encoderTooSlowForTarget ? 1 : 0,
                media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
                wgcLiveRecoveryModeActive ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                wgcEncoderRecoveryLimitedCurrent ? 1 : 0, srcFpsX100Val / 100.0, srcJitterUsVal, dupTsPerSec,
                dupTsSkippedPerSec,
                wgcTsSmoothSamplesWindow > 0
                    ? SaturatingToUint32(wgcTsSmoothDevAccumUsWindow / wgcTsSmoothSamplesWindow)
                    : 0u,
                wgcTsSmoothDevMaxUsWindow, wgcTsSmoothDevMaxUsTotal, wgcTsSmoothSnapCountWindow, smoothedEncCycleMs,
                encodeSpikeCountThisSecond);
            wgcTsSmoothSamplesWindow = 0;
            wgcTsSmoothDevAccumUsWindow = 0;
            wgcTsSmoothDevMaxUsWindow = 0;
            wgcTsSmoothSnapCountWindow = 0;

            const bool wgcEncoderLimitedSmoothnessActive = isWgcEncoderLimitedSmoothnessMode();
            if (useScreenGrab && recordingOutputLive &&
                (wgcEncoderLimitedSmoothnessActive || wgcSourceStarvedCurrent || wgcSchedulerLimitedCurrent ||
                 outputShortfallTicks > 0 || wgcRepeatPolicyHoldCount > 0 || wgcDropStaleDebtCount > 0)) {
                ++wgcEncoderLimitedCadenceEventCount;
                const char* cadenceMode = wgcEncoderLimitedSmoothnessActive ? "encoder_limited"
                                          : wgcSourceStarvedCurrent         ? "source_starved"
                                          : wgcSchedulerLimitedCurrent      ? "scheduler_limited"
                                                                            : "normal_pressure";
                const uint32_t wgcSmoothnessRepeatsAvoidedWindow =
                    SaturatingToUint32(static_cast<uint64_t>(wgcDelayOlderFrameAvoidedRepeatWindow) +
                                       static_cast<uint64_t>(wgcDelayRepeatRescueSuccessWindow) +
                                       static_cast<uint64_t>(wgcDelayRepeatPromotedBeforeRepeatWindow));
                const double wgcSmoothnessActiveDelayMs =
                    static_cast<double>(qpcToUs(wgcSmoothnessActiveDelayQpc)) / 1000.0;
                const double wgcSmoothnessEstimatedVramMb =
                    static_cast<double>(wgcSmoothnessEstimatedVramBytes) / (1024.0 * 1024.0);
                const uint32_t wgcPoolFreeNow = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
                const uint32_t wgcPoolFreeMin = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
                const int64_t wgcWindowSmoothTargetUs =
                    qpcToUs(ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                        wgcSmoothnessRetainedFrames, targetIntervalTicks, getWgcSmoothnessOutputFps(),
                        config.wgcSmoothnessBufferMaxMs));
                const int64_t wgcWindowSmoothActualUs = qpcToUs(wgcSmoothnessActiveDelayQpc);
                const int64_t wgcWindowSmoothDeficitUs =
                    std::max<int64_t>(0, wgcWindowSmoothTargetUs - wgcWindowSmoothActualUs);
                const int64_t wgcWindowEffectiveDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
                const int64_t wgcWindowStartupDeficitUs =
                    std::max<int64_t>(0, wgcStartupDelayTargetUs - wgcWindowEffectiveDelayUs);
                LogInfo(
                    "[WGC CFR CADENCE EVENT] mode=%s shortfall=%u/%.1fms phaseErrorAvg=%dus "
                    "phaseErrorMax=%uus rebaseWindow=%u encoderDropWindow=%u encoderDropTotal=%llu "
                    "tooNewRepeat=%u syncDelayHold=%u syncDelaySourceHold=%u syncDelayPolicyHold=%u "
                    "tooNewLeadMax=%uus staleDrop=%u freshMiss=%upm bufNow=%zu oldest=%.1fms enc=%.2fms "
                    "sustain=%.1ffps overload=0x%X lowSourceBypass=%u modeMismatch=%u sourceBacktrack=%u "
                    "avDelay=%.1fms delayResidualAvg=%d/%uus delayResidualMax=%uus delayResidualP95=%uus "
                    "delayResidualLateMax=%uus delayResidualWin=%d/%uus delayResidualWinP95=%uus "
                    "rawResidualAvg=%d/%uus rawResidualMax=%uus rawResidualP95=%uus rawResidualLateMax=%uus "
                    "rawResidualWin=%d/%uus rawResidualWinP95=%uus rawMinusPredicted=%dus/%uus "

                    "postRejectSync=%u postRejectRescue=%u lowerBoundRepeat=%u excessRepeat=%u "
                    "policyAddedRepeat=%u excessRepeatCluster=%u/%u "
                    "sourceWindow=%u/%u sourceRolling=%u/%u sourceDeficit=%u sourceSurplus=%u "
                    "smoothBufMs=%.1f smoothFrames=%u/%u/%u "
                    "smoothSlots=%u retainedCap=%u reservedFreeSlots=%u retainedCapTrim=%u "
                    "poolFreeNow=%u poolFreeMin=%u poolPressureTrim=%u "
                    "smoothDeficit=%lldus startupDeficit=%lldus "
                    "smoothVramMB=%.1f smoothCap=%d smoothReason=%s "
                    "repeatsAvoided=%u repeatsUnavoidable=%u "
                    "reservoir=%u/%u lowTicks=%u "
                    "delayRelaxed=%u delayRelaxedRejectSync=%u repeatClusterPressure=%u/%u "
                    "delayRelaxedBetter=%u delayRelaxedCluster=%u delayRelaxedRejectHeadroom=%u "
                    "delayRelaxedRejectCost=%u softLateReject=%u softLateAccept=%u olderFrame=%u "
                    "sourceLimitRepeat=%u repeatRescue=%u/%u repeatPromote=%u/%u repeatPromoteSoft=%u "
                    "repeatSafeAfter=%u repeatSafe=%u/%u repeatSoftSafe=%u/%u repeatClass=%u/%u/%u "
                    "repeatReserve=%u/%uus hardOnly=%u syncProtected=%u nearCap=%u oldestSoftSafe=%uus "
                    "uniformCadence=%u uniformHold=%u delayPaceCapTrim=%u sourceRecovery=%u/%llu "
                    "cause=S%d/D%d/E%d",
                    cadenceMode, outputShortfallTicks, shortfallDurationMs, avgSignedWgcSelectionErrorUs,
                    wgcSelectionErrorMaxUs, wgcLiveSchedulerRebaseThisWindow, wgcEncoderLimitedSourceDropThisWindow,
                    static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcRepeatPolicyHoldCount,
                    wgcSyncDelayHoldCount, wgcSyncDelaySourceLimitedHoldCount, wgcSyncDelayPolicyHoldCount,
                    wgcTooNewLeadMaxUs, wgcDropStaleDebtCount, wgcNoFreshTickPermille, bufferedWgcFrames.size(),
                    oldestBufferedFrameAgeMs, smoothedEncodeMs, sustainableOutputFps, overloadFlags,
                    wgcEncoderLimitedSuppressedByLowSourceThisWindow, wgcCapacityPressureModeMismatchThisWindow,
                    wgcSelectedSourceBacktrackThisWindow,
                    static_cast<double>(qpcToUs(getWgcEffectiveContentDelayQpc())) / 1000.0, delayResidualSignedAvgUs,
                    delayResidualAvgUs, wgcDelayResidualAbsMaxUs, delayResidualP95Us, wgcDelayResidualLateMaxUs,
                    delayResidualWindowSignedAvgUs, delayResidualWindowAvgUs, delayResidualWindowP95Us,
                    rawResidualSignedAvgUs, rawResidualAvgUs, wgcDelayRawResidualAbsMaxUs, rawResidualP95Us,
                    wgcDelayRawResidualLateMaxUs, rawResidualWindowSignedAvgUs, rawResidualWindowAvgUs,
                    rawResidualWindowP95Us, rawMinusPredictedAvgUs, wgcDelayRawMinusPredictedAbsMaxUs,
                    wgcDelayPostSelectionRejectedSyncRiskWindow, wgcDelayPostSelectionRescuedSyncRiskWindow,
                    wgcSourceRepeatLowerBoundWindow, wgcExcessRepeatWindow, wgcPolicyAddedRepeatWindow,
                    wgcExcessRepeatClusterWindow, wgcExcessRepeatClusterWindowMaxTicks, wgcRollingSourceAcceptedWindow,
                    wgcRollingSourceCfrTicksWindow, wgcRollingSourceAcceptedSum, wgcRollingSourceCfrTickSum,
                    wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames, wgcSmoothnessActiveDelayMs,
                    wgcSmoothnessActualFrames, wgcSmoothnessRetainedFrames, wgcSmoothnessDesiredFrames,
                    wgcSmoothnessPoolSlots, wgcSmoothnessRetainedFrameCap, wgcSmoothnessReservedFreeSlots,
                    wgcRetainedCapTrimWindow, wgcPoolFreeNow, wgcPoolFreeMin, wgcPoolPressureTrimWindow,
                    static_cast<long long>(wgcWindowSmoothDeficitUs), static_cast<long long>(wgcWindowStartupDeficitUs),
                    wgcSmoothnessEstimatedVramMb, wgcSmoothnessCapLimited ? 1 : 0, getWgcSmoothnessBufferReason(),
                    wgcSmoothnessRepeatsAvoidedWindow, wgcSourceRepeatLowerBoundWindow, delayReservoirLowWaterFrames,
                    delayReservoirTargetFrames, wgcDelayReservoirLowWaterTickCount, wgcDelayRelaxedSelectionWindowCount,
                    wgcDelayRelaxedRejectedSyncRiskWindow, wgcDelayRepeatClusterPressureWindow,
                    wgcDelayRepeatClusterPressureWindowMaxTicks, wgcDelayRelaxedBetterTargetWindow,
                    wgcDelayRelaxedRepeatClusterWindow, wgcDelayRelaxedRejectedResidualHeadroomWindow,
                    wgcDelayRelaxedRejectedRepeatCostWindow, wgcDelaySoftLateRejectedWindow,
                    wgcDelaySoftLateAcceptedWindow, wgcDelayOlderFrameAvoidedRepeatWindow,
                    wgcDelaySourceLimitedRepeatWindow, wgcDelayRepeatRescueSuccessWindow,
                    wgcDelayRepeatRescueAttemptWindow, wgcDelayRepeatPromotedBeforeRepeatWindow,
                    wgcDelayRepeatPromotionAttemptWindow, wgcDelayRepeatPromotionRejectedSoftWindow,
                    wgcDelayRepeatSafeAfterPromotionWindow, wgcDelayRepeatWithSafeCandidateWindow,
                    wgcDelayRepeatWithoutSafeCandidateWindow, wgcDelayRepeatWithSoftSafeCandidateWindow,
                    wgcDelayRepeatWithoutSoftSafeCandidateWindow, wgcDelayWindowHealthyRepeatWindow,
                    wgcDelayWindowRecoverableRepeatWindow, wgcDelayWindowSourceLimitedRepeatWindow,
                    wgcDelayRepeatReserveDepthWindowMax, wgcDelayRepeatReserveSpanWindowMaxUs,
                    wgcDelayRepeatHardOnlyCandidateWindow, wgcDelaySyncProtectedRepeatWindow,
                    wgcDelayNearCapAcceptedWindow, wgcDelayOldestSoftSafeAgeWindowMaxUs, wgcDelayUniformCadenceWindow,
                    wgcDelayUniformHoldWindow, wgcDelayPaceCapTrimWindow, wgcSyncDelaySourceRecoveryHoldCount,
                    static_cast<unsigned long long>(wgcSyncDelaySourceRecoveryHoldTotal),
                    wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                    wgcEncoderRecoveryLimitedCurrent ? 1 : 0);
                // Compact per-window jitter-budget view: is the (audio-latency OR floor) buffer absorbing
                // bursty WGC delivery, or is jitter overflowing it into even repeats? bufNow staying at/above
                // the reservoir low-water with bounded windowResidualLate means absorbed; bufNow draining to
                // 0 with uniformHold repeats means the delivery burst exceeded the buffer depth (overflow ->
                // even repeats, NOT a sync fault). Source-limited repeats are attributed separately.
                if (isWgcEffectiveContentDelayActive()) {
                    const int64_t jbDeliveryGapAvgUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t jbDeliveryGapMaxUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t jbSourceJitterAvgUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t jbSourceJitterMaxUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetSourceJitterMaxUs() : 0;
                    const bool jbAbsorbing = bufferedWgcFrames.size() >= delayReservoirLowWaterFrames;
                    LogInfo(
                        "[WGC CFR JITTER BUDGET] floorSource=%s effectiveDelayUs=%lld floorTargetUs=%lld "
                        "bufNow=%zu reservoir=%u/%u deliveryGapUs(avg/max)=%lld/%lld "
                        "sourceJitterUs(avg/max)=%lld/%lld windowResidualLateMaxUs=%u windowResidualP95Us=%u "
                        "uniformHoldRepeats=%u sourceLimitedRepeats=%u paceCapTrim=%u absorbing=%d",
                        avContentDelayActive ? "audio" : wgcSmoothnessFloorSource,
                        static_cast<long long>(wgcWindowEffectiveDelayUs),
                        static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), bufferedWgcFrames.size(),
                        delayReservoirLowWaterFrames, delayReservoirTargetFrames,
                        static_cast<long long>(jbDeliveryGapAvgUs), static_cast<long long>(jbDeliveryGapMaxUs),
                        static_cast<long long>(jbSourceJitterAvgUs), static_cast<long long>(jbSourceJitterMaxUs),
                        wgcDelayResidualLateMaxUs, delayResidualWindowP95Us, wgcDelayUniformHoldWindow,
                        wgcDelaySourceLimitedRepeatWindow, wgcDelayPaceCapTrimWindow, jbAbsorbing ? 1 : 0);
                }
            }

            static uint64_t s_lastWgcCapacityWarnTick = 0;
            static uint64_t s_lastWgcSourceLimitedInfoTick = 0;
            static uint32_t s_wgcCapacityLimitedStreakSeconds = 0;
            if (useScreenGrab && recordingOutputLive) {
                const bool encoderPressure = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
                                             (overloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
                                             smoothedEncodeMs >= frameIntervalMs;
                const bool muxPressure =
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0 || muxBackpressureWaitUs > 0;
                const bool captureLimitedForOverlay =
                    ce::capture_policy::IsWgcCaptureLimitedForOverlay(wgcCaptureHealthFlags);
                const bool hardCapacityPressure = muxPressure || (encoderPressure && !captureLimitedForOverlay);
                const bool capacityLimitedThisSecond =
                    hardCapacityPressure && (outputShortfallTicks > 0 || oldestBufferedFrameAgeUs > 0);
                s_wgcCapacityLimitedStreakSeconds =
                    capacityLimitedThisSecond ? (s_wgcCapacityLimitedStreakSeconds + 1) : 0;
                const uint64_t nowTick = GetTickCount64();
                const bool transientStartupEncoderPressure =
                    !muxPressure &&
                    ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, nowTick) &&
                    s_wgcCapacityLimitedStreakSeconds < 2;
                if (hardCapacityPressure && !transientStartupEncoderPressure &&
                    (nowTick - s_lastWgcCapacityWarnTick) >= 5000) {
                    const char* limiter = encoderPressure && muxPressure ? "encoder+mux"
                                          : encoderPressure              ? "encoder"
                                                                         : "mux";
                    const char* warningPrefix =
                        encoderTooSlowForTarget ? "Encoder cannot sustain target" : "Output limited";
                    LogWarn(
                        "[WGC CFR] %s (%s): target=%ufps sustain=%.1ffps encode=%.2fms budget=%.2fms util=%upm "
                        "shortfall=%u/%.1fms oldest=%.1fms streak=%us muxQ=%uKB/%u muxWait=%uus noFresh=%upm",
                        warningPrefix, limiter, outputFps, sustainableOutputFps, smoothedEncodeMs, frameIntervalMs,
                        encoderBudgetUtilizationPermille, outputShortfallTicks, shortfallDurationMs,
                        oldestBufferedFrameAgeMs, s_wgcCapacityLimitedStreakSeconds, (muxQueueBytes + 1023u) / 1024u,
                        muxQueuePackets, muxBackpressureWaitUs, wgcNoFreshTickPermille);
                    s_lastWgcCapacityWarnTick = nowTick;
                }
                const bool sourceStarvedPressure =
                    wgcSourceStarvedCurrent ||
                    (wgcNoFreshTickPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille &&
                     wgcRecentInputMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps);
                if (sourceStarvedPressure && (nowTick - s_lastWgcSourceLimitedInfoTick) >= 5000) {
                    const int64_t wgcInfoCallbackGapAvgUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t wgcInfoCallbackGapMaxUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t wgcInfoSourceJitterAvgUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t wgcInfoSourceJitterMaxUs = media_main_g_WgcCap ? media_main_g_WgcCap->GetSourceJitterMaxUs() : 0;
                    const uint32_t wgcInfoPoolFreeMin = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
                    const uint32_t wgcInfoPoolSaturatedDrops = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSaturatedDropCount() : 0u;
                    const uint32_t wgcInfoIngressHard = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressHardReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressSoft = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressDecimated = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u;
                    const bool wgcInfoPoolPressure =
                        wgcInfoPoolSaturatedDrops > 0 || wgcInfoIngressHard > 0 || wgcInfoPoolFreeMin == 0;
                    const bool wgcInfoCleanCe = !encoderPressure && !muxPressure && !wgcInfoPoolPressure &&
                                                wgcExcessRepeatWindow == 0 && wgcPolicyAddedRepeatWindow == 0 &&
                                                wgcDelayPostSelectionRejectedSyncRiskWindow == 0;
                    const char* wgcInfoCoverageReason =
                        wgcSchedulerLimitedCurrent ? "wgc_delivery_gap"
                        : wgcSourceStarvedCurrent  ? "source_or_delivery_underfeed"
                        : wgcRecentDeliveredMin250Fps + ce::capture_policy::kWgcRecoverySourceMarginFps < outputFps
                            ? "delivered_below_cfr_target"
                            : "no_fresh_source_for_cfr_slots";
                    LogInfo(
                        "[WGC CFR] CFR source-coverage repeats: reason=%s target=%ufps input=%u/%u "
                        "delivered=%u/%u freshMiss=%upm buffered=%u oldest=%.1fms shortfall=%u/%.1fms "
                        "duplicates=%u lowerBound=%u excess=%u policyAdded=%u cleanCE=%d cause=S%d/D%d/E%d "
                        "encoderPressure=%d muxPressure=%d poolPressure=%d poolFreeMin=%u poolSat=%u "
                        "ingressHard=%u ingressSoft=%u ingressDec=%u callbackGapUs(avg/max)=%lld/%lld "
                        "sourceJitterUs(avg/max)=%lld/%lld overlayEncoderWarn=%d "
                        "note=cfr_repeats_mean_no_sync_safe_fresh_source_for_some_slots",
                        wgcInfoCoverageReason, outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps,
                        wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcNoFreshTickPermille,
                        bufferedAtTickMinValue, oldestBufferedFrameAgeMs, outputShortfallTicks, shortfallDurationMs,
                        cadenceCounters.liveTickDuplicateCount, wgcSourceRepeatLowerBoundWindow, wgcExcessRepeatWindow,
                        wgcPolicyAddedRepeatWindow, wgcInfoCleanCe ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0,
                        wgcSchedulerLimitedCurrent ? 1 : 0, wgcEncoderRecoveryLimitedCurrent ? 1 : 0,
                        encoderPressure ? 1 : 0, muxPressure ? 1 : 0, wgcInfoPoolPressure ? 1 : 0, wgcInfoPoolFreeMin,
                        wgcInfoPoolSaturatedDrops, wgcInfoIngressHard, wgcInfoIngressSoft, wgcInfoIngressDecimated,
                        static_cast<long long>(wgcInfoCallbackGapAvgUs),
                        static_cast<long long>(wgcInfoCallbackGapMaxUs),
                        static_cast<long long>(wgcInfoSourceJitterAvgUs),
                        static_cast<long long>(wgcInfoSourceJitterMaxUs),
                        ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, wgcCaptureHealthFlags,
                                                                       recordingHealthState.flags) ==
                                ce::capture_policy::kOverlayWarningEncoderOverload
                            ? 1
                            : 0);
                    s_lastWgcSourceLimitedInfoTick = nowTick;
                }
            } else if (!useScreenGrab && recordingOutputLive) {
                s_wgcCapacityLimitedStreakSeconds = 0;
                static uint64_t s_lastInjectRepeatPressureInfoTick = 0;
                const uint32_t duplicateTicksThisWindow = cadenceCounters.liveTickDuplicateCount;
                const uint32_t deferredRepeatsThisWindow = dupDeferred - lastDuplicateReasonDeferred;
                const uint32_t sourceRepeatsThisWindow = dupNoSource - lastDuplicateReasonNoSource;
                const bool hardEncoderPressure =
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
                    (overloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0 ||
                    media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
                const uint64_t nowTick = GetTickCount64();
                if ((duplicateTicksThisWindow > 0 || injectDeferredRequeuedThisWindow > 0 ||
                     injectFreshCatchupThisWindow > 0 || injectLiveStaleTrimThisWindow > 0 ||
                     injectTargetSupersededThisWindow > 0 || injectTargetHoldWithCandidateThisWindow > 0) &&
                    (nowTick - s_lastInjectRepeatPressureInfoTick) >= 5000) {
                    LogInfo(
                        "[Inject CFR] Repeat pressure: hardEncoderOverload=%d dup=%u srcLimited=%u fenceDeferred=%u "
                        "timer=%u freshCatchup=%u repeatCatchup=%u staleTrim=%u recovery=%d/%u requeued=%u "
                        "droppedDeferred=%u targetSelect=%u targetSuperseded=%u targetHold=%u "
                        "holdWithCandidate=%u targetResidualMax=%uus "
                        "tickEmit=%u unique=%u sourceFps=%.2f enc=%.2fms service=%.2fms cycle=%.2fms "
                        "sustain=%.1ffps overload=0x%X",
                        hardEncoderPressure ? 1 : 0, duplicateTicksThisWindow, sourceRepeatsThisWindow,
                        deferredRepeatsThisWindow, dupTimer - lastDuplicateReasonTimerRebase,
                        injectFreshCatchupThisWindow, injectRepeatCatchupThisWindow, injectLiveStaleTrimThisWindow,
                        injectCfrRecoveryActive ? 1 : 0, injectCfrRecoveryEpisodesThisWindow,
                        injectDeferredRequeuedThisWindow, injectDeferredDroppedThisWindow,
                        injectTargetSelectThisWindow, injectTargetSupersededThisWindow,
                        injectTargetHoldThisWindow, injectTargetHoldWithCandidateThisWindow,
                        injectTargetResidualMaxUs,
                        cadenceCounters.liveTickEmitCount, cadenceCounters.liveTickUniqueCount, srcFpsX100Val / 100.0,
                        smoothedEncodeMs, smoothedInjectServiceMs, smoothedEncCycleMs, sustainableOutputFps,
                        overloadFlags);
                    s_lastInjectRepeatPressureInfoTick = nowTick;
                }
            } else {
                s_wgcCapacityLimitedStreakSeconds = 0;
            }

            lastDuplicateReasonNoSource = dupNoSource;
            lastDuplicateReasonDeferred = dupDeferred;
            lastDuplicateReasonTimerRebase = dupTimer;
            lastDuplicateReasonDrain = dupDrain;
            lastInvalidMetaCount = invalidMeta;
            lastInvalidHandleCount = invalidHandle;
            lastTimestampRegressionCount = tsRegress;
            lastTimestampStallCount = tsStall;
            lastPacketClampCount = packetClamps;
            lastNegativePtsCount = negativePts;
            lastNonMonotonicPtsCount = nonMonotonicPts;
            injectDeferredRequeuedThisWindow = 0;
            injectDeferredDroppedThisWindow = 0;
            injectFreshCatchupThisWindow = 0;
            injectRepeatCatchupThisWindow = 0;
            injectLiveStaleTrimThisWindow = 0;
            injectTargetSelectThisWindow = 0;
            injectTargetSupersededThisWindow = 0;
            injectTargetHoldThisWindow = 0;
            injectTargetHoldWithCandidateThisWindow = 0;
            injectCfrRecoveryEpisodesThisWindow = 0;
            activePathMismatchDiscardThisWindow = 0;
            cadenceCounters.Reset();
            cadenceCounters.holdTicksRunning = savedHoldTicks;  // Preserve in-progress hold run
            wgcSelectionErrorAccumUs = 0;
            wgcSelectionErrorSignedAccumUs = 0;
            wgcSelectionErrorSamples = 0;
            wgcSelectionErrorMaxUs = 0;
            wgcSelectionEarlyMaxUs = 0;
            wgcSelectionLateMaxUs = 0;
            wgcSelectionTargetClampCount = 0;
            wgcSelectionTargetClampMaxUs = 0;
            wgcHoldForNextTickCount = 0;
            wgcHeldFreshFrameTickCount = 0;
            wgcSelectionDelayTickCount = 0;
            wgcSyncDelayHoldCount = 0;
            wgcSyncDelaySourceLimitedHoldCount = 0;
            wgcSyncDelayPolicyHoldCount = 0;
            wgcTooNewLeadMaxUs = 0;
            wgcDelayResidualWindowSamples = 0;
            wgcDelayResidualWindowAbsAccumUs = 0;
            wgcDelayResidualWindowSignedAccumUs = 0;
            wgcDelayResidualWindowAbsMaxUs = 0;
            wgcDelayResidualWindowLateMaxUs = 0;
            wgcDelayResidualWindowAbsHistogram.fill(0);
            wgcDelayRawResidualWindowSamples = 0;
            wgcDelayRawResidualWindowAbsAccumUs = 0;
            wgcDelayRawResidualWindowSignedAccumUs = 0;
            wgcDelayRawResidualWindowAbsMaxUs = 0;
            wgcDelayRawResidualWindowLateMaxUs = 0;
            wgcDelayRawResidualWindowAbsHistogram.fill(0);
            wgcDelayRawMinusPredictedWindowSamples = 0;
            wgcDelayRawMinusPredictedWindowSignedAccumUs = 0;
            wgcDelayRawMinusPredictedWindowAbsMaxUs = 0;
            wgcDelayRelaxedSelectionWindowCount = 0;
            wgcDelayRelaxedBetterTargetWindow = 0;
            wgcDelayRelaxedRepeatClusterWindow = 0;
            wgcDelayRelaxedRejectedSyncRiskWindow = 0;
            wgcDelayRelaxedRejectedResidualHeadroomWindow = 0;
            wgcDelayRelaxedRejectedRepeatCostWindow = 0;
            wgcDelaySoftLateRejectedWindow = 0;
            wgcDelaySoftLateAcceptedWindow = 0;
            wgcDelayNearCapAcceptedWindow = 0;
            wgcDelayUniformCadenceWindow = 0;
            wgcDelayUniformHoldWindow = 0;
            wgcDelayPaceCapTrimWindow = 0;
            wgcRetainedCapTrimWindow = 0;
            wgcPoolPressureTrimWindow = 0;
            wgcDelayOlderFrameAvoidedRepeatWindow = 0;
            wgcDelaySourceLimitedRepeatWindow = 0;
            wgcDelayRepeatRescueAttemptWindow = 0;
            wgcDelayRepeatRescueSuccessWindow = 0;
            wgcDelayRepeatRescueRejectedSyncWindow = 0;
            wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
            wgcDelayRepeatRescueRejectedCostWindow = 0;
            wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
            wgcDelayRepeatPromotionAttemptWindow = 0;
            wgcDelayRepeatPromotionRejectedSoftWindow = 0;
            wgcDelayRepeatSafeAfterPromotionWindow = 0;
            wgcDelayRepeatWithSafeCandidateWindow = 0;
            wgcDelayRepeatWithoutSafeCandidateWindow = 0;
            wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
            wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
            wgcDelayRepeatHardOnlyCandidateWindow = 0;
            wgcDelaySyncProtectedRepeatWindow = 0;
            wgcDelayWindowHealthyRepeatWindow = 0;
            wgcDelayWindowRecoverableRepeatWindow = 0;
            wgcDelayWindowSourceLimitedRepeatWindow = 0;
            wgcDelayWindowHardStallRepeatWindow = 0;
            wgcDelayWindowPostStallRepeatWindow = 0;
            wgcDelayPostStallSafeFrameWindow = 0;
            wgcDelayRepeatReserveDepthWindowMax = 0;
            wgcDelayRepeatReserveSpanWindowMaxUs = 0;
            wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
            wgcDelayPostSelectionRejectedSyncRiskWindow = 0;
            wgcDelayPostSelectionRescuedSyncRiskWindow = 0;
            wgcDelayRepeatClusterPressureWindow = 0;
            wgcDelayRepeatClusterPressureWindowMaxTicks = 0;
            wgcSourceRepeatLowerBoundWindow = 0;
            wgcExcessRepeatWindow = 0;
            wgcPolicyAddedRepeatWindow = 0;
            wgcExcessRepeatClusterWindow = 0;
            wgcExcessRepeatClusterWindowMaxTicks = 0;
            wgcSyncDelaySourceRecoveryHoldCount = 0;
            cfrCatchupTicksExecuted = 0;
            wgcFreshCatchupCount = 0;
            wgcReserveSpendTickCount = 0;
            wgcProducerRateRetuneCount = 0;
            wgcNoFreshTickCount = 0;
            wgcDelayReservoirLowWaterTickCount = 0;
            wgcQueueTickSampleCount = 0;
            wgcNoFreshTickPermille = 0;
            wgcBufferedAtTickSum = 0;
            wgcBufferedAtTickMin = UINT32_MAX;
            wgcNoReserveTickCount = 0;
            wgcAncientSelectionCount = 0;
            wgcFreshSelectionMissCount = 0;
            wgcStaleUniqueFallbackCount = 0;
            wgcRepeatNoFreshCount = 0;
            wgcRepeatPolicyHoldCount = 0;
            wgcCoverageRepeatHoldCount = 0;
            wgcCoverageDelayTicksCurrent = 0;
            wgcRepeatTimerLateCount = 0;
            wgcRepeatCatchupCount = 0;
            wgcSelectFreshCount = 0;
            wgcSelectDuplicateSourceCount = 0;
            wgcDropObsoleteCount = 0;
            wgcDropStaleDebtCount = 0;
            wgcDropStaleDebtMaxUs = 0;
            wgcEncoderLimitedSourceDropThisWindow = 0;
            wgcEncoderLimitedSuppressedByLowSourceThisWindow = 0;
            wgcCapacityPressureModeMismatchThisWindow = 0;
            wgcSelectedSourceBacktrackThisWindow = 0;
            wgcLiveSchedulerRebaseThisWindow = 0;
            lastHealthLog = GetTickCount();
        }
}
