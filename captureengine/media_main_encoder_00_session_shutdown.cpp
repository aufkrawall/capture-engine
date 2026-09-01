#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::Shutdown() {

    if (hTimer) {
        CloseHandle(hTimer);
    }

    if (!bufferedWgcFrames.empty()) {
        ClearBufferedWgcFrames();
    }

    if (media_main_g_pSharedMem && liveTicksOutput > 0) {
        auto& state = media_main_g_pSharedMem->runtimeState;
        const bool useScreenGrab = IsActiveScreenGrab();
        const uint32_t dupNoSource = state.duplicateFramesNoSource.load(std::memory_order_relaxed);
        const uint32_t dupDeferred = state.duplicateFramesDeferred.load(std::memory_order_relaxed);
        const uint32_t dupTimer = state.duplicateFramesTimerRebase.load(std::memory_order_relaxed);
        const uint32_t dupDrain = state.duplicateFramesDrain.load(std::memory_order_relaxed);
        const uint32_t oldestBufferedFrameAgeUs = state.oldestBufferedFrameAgeUs.load(std::memory_order_relaxed);
        uint32_t outputShortfallTicks = 0;
        if (recordingOutputLive && liveStartQpc.QuadPart > 0 && targetIntervalTicks > 0 && liveTicksScheduled > 0) {
            LARGE_INTEGER nowQpc;
            QueryPerformanceCounter(&nowQpc);
            if (nowQpc.QuadPart > liveStartQpc.QuadPart) {
                outputShortfallTicks = updateLiveCfrShortfall(nowQpc.QuadPart);
            }
        }
        const double shortfallDurationMs =
            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
        const uint32_t finalOverloadFlags = state.encoderOverloadFlags.load(std::memory_order_relaxed);
        const bool finalEncoderPressure =
            media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
            (finalOverloadFlags & ce::capture_policy::kEncoderOverloadFlagEncoder) != 0 ||
            smoothedEncodeMs >= frameIntervalMs;
        const bool finalMuxPressure =
            (finalOverloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0;
        recordingHealthState = ce::capture_policy::UpdateRecordingHealth(
            recordingHealthState,
            {recordingOutputLive, !config.video.useVFR, finalEncoderPressure, finalMuxPressure,
             SaturatingToUint32(static_cast<uint64_t>(std::max(0.0, std::ceil(shortfallDurationMs))))});
        PublishRecordingHealth(recordingHealthState);
        const double sustainableOutputFps = ce::capture_policy::GetEncoderSustainableOutputFps(smoothedEncodeMs);
        const InputFrameRatePredictor& activeInputPredictor = useScreenGrab ? wgcInputPredictor : injectInputPredictor;
        const uint32_t srcFpsX100Val =
            activeInputPredictor.IsCalibrated()
                ? static_cast<uint32_t>(activeInputPredictor.GetPredictedFps(qpcFreq.QuadPart) * 100.0)
                : 0u;
        const uint32_t srcJitterUsVal = activeInputPredictor.IsCalibrated()
                                            ? static_cast<uint32_t>(activeInputPredictor.GetJitterUs(qpcFreq.QuadPart))
                                            : 0u;
        accumulateCaptureSummarySample(useScreenGrab, srcFpsX100Val, srcJitterUsVal, dupNoSource, dupDeferred, dupTimer,
                                       dupDrain, oldestBufferedFrameAgeUs, shortfallDurationMs, sustainableOutputFps);
    }

    if (wgcStarvedEpisode.active) {
        const uint64_t durationMs = GetTickCount64() - wgcStarvedEpisode.startTickMs;
        const uint64_t outputTicks = liveTicksOutput - wgcStarvedEpisode.startLiveTicks;
        const uint64_t duplicateTicks = captureSessionSummary.duplicateTicks - wgcStarvedEpisode.startDuplicateTicks;
        finishWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks);
    }

    if (liveTicksOutput > 0) {
        const uint64_t duplicatePermille = (captureSessionSummary.duplicateTicks * 1000ull) / liveTicksOutput;
        const bool summaryUsesScreenGrab = IsActiveScreenGrab();
        if (summaryUsesScreenGrab) {
            const uint64_t noFreshPermille =
                captureSessionSummary.queueTickSamples > 0
                    ? (captureSessionSummary.noFreshTicks * 1000ull) / captureSessionSummary.queueTickSamples
                    : 0ull;
            const uint64_t noReservePermille =
                captureSessionSummary.queueTickSamples > 0
                    ? (captureSessionSummary.noReserveTicks * 1000ull) / captureSessionSummary.queueTickSamples
                    : 0ull;
            LogInfo(
                "[WGC CFR SUMMARY] Live=%llu Dup=%llu DupPct=%.1f%% NoFresh=%llupm NoReserve=%llupm Pacer=%llu "
                "DupReason(src=%llu def=%llu timer=%llu drain=%llu) SourceLimitedRepeats=%llu StarvedEpisodes=%llu "
                "GridDebtSyncHolds=%llu GridDebtLeadMax=%lluus BiasClampCount=%llu longest=%llums longestDup=%llu "
                "longestContiguousDup=%llu (%llums) "
                "worstIn=%u "
                "worstDel=%u",
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<double>(duplicatePermille) / 10.0, static_cast<unsigned long long>(noFreshPermille),
                static_cast<unsigned long long>(noReservePermille),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.emittedRepeats),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDeferredTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTimerTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDrainTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.starvedEpisodes),
                static_cast<unsigned long long>(wgcUniformGridDebtHoldTotal),
                static_cast<unsigned long long>(wgcUniformGridDebtLeadMaxUs),
                static_cast<unsigned long long>(wgcBiasClampCount),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeMs),
                static_cast<unsigned long long>(captureSessionSummary.longestStarvedEpisodeDuplicateTicks),
                static_cast<unsigned long long>(captureSessionSummary.longestContiguousDupTicks),
                static_cast<unsigned long long>(static_cast<double>(captureSessionSummary.longestContiguousDupTicks) *
                                                frameIntervalMs),
                captureSessionSummary.longestStarvedEpisodeMinInputFps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.longestStarvedEpisodeMinInputFps,
                captureSessionSummary.longestStarvedEpisodeMinDeliveredFps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.longestStarvedEpisodeMinDeliveredFps);
            LogInfo(
                "[WGC CFR SUMMARY] SourceFps=%.2f..%.2f MinIn250=%u MinDel250=%u FreshMissMax=%upm JitterMax=%uus "
                "SelMax=%uus WgcSelMax=%uus Oldest=%.1fms ShortfallMax=%.1fms EncEmaMax=%.2fms SustainMin=%.1ffps "
                "LowSrcImmediate=%u StaleDebtDrop=%llu TimelineDebtDrop=%llu LiveRebase=%llu/%u "
                "EncoderDrop=%llu/%u PostStopDrop=%llu/%uus",
                captureSessionSummary.worstSourceFpsX100 == std::numeric_limits<uint32_t>::max()
                    ? 0.0
                    : (captureSessionSummary.worstSourceFpsX100 / 100.0),
                captureSessionSummary.bestSourceFpsX100 / 100.0,
                captureSessionSummary.worstInputMin250Fps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.worstInputMin250Fps,
                captureSessionSummary.worstDeliveredMin250Fps == std::numeric_limits<uint32_t>::max()
                    ? 0u
                    : captureSessionSummary.worstDeliveredMin250Fps,
                captureSessionSummary.worstFreshMissPermille, captureSessionSummary.worstSourceJitterUs,
                captureSessionSummary.worstSelectionErrorUs, captureSessionSummary.worstWgcSelectionErrorUs,
                static_cast<double>(captureSessionSummary.worstOldestBufferedFrameAgeUs) / 1000.0,
                captureSessionSummary.maxShortfallDurationMs, captureSessionSummary.maxEncodeEmaMs,
                captureSessionSummary.minEncoderSustainFps == std::numeric_limits<double>::max()
                    ? 0.0
                    : captureSessionSummary.minEncoderSustainFps,
                captureSessionSummary.lowSourceImmediateExits, static_cast<unsigned long long>(wgcDropStaleDebtTotal),
                static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcEncoderLimitedSourceDropMaxTicks,
                static_cast<unsigned long long>(wgcPostStopFrameDropTotal), wgcPostStopFrameDropMaxUs);
            LogInfo(
                "[WGC CFR RECOVERY] GridFresh=%llu Held=%llu FreshSvcEma=%.2fms ServiceHeadroomRatio=%.2f "
                "PtsGrid=immutable AudioTimeline=unchanged",
                static_cast<unsigned long long>(wgcFreshCatchupTotal),
                static_cast<unsigned long long>(wgcRepeatCatchupTotal), smoothedWgcFreshServiceMs,
                ce::capture_policy::kWgcFreshCatchupServiceBudgetRatio);
            LogInfo(
                "[WGC CFR OVERLOAD PACER] Episodes=%llu RepeatDecisions=%llu Probes=%llu Emitted=%llu FreshGrants=%llu MaxRepeatRun=%u "
                "MinFreshFraction=%.3f FreshSvcEma=%.2fms/%u RepeatSvcEma=%.2fms/%u BudgetRatio=%.2f "
                "PtsGrid=immutable AudioTimeline=unchanged",
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.episodes),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.probeRepeats),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.emittedRepeats),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.freshGrants),
                wgcOverloadRepeatPacer.maxConsecutiveProactiveRepeats,
                wgcOverloadRepeatPacer.minimumFreshFraction, smoothedWgcFreshServiceMs, wgcFreshServiceSamples,
                smoothedWgcRepeatServiceMs, wgcRepeatServiceSamples,
                ce::capture_policy::kWgcOverloadRepeatPacerBudgetRatio);
            const int64_t wgcAvSyncStartupDelayUs =
                qpcFreq.QuadPart > 0 && wgcAvSyncStartupEffectiveDelayQpc > 0
                    ? (wgcAvSyncStartupEffectiveDelayQpc * 1000000) / qpcFreq.QuadPart
                    : 0;
            const int64_t wgcAvSyncScheduleOffsetUs =
                qpcFreq.QuadPart > 0 ? (wgcAvSyncScheduleOffsetQpc * 1000000) / qpcFreq.QuadPart : 0;
            const uint32_t wgcDelayResidualAvgAbsUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const int32_t wgcDelayResidualAvgSignedUs =
                wgcDelayResidualSamples > 0 ? static_cast<int32_t>(wgcDelayResidualSignedAccumUs /
                                                                   static_cast<int64_t>(wgcDelayResidualSamples))
                                            : 0;
            const uint32_t wgcDelayRealizedAvgUs =
                wgcDelayResidualSamples > 0 ? SaturatingToUint32(wgcDelayRealizedAccumUs / wgcDelayResidualSamples)
                                            : 0u;
            const uint32_t wgcDelayRealizedMinFinalUs =
                wgcDelayResidualSamples > 0 && wgcDelayRealizedMinUs != UINT32_MAX ? wgcDelayRealizedMinUs : 0u;
            const uint32_t wgcDelayRawResidualAvgAbsUs =
                wgcDelayRawResidualSamples > 0
                    ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                    : 0u;
            const int32_t wgcDelayRawResidualAvgSignedUs =
                wgcDelayRawResidualSamples > 0 ? static_cast<int32_t>(wgcDelayRawResidualSignedAccumUs /
                                                                      static_cast<int64_t>(wgcDelayRawResidualSamples))
                                               : 0;
            const int32_t wgcDelayRawMinusPredictedAvgSignedUs =
                wgcDelayRawMinusPredictedSamples > 0
                    ? static_cast<int32_t>(wgcDelayRawMinusPredictedSignedAccumUs /
                                           static_cast<int64_t>(wgcDelayRawMinusPredictedSamples))
                    : 0;
            const bool wgcActiveDelayMixedPolicyFault = ce::capture_policy::IsWgcActiveDelayMixedPolicyPressureFault(
                SaturatingToUint32(wgcSyncDelaySourceLimitedHoldTotal), SaturatingToUint32(wgcSyncDelayPolicyHoldTotal),
                SaturatingToUint32(wgcSyncDelayHoldTotal));
            const uint64_t wgcPolicyNoSourceRepeats =
                std::min<uint64_t>(captureSessionSummary.duplicateNoSourceTicks, wgcSyncDelayPolicyHoldTotal);
            const uint64_t wgcDeliveryRepeatLowerBoundTotal =
                captureSessionSummary.duplicateNoSourceTicks - wgcPolicyNoSourceRepeats;
            const uint64_t wgcCombinedSourceRepeatLowerBoundTotal =
                std::max(wgcSourceRepeatLowerBoundTotal, wgcDeliveryRepeatLowerBoundTotal);
            const uint64_t wgcDuplicateRepeatExcessTotal =
                captureSessionSummary.duplicateTicks > wgcCombinedSourceRepeatLowerBoundTotal
                    ? (captureSessionSummary.duplicateTicks - wgcCombinedSourceRepeatLowerBoundTotal)
                    : 0ull;
            const uint64_t wgcCombinedExcessRepeatTotal = std::max(wgcExcessRepeatTotal, wgcDuplicateRepeatExcessTotal);
            const bool wgcCfrSmoothnessNotMaximal = ce::capture_policy::IsWgcCfrSmoothnessNotMaximal(
                SaturatingToUint32(liveTicksOutput), SaturatingToUint32(wgcCombinedExcessRepeatTotal),
                SaturatingToUint32(wgcPolicyAddedRepeatTotal), wgcExcessRepeatClusterMaxTicks,
                SaturatingToUint32(wgcDelayPostSelectionRejectedSyncRiskTotal));
            SnapshotPublishedWgcRuntimeLogState();
            const bool wgcLogSnapshotHasPool = media_main_g_WgcRuntimeLogSnapshot.hasPoolEvidence.load(std::memory_order_acquire);
            const uint32_t wgcSummarySourceFramePoolBuffers =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSourceFramePoolBufferCount() : 0u);
            const uint32_t wgcSummaryBudgetSurfaces =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.budgetSurfaces.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessBudgetSurfaceCount() : 0u);
            const uint32_t wgcSummarySyncFrames =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.syncFrames.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessSyncFrameCount() : 0u);
            const uint32_t wgcSummaryExtraFrames =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.extraFrames.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u);
            const uint32_t wgcSummaryRetainedCap =
                wgcLogSnapshotHasPool
                    ? media_main_g_WgcRuntimeLogSnapshot.retainedCap.load(std::memory_order_relaxed)
                    : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCap() : wgcSmoothnessRetainedFrameCap);
            const uint32_t wgcSummaryReservedFreeSlots =
                wgcLogSnapshotHasPool
                    ? media_main_g_WgcRuntimeLogSnapshot.reservedFreeSlots.load(std::memory_order_relaxed)
                    : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount() : wgcSmoothnessReservedFreeSlots);
            const uint32_t wgcSummarySafetySlots =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.safetySlots.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessSafetySlotCount() : 0u);
            const uint32_t wgcSummaryPoolLeasedMax =
                wgcLogSnapshotHasPool ? std::max(media_main_g_WgcRuntimeLogSnapshot.poolLeasedMax.load(std::memory_order_relaxed),
                                                 media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u);
            const uint32_t wgcSnapshotFreeMin = media_main_g_WgcRuntimeLogSnapshot.poolFreeMin.load(std::memory_order_relaxed);
            const uint32_t wgcCurrentFreeMin = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
            const uint32_t wgcSummaryPoolFreeMin =
                (wgcLogSnapshotHasPool && wgcSnapshotFreeMin != UINT32_MAX)
                    ? (wgcCurrentFreeMin > 0 ? std::min(wgcSnapshotFreeMin, wgcCurrentFreeMin) : wgcSnapshotFreeMin)
                    : wgcCurrentFreeMin;
            const uint32_t wgcSummaryPoolFreeNow = media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
            const uint32_t wgcSummaryPoolSaturatedDrops =
                wgcLogSnapshotHasPool
                    ? std::max(media_main_g_WgcRuntimeLogSnapshot.poolSaturatedDrops.load(std::memory_order_relaxed),
                               media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSaturatedDropCount() : 0u)
                    : (media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSaturatedDropCount() : 0u);
            const uint32_t wgcSummaryOverwritePrevented =
                wgcLogSnapshotHasPool
                    ? std::max(media_main_g_WgcRuntimeLogSnapshot.poolOverwritePrevented.load(std::memory_order_relaxed),
                               media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u)
                    : (media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u);
            const uint32_t wgcSummaryLeaseMismatches =
                wgcLogSnapshotHasPool
                    ? std::max(media_main_g_WgcRuntimeLogSnapshot.poolLeaseMismatches.load(std::memory_order_relaxed),
                               media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolLeaseMismatchCount() : 0u)
                    : (media_main_g_WgcCap ? media_main_g_WgcCap->GetPoolLeaseMismatchCount() : 0u);
            const uint64_t wgcSummarySmoothVramBytes =
                wgcLogSnapshotHasPool && media_main_g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed) > 0
                    ? media_main_g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed)
                    : wgcSmoothnessEstimatedVramBytes;
            const uint32_t wgcSummarySourceFormat =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.sourceFormat.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessSourceDxgiFormat() : 0u);
            const uint32_t wgcSummaryRetainedFormat =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.retainedFormat.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessCopyDxgiFormat() : 0u);
            const uint32_t wgcSummaryCompactRetained =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.compactRetained.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap && media_main_g_WgcCap->IsCompactRetainedCopyActive() ? 1u : 0u);
            const uint64_t wgcSummarySourceBudgetBytes =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.sourceBudgetBytes.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessSourceEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummaryCopyBudgetBytes =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.copyBudgetBytes.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessCopyEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummarySourceSurfaceBytes =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessSourceBytesPerSurface() : 0ull);
            const uint64_t wgcSummaryCopySurfaceBytes =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.copySurfaceBytes.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessCopyBytesPerSurface() : 0ull);
            const int64_t wgcSummaryConvertUs =
                wgcLogSnapshotHasPool ? media_main_g_WgcRuntimeLogSnapshot.lastConvertUs.load(std::memory_order_relaxed)
                                      : (media_main_g_WgcCap ? media_main_g_WgcCap->GetLastPoolConvertTimeUs() : 0);
            const uint32_t wgcSummaryOutputFps = getWgcSmoothnessOutputFps();
            const uint32_t wgcSummaryDuplicateTimestampsSeen =
                std::max(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen.load(std::memory_order_relaxed),
                         media_main_g_WgcCap ? media_main_g_WgcCap->GetNormalizedDuplicateTimestampCount() : 0u);
            const uint32_t wgcSummaryDuplicateTimestampsSkipped =
                std::max(media_main_g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped.load(std::memory_order_relaxed),
                         media_main_g_WgcCap ? media_main_g_WgcCap->GetDuplicateTimestampSkipCount() : 0u);
            const int64_t wgcSummarySmoothTargetDelayUs =
                qpcToUs(ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                    wgcSmoothnessRetainedFrames, targetIntervalTicks, wgcSummaryOutputFps,
                    config.wgcSmoothnessBufferMaxMs));
            const int64_t wgcSummarySmoothActualDelayUs = qpcToUs(wgcSmoothnessActiveDelayQpc);
            const int64_t wgcSummarySmoothDelayDeficitUs =
                std::max<int64_t>(0, wgcSummarySmoothTargetDelayUs - wgcSummarySmoothActualDelayUs);
            const int64_t wgcSummaryEffectiveDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
            const int64_t wgcSummaryStartupDelayDeficitUs =
                std::max<int64_t>(0, wgcStartupDelayTargetUs - wgcSummaryEffectiveDelayUs);
            LogInfo(
                "[WGC CFR SMOOTHNESS SUMMARY] encoderLimitedDrops=%llu maxDropTicks=%u cadenceEvents=%llu "
                "phaseErrorMax=%uus shortfallMax=%.1fms staleDebtDrops=%llu liveRebase=%llu/%u "
                "tooNewRepeats=%u syncDelayHolds=%llu tooNewLeadMax=%uus avDelay=%.1fms startupDelay=%.1fms "
                "scheduleOffset=%lldus effectiveDelay=%.1fms lowSourceBypass=%llu modeMismatch=%llu "
                "sourceBacktrack=%llu syncDelaySourceLimitedHolds=%llu syncDelayPolicyHolds=%llu "
                "startupReserveFrames=%u startupReserveSpan=%lldus startupDelayTarget=%lldus "
                "startupReserveSelected=%d startupReserveReason=%s producerTargetFps=%u producerContractRetunes=%llu "
                "smoothBuf=%d smoothTargetMs=%u "
                "smoothFrames=%u/%u/%u smoothDelay=%.1fms smoothPoolSlots=%u sourceFramePoolBuffers=%u "
                "budgetSurfaces=%u syncFrames=%u extraFrames=%u retainedCap=%u reservedFreeSlots=%u safetySlots=%u "
                "retainedCapTrim=%llu ingressAccepted=%u ingressDecimated=%u ingressPlaySoft=%u "
                "ingressPlayCredit=%u ingressRetained=%u/%u "
                "ingressLowWater=%u leasedMax=%u freeNow=%u freeMin=%u poolPressureTrim=%llu "
                "poolSaturatedDrops=%u overwritePrevented=%u "

                "leaseMismatches=%u smoothVramMB=%.1f smoothCapLimited=%d smoothReason=%s "
                "sourceFmt=%u retainedFmt=%u compactRetained=%d sourceBudgetMB=%.1f copyBudgetMB=%.1f "
                "sourceSurfaceMB=%.1f copySurfaceMB=%.1f convertUs=%lld",
                static_cast<unsigned long long>(wgcEncoderLimitedSourceDropTotal), wgcEncoderLimitedSourceDropMaxTicks,
                static_cast<unsigned long long>(wgcEncoderLimitedCadenceEventCount),
                captureSessionSummary.maxWgcContentPhaseErrorUs, captureSessionSummary.maxShortfallDurationMs,
                static_cast<unsigned long long>(wgcDropStaleDebtTotal),
                static_cast<unsigned long long>(wgcLiveSchedulerRebaseTotal), wgcLiveSchedulerRebaseMaxTicks,
                SaturatingToUint32(wgcRepeatPolicyHoldTotal), static_cast<unsigned long long>(wgcSyncDelayHoldTotal),
                wgcTooNewLeadSessionMaxUs, avContentDelayActive ? maxAudioCaptureLatencyMs : 0.0f,
                static_cast<double>(wgcAvSyncStartupDelayUs) / 1000.0,
                static_cast<long long>(wgcAvSyncScheduleOffsetUs),
                static_cast<double>(qpcToUs(getWgcEffectiveContentDelayQpc())) / 1000.0,
                static_cast<unsigned long long>(wgcEncoderLimitedSuppressedByLowSourceTotal),
                static_cast<unsigned long long>(wgcCapacityPressureModeMismatchTotal),
                static_cast<unsigned long long>(wgcSelectedSourceBacktrackTotal),
                static_cast<unsigned long long>(wgcSyncDelaySourceLimitedHoldTotal),
                static_cast<unsigned long long>(wgcSyncDelayPolicyHoldTotal), wgcStartupReserveFrames,
                static_cast<long long>(wgcStartupReserveSpanUs), static_cast<long long>(wgcStartupDelayTargetUs),
                wgcStartupSelectedByDelayReserve ? 1 : 0, wgcStartupReserveReason.c_str(),
                media_main_g_WgcCap ? media_main_g_WgcCap->GetProducerTargetFps() : 0u,
                static_cast<unsigned long long>(wgcProducerRateRetuneTotal), config.wgcSmoothnessBufferEnabled ? 1 : 0,
                config.wgcSmoothnessBufferMaxMs, wgcSmoothnessActualFrames, wgcSmoothnessRetainedFrames,
                wgcSmoothnessDesiredFrames, static_cast<double>(wgcSummarySmoothActualDelayUs) / 1000.0,
                wgcSmoothnessPoolSlots, wgcSummarySourceFramePoolBuffers, wgcSummaryBudgetSurfaces,
                wgcSummarySyncFrames, wgcSummaryExtraFrames, wgcSummaryRetainedCap, wgcSummaryReservedFreeSlots,
                wgcSummarySafetySlots, static_cast<unsigned long long>(wgcRetainedCapTrimTotal),
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressLowWaterFrameCount() : 0u, wgcSummaryPoolLeasedMax,
                wgcSummaryPoolFreeNow, wgcSummaryPoolFreeMin, static_cast<unsigned long long>(wgcPoolPressureTrimTotal),
                wgcSummaryPoolSaturatedDrops, wgcSummaryOverwritePrevented, wgcSummaryLeaseMismatches,
                static_cast<double>(wgcSummarySmoothVramBytes) / (1024.0 * 1024.0), wgcSmoothnessCapLimited ? 1 : 0,
                wgcSmoothnessBufferReason.c_str(), wgcSummarySourceFormat, wgcSummaryRetainedFormat,
                wgcSummaryCompactRetained, static_cast<double>(wgcSummarySourceBudgetBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummaryCopyBudgetBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummarySourceSurfaceBytes) / (1024.0 * 1024.0),
                static_cast<double>(wgcSummaryCopySurfaceBytes) / (1024.0 * 1024.0),
                static_cast<long long>(wgcSummaryConvertUs));
            LogInfo(
                "[WGC CFR SMOOTHNESS BUFFER] smoothTargetDelay=%lldus smoothActualDelay=%lldus "
                "smoothDelayDeficit=%lldus startupDelayTarget=%lldus effectiveDelay=%lldus "
                "startupDelayDeficit=%lldus finalAvSync=exported_tracks_authoritative",
                static_cast<long long>(wgcSummarySmoothTargetDelayUs),
                static_cast<long long>(wgcSummarySmoothActualDelayUs),
                static_cast<long long>(wgcSummarySmoothDelayDeficitUs), static_cast<long long>(wgcStartupDelayTargetUs),
                static_cast<long long>(wgcSummaryEffectiveDelayUs),
                static_cast<long long>(wgcSummaryStartupDelayDeficitUs));
            const uint32_t wgcSummaryIngressHard = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressHardReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressSoft = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressDecimated = media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u;
            const uint32_t wgcSummaryOverloadFlags =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
            const uint32_t wgcSummaryMuxBackpressure =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.muxBackpressureCount.load(std::memory_order_relaxed) : 0u;
            const bool wgcSummaryPoolPressure =
                wgcSummaryPoolSaturatedDrops > 0 || wgcSummaryIngressHard > 0 || wgcSummaryPoolFreeMin == 0;
            const uint32_t recordingCapacityCauses =
                recordingHealthState.flags & ce::capture_policy::kRecordingHealthCauseMask;
            const bool recordingVideoDegraded = ce::capture_policy::HasRecordingHealthFlag(
                recordingHealthState.flags, ce::capture_policy::kRecordingHealthFlagVideoDegraded);
            const bool capacityDebtDominant =
                ce::capture_policy::IsRecordingCapacityDebtDominant(recordingHealthState);
            const bool encoderDebtHistoryLoss =
                recordingVideoDegraded && capacityDebtDominant &&
                (recordingCapacityCauses & ce::capture_policy::kRecordingHealthFlagEncoderPressureObserved) != 0;
            const bool muxDebtHistoryLoss =
                recordingVideoDegraded && capacityDebtDominant &&
                (recordingCapacityCauses & ce::capture_policy::kRecordingHealthFlagMuxPressureObserved) != 0;
            const bool wgcSummaryEncoderMuxPressure =
                ce::capture_policy::HasRecordingEncoderOrMuxPressure(
                    wgcSummaryOverloadFlags, wgcSummaryMuxBackpressure, wgcEncoderLimitedSourceDropTotal) ||
                encoderDebtHistoryLoss || muxDebtHistoryLoss;
            const bool nonDominantCapacityHistory =
                recordingVideoDegraded && recordingCapacityCauses != 0 && !capacityDebtDominant;
            const char* wgcSummaryLimiter =
                encoderDebtHistoryLoss && muxDebtHistoryLoss ? "encoder_mux_timeline_debt"
                : encoderDebtHistoryLoss                    ? "encoder_timeline_debt"
                : muxDebtHistoryLoss                        ? "mux_timeline_debt"
                : nonDominantCapacityHistory && wgcSummaryPoolPressure
                    ? "wgc_pool_pressure"
                : nonDominantCapacityHistory && captureSessionSummary.duplicateNoSourceTicks > 0
                    ? "source_limited"
                : wgcSummaryEncoderMuxPressure              ? "encoder_or_mux"
                : wgcSummaryPoolPressure                    ? "wgc_pool_pressure"
                : captureSessionSummary.duplicateNoSourceTicks > 0
                    ? "source_limited"
                : captureSessionSummary.duplicateTicks > 0 ? "source_cadence_or_vrr"
                                                           : "none";
            const bool wgcSummaryCleanEncoderMux =
                !wgcSummaryEncoderMuxPressure && wgcSummaryOverloadFlags == 0 && wgcSummaryMuxBackpressure == 0;
            const bool wgcSummaryCleanPool = !wgcSummaryPoolPressure;
            const bool wgcSummaryCleanSelection = wgcCombinedExcessRepeatTotal == 0 && wgcPolicyAddedRepeatTotal == 0 &&
                                                  wgcDelayPostSelectionRejectedSyncRiskTotal == 0 &&
                                                  !wgcCfrSmoothnessNotMaximal && !wgcActiveDelayMixedPolicyFault &&
                                                  wgcCapacityPressureModeMismatchTotal == 0 &&
                                                  wgcSelectedSourceBacktrackTotal == 0;
            const bool wgcSummaryCoverageHoles =
                captureSessionSummary.duplicateNoSourceTicks > 0 || wgcCombinedSourceRepeatLowerBoundTotal > 0 ||
                wgcSourceRepeatLowerBoundTotal > 0 || wgcDeliveryRepeatLowerBoundTotal > 0;
            const bool wgcSummarySourceCoverageBestEffort = wgcSummaryCoverageHoles && wgcSummaryCleanEncoderMux &&
                                                            wgcSummaryCleanPool && wgcSummaryCleanSelection &&
                                                            !recordingVideoDegraded;
            const char* wgcSummaryCoverage =
                wgcSummaryCoverageHoles ? "limited"
                                        : (captureSessionSummary.duplicateTicks > 0 ? "cadence_or_vrr" : "full");
            const char* wgcSummaryCoverageReason =
                encoderDebtHistoryLoss ? "encoder_debt_history_loss"
                : muxDebtHistoryLoss   ? "mux_debt_history_loss"
                : (wgcSourceRepeatLowerBoundTotal > 0 && wgcDeliveryRepeatLowerBoundTotal > 0)
                    ? "source_and_delivery_holes"
                : (wgcDeliveryRepeatLowerBoundTotal > 0) ? "delivery_holes"
                : (wgcSourceRepeatLowerBoundTotal > 0)   ? "source_holes"
                : (captureSessionSummary.duplicateNoSourceTicks > 0)
                    ? "no_fresh_source_for_cfr_slots"
                    : (captureSessionSummary.duplicateTicks > 0 ? "cadence_or_vrr" : "none");
            LogInfo(
                "[WGC CFR QUALITY] duplicatePct=%.1f duplicates=%llu/%llu worst1sUnique=%u worst1sRepeats=%u "
                "worst1sEmit=%u limiter=%s sourceLimitedRepeats=%llu poolPressure=%d freeMin=%u "
                "poolSaturatedDrops=%u ingressHard=%u ingressSoft=%u ingressDecimated=%u "
                "poolPressureTrim=%llu "
                "ingressPlaySoft=%u ingressPlayCredit=%u overwritePrevented=%u "
                "syncProtectedRepeats=%llu policyAddedRepeats=%llu excessRepeats=%llu "
                "smoothDelayDeficitUs=%lld startupDelayDeficitUs=%lld "
                "dupTsSeen=%u dupTsSkipped=%u encoderOverload=0x%X muxBackpressure=%u "
                "compactRetained=%d sourceFmt=%u retainedFmt=%u convertUs=%lld backend=%s timingBasis=cpu_wall "
                "finalAvSync=exported_tracks_authoritative",
                static_cast<double>(duplicatePermille) / 10.0,
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<unsigned long long>(liveTicksOutput), captureSessionSummary.worstOneSecondUniqueCount,
                captureSessionSummary.worstOneSecondRepeatCount, captureSessionSummary.worstOneSecondEmitCount,
                wgcSummaryLimiter, static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                wgcSummaryPoolPressure ? 1 : 0, wgcSummaryPoolFreeMin, wgcSummaryPoolSaturatedDrops,
                wgcSummaryIngressHard, wgcSummaryIngressSoft, wgcSummaryIngressDecimated,
                static_cast<unsigned long long>(wgcPoolPressureTrimTotal),
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u, wgcSummaryOverwritePrevented,
                static_cast<unsigned long long>(wgcDelaySyncProtectedRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<long long>(wgcSummarySmoothDelayDeficitUs),
                static_cast<long long>(wgcSummaryStartupDelayDeficitUs), wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped, wgcSummaryOverloadFlags, wgcSummaryMuxBackpressure,
                wgcSummaryCompactRetained, wgcSummarySourceFormat, wgcSummaryRetainedFormat,
                static_cast<long long>(wgcSummaryConvertUs),
                media_main_g_WgcCap && media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc");
            LogInfo(
                "[WGC CFR SOURCE COVERAGE] coverage=%s reason=%s bestEffort=%d outputFps=%u "
                "duplicates=%llu/%llu sourceLimitedRepeats=%llu sourceRepeatLowerBound=%llu "
                "syncSourceRepeatLowerBound=%llu deliveryRepeatLowerBound=%llu excessRepeats=%llu "
                "policyAddedRepeats=%llu policyNoSourceRepeats=%llu cleanEncoderMux=%d cleanPool=%d "
                "cleanSelection=%d encoderOverload=0x%X muxBackpressure=%u poolPressure=%d "
                "poolFreeMin=%u finalAvSync=exported_tracks_authoritative "
                "note=surplus_source_frames_are_dropped_when_available_repeats_mean_cfr_coverage_holes",
                wgcSummaryCoverage, wgcSummaryCoverageReason, wgcSummarySourceCoverageBestEffort ? 1 : 0,
                wgcSummaryOutputFps, static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(wgcCombinedSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcDeliveryRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyNoSourceRepeats), wgcSummaryCleanEncoderMux ? 1 : 0,
                wgcSummaryCleanPool ? 1 : 0, wgcSummaryCleanSelection ? 1 : 0, wgcSummaryOverloadFlags,
                wgcSummaryMuxBackpressure, wgcSummaryPoolPressure ? 1 : 0, wgcSummaryPoolFreeMin);
            LogInfo(
                "[WGC CFR SMOOTHNESS INGRESS] accepted=%u decimated=%u retained=%u/%u lowWater=%u "
                "accLowWater=%u accRecovery=%u accSourceBelow=%u accHealthy=%u "
                "accPlaySoft=%u accPlayCredit=%u "
                "decSoftReserve=%u decHardReserve=%u decCredit=%u "
                "softReservePressure=%u hardReservePressure=%u dupTsSeen=%u dupTsSkipped=%u lastReason=%s",
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressLowWaterFrameCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedLowWaterCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedRecoveryCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedSourceBelowCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedHealthyCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedSoftReserveCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedHardReserveCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCreditCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressSoftReservePressureCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressHardReservePressureCount() : 0u, wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped,
                media_main_g_WgcCap ? WgcIngressAdmissionReasonName(media_main_g_WgcCap->GetIngressAdmissionReasonCode()) : "none");
            LogInfo(
                "[WGC CFR SMOOTHNESS SOURCE] acceptedTotal=%llu cfrTicksTotal=%llu "
                "rollingAccepted=%u rollingCfrTicks=%u rollingDeficit=%u rollingSurplus=%u "
                "lastWindowAccepted=%u lastWindowCfrTicks=%u windowSlots=%zu",
                static_cast<unsigned long long>(wgcRollingSourceAcceptedTotal),
                static_cast<unsigned long long>(wgcRollingSourceCfrTickTotal), wgcRollingSourceAcceptedSum,
                wgcRollingSourceCfrTickSum, wgcRollingSourceDeficitFrames, wgcRollingSourceSurplusFrames,
                wgcRollingSourceAcceptedWindow, wgcRollingSourceCfrTicksWindow, wgcRollingSourceSlotCount);
            LogInfo(
                "[WGC CFR SMOOTHNESS DELAY] delayReservoirLowWaterFrames=%u delayReservoirTargetFrames=%u "
                "delayReservoirLowWaterTicks=%llu realizedDelayAvg=%uus realizedDelayMin=%uus "
                "realizedDelayMax=%uus delayResidualAvg=%d/%uus delayResidualMax=%uus "
                "delayResidualP95=%uus delayResidualLateMax=%uus delayResidualEarlyMax=%uus "
                "rawResidualAvg=%d/%uus rawResidualMax=%uus rawResidualP95=%uus rawResidualLateMax=%uus "
                "rawResidualEarlyMax=%uus predictedResidualAvg=%d/%uus predictedResidualP95=%uus "
                "predictedResidualLateMax=%uus rawMinusPredictedAvg=%d/%uus rawMinusPredictedMax=%uus",
                getWgcDelayReservoirLowWaterFrames(), getWgcDelayReservoirTargetFrames(),
                static_cast<unsigned long long>(wgcDelayReservoirLowWaterTickTotal), wgcDelayRealizedAvgUs,
                wgcDelayRealizedMinFinalUs, wgcDelayRealizedMaxUs, wgcDelayResidualAvgSignedUs,
                wgcDelayResidualAvgAbsUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualP95Us(), wgcDelayResidualLateMaxUs,
                wgcDelayResidualEarlyMaxUs, wgcDelayRawResidualAvgSignedUs, wgcDelayRawResidualAvgAbsUs,
                wgcDelayRawResidualAbsMaxUs, wgcDelayRawResidualP95Us(), wgcDelayRawResidualLateMaxUs,
                wgcDelayRawResidualEarlyMaxUs, wgcDelayResidualAvgSignedUs, wgcDelayResidualAvgAbsUs,
                wgcDelayResidualP95Us(), wgcDelayResidualLateMaxUs, wgcDelayRawMinusPredictedAvgSignedUs,
                SaturatingToUint32(static_cast<uint64_t>(wgcDelayRawMinusPredictedAvgSignedUs >= 0
                                                             ? wgcDelayRawMinusPredictedAvgSignedUs
                                                             : -wgcDelayRawMinusPredictedAvgSignedUs)),
                wgcDelayRawMinusPredictedAbsMaxUs);
            // Smoothness FLOOR rollup: ties the resolved floor to its realized result so a soak run is
            // conclusive. With the floor active and working, realizedDelay(min/avg/max) should sit near
            // smoothFloorRealizedTargetUs with a bounded residualLateMax (jitter absorbed); a collapsed
            // realizedDelayMin with a large residualLateMax means jitter exceeded the floor budget
            // (overflow -> even repeats), NOT a sync/ghost-judder fault (audio anchor never moved).
            LogInfo(
                "[WGC CFR SMOOTHNESS FLOOR] smoothFloorSource=%s smoothFloorConfigured=%d smoothFloorMs=%u "
                "smoothFloorRequestedUs=%lld smoothFloorDelayUs=%lld smoothFloorClampedBy=%s "
                "smoothFloorRealizedTargetUs=%lld measuredDeliveryGapUs(avg/max)=%u/%u "
                "measuredSourceJitterUs(avg/max)=%u/%u realizedDelay(min/avg/max)Us=%u/%u/%u "
                "residualLateMaxUs=%u avContentDelayActive=%d",
                wgcSmoothnessFloorSource, wgcSmoothnessFloorConfigured ? 1 : 0, config.wgcSmoothnessFloorMs,
                static_cast<long long>(qpcToUs(wgcSmoothnessFloorRequestedQpc)),
                static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), wgcSmoothnessFloorClampedBy,
                static_cast<long long>(qpcToUs(avContentDelayActive ? 0 : wgcSmoothnessFloorDelayQpc)),
                wgcSmoothnessFloorJitter.deliveryGapAvgUs, wgcSmoothnessFloorJitter.deliveryGapMaxUs,
                wgcSmoothnessFloorJitter.sourceJitterAvgUs, wgcSmoothnessFloorJitter.sourceJitterMaxUs,
                wgcDelayRealizedMinFinalUs, wgcDelayRealizedAvgUs, wgcDelayRealizedMaxUs, wgcDelayResidualLateMaxUs,
                avContentDelayActive ? 1 : 0);
            LogInfo(
                "[WGC CFR SMOOTHNESS REPEAT] delayResidualRelaxedSelections=%llu delayResidualRelaxedMax=%uus "
                "delayResidualRelaxedRejectedSync=%llu delayRepeatClusterPressure=%llu "
                "delayRepeatClusterMax=%u delayResidualRelaxedBetter=%llu delayResidualRelaxedCluster=%llu "
                "delayResidualRelaxedRejectedHeadroom=%llu delayResidualRelaxedRejectedCost=%llu "
                "delaySoftLateRejected=%llu delaySoftLateAccepted=%llu delayOlderFrameAvoidedRepeat=%llu "
                "delaySourceLimitedRepeats=%llu delayRepeatRescue=%llu/%llu "
                "delayRepeatRescueRejected=%llu/%llu/%llu delayRepeatPromoted=%llu/%llu "
                "delayRepeatPromoteRejectedSoft=%llu delayRepeatSafeAfterPromote=%llu "
                "delayRepeatSafeCandidate=%llu delayRepeatNoSafeCandidate=%llu "
                "delayRepeatSoftSafeCandidate=%llu delayRepeatNoSoftSafeCandidate=%llu "
                "delayRepeatWindowClass=%llu/%llu/%llu delayRepeatWindowState=%llu/%llu/%llu/%llu/%llu "
                "delayPostStallSafeFrames=%llu delayRepeatReserveMax=%u/%uus "
                "delaySourceRecoveryHolds=%llu delaySourceRecoveryTicks=%llu "
                "delayNearCapAccepted=%llu delayHardOnlyCandidates=%llu "
                "delaySyncProtectedRepeats=%llu delayOldestSoftSafeAgeMax=%uus delayUniformCadence=%llu "
                "delayUniformHold=%llu delayPaceCapTrim=%llu",
                static_cast<unsigned long long>(wgcDelayRelaxedSelectionCount), wgcDelayRelaxedSelectionMaxUs,
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedSyncRiskTotal),
                static_cast<unsigned long long>(wgcDelayRepeatClusterPressureTotal),
                wgcDelayRepeatClusterPressureMaxTicks,
                static_cast<unsigned long long>(wgcDelayRelaxedBetterTargetTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRepeatClusterTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedResidualHeadroomTotal),
                static_cast<unsigned long long>(wgcDelayRelaxedRejectedRepeatCostTotal),
                static_cast<unsigned long long>(wgcDelaySoftLateRejectedTotal),
                static_cast<unsigned long long>(wgcDelaySoftLateAcceptedTotal),
                static_cast<unsigned long long>(wgcDelayOlderFrameAvoidedRepeatTotal),
                static_cast<unsigned long long>(wgcDelaySourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueSuccessTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueAttemptTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedSyncTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedHeadroomTotal),
                static_cast<unsigned long long>(wgcDelayRepeatRescueRejectedCostTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotedBeforeRepeatTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotionAttemptTotal),
                static_cast<unsigned long long>(wgcDelayRepeatPromotionRejectedSoftTotal),
                static_cast<unsigned long long>(wgcDelayRepeatSafeAfterPromotionTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithoutSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithSoftSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayRepeatWithoutSoftSafeCandidateTotal),
                static_cast<unsigned long long>(wgcDelayWindowHealthyRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowRecoverableRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowSourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowHealthyRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowRecoverableRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowSourceLimitedRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowHardStallRepeatTotal),
                static_cast<unsigned long long>(wgcDelayWindowPostStallRepeatTotal),
                static_cast<unsigned long long>(wgcDelayPostStallSafeFrameTotal), wgcDelayRepeatReserveDepthMax,
                wgcDelayRepeatReserveSpanMaxUs, static_cast<unsigned long long>(wgcSyncDelaySourceRecoveryHoldTotal),
                static_cast<unsigned long long>(wgcActiveDelaySourceRecoveryTicks),
                static_cast<unsigned long long>(wgcDelayNearCapAcceptedTotal),
                static_cast<unsigned long long>(wgcDelayRepeatHardOnlyCandidateTotal),
                static_cast<unsigned long long>(wgcDelaySyncProtectedRepeatTotal), wgcDelayOldestSoftSafeAgeMaxUs,
                static_cast<unsigned long long>(wgcDelayUniformCadenceTotal),
                static_cast<unsigned long long>(wgcDelayUniformHoldTotal),
                static_cast<unsigned long long>(wgcDelayPaceCapTrimTotal));
            LogInfo(
                "[WGC CFR SMOOTHNESS VERDICT] delayPostSelectionRejectedSync=%llu "
                "delayPostSelectionRescuedSync=%llu sourceRepeatLowerBound=%llu excessRepeats=%llu "
                "policyAddedRepeats=%llu excessRepeatClusters=%llu excessRepeatClusterMax=%u "
                "smoothnessNotMaximal=%d mixedPolicyFault=%d syncSourceRepeatLowerBound=%llu "
                "deliveryRepeatLowerBound=%llu policyNoSourceRepeats=%llu",
                static_cast<unsigned long long>(wgcDelayPostSelectionRejectedSyncRiskTotal),
                static_cast<unsigned long long>(wgcDelayPostSelectionRescuedSyncRiskTotal),
                static_cast<unsigned long long>(wgcCombinedSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcExcessRepeatClusterTotal), wgcExcessRepeatClusterMaxTicks,
                wgcCfrSmoothnessNotMaximal ? 1 : 0, wgcActiveDelayMixedPolicyFault ? 1 : 0,
                static_cast<unsigned long long>(wgcSourceRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcDeliveryRepeatLowerBoundTotal),
                static_cast<unsigned long long>(wgcPolicyNoSourceRepeats));
            privacyRuntime.LogSummary(qpcFreq.QuadPart);
        } else {
            LogInfo(
                "[Inject CFR SUMMARY] Live=%llu Dup=%llu DupPct=%.1f%% DupReason(src=%llu def=%llu timer=%llu "
                "drain=%llu) FreshCatchup=%llu RepeatCatchup=%llu StaleTrim=%llu Recovery=%d/%llu "
                "PathMismatch=%llu/%llu "
                "DefRequeued=%llu DefDropped=%llu",
                static_cast<unsigned long long>(liveTicksOutput),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTicks),
                static_cast<double>(duplicatePermille) / 10.0,
                static_cast<unsigned long long>(captureSessionSummary.duplicateNoSourceTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDeferredTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateTimerTicks),
                static_cast<unsigned long long>(captureSessionSummary.duplicateDrainTicks),
                static_cast<unsigned long long>(injectFreshCatchupTotal),
                static_cast<unsigned long long>(injectRepeatCatchupTotal),
                static_cast<unsigned long long>(injectLiveStaleTrimTotal), injectCfrRecoveryActive ? 1 : 0,
                static_cast<unsigned long long>(injectCfrRecoveryEpisodesTotal),
                static_cast<unsigned long long>(activePathMismatchDiscardTotal),
                static_cast<unsigned long long>(media_main_g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(injectDeferredRequeuedTotal),
                static_cast<unsigned long long>(injectDeferredDroppedTotal));
            LogInfo(
                "[Inject CFR SUMMARY] SourceFps=%.2f..%.2f JitterMax=%uus SelMax=%uus EncEmaMax=%.2fms "
                "ServiceEma=%.2fms ServiceMax=%uus SustainMin=%.1ffps",
                injectWorstSourceFpsX100 == std::numeric_limits<uint32_t>::max() ? 0.0
                                                                                 : (injectWorstSourceFpsX100 / 100.0),
                injectBestSourceFpsX100 / 100.0, injectWorstSourceJitterUs, injectWorstSelectionErrorUs,
                captureSessionSummary.maxEncodeEmaMs, smoothedInjectServiceMs, injectServiceMaxUs,
                captureSessionSummary.minEncoderSustainFps == std::numeric_limits<double>::max()
                    ? 0.0
                    : captureSessionSummary.minEncoderSustainFps);
            LogInfo(
                "[Inject CFR OVERLOAD PACER] Episodes=%llu RepeatDecisions=%llu Probes=%llu Emitted=%llu "
                "FreshGrants=%llu MaxRepeatRun=%u MinFreshFraction=%.3f FreshSvcEma=%.2fms/%u "
                "RepeatSvcEma=%.2fms/%u PtsGrid=immutable AudioTimeline=unchanged",
                static_cast<unsigned long long>(injectOverloadRepeatRuntime.pacer.episodes),
                static_cast<unsigned long long>(injectOverloadRepeatRuntime.pacer.proactiveRepeats),
                static_cast<unsigned long long>(injectOverloadRepeatRuntime.pacer.probeRepeats),
                static_cast<unsigned long long>(injectOverloadRepeatRuntime.pacer.emittedRepeats),
                static_cast<unsigned long long>(injectOverloadRepeatRuntime.pacer.freshGrants),
                injectOverloadRepeatRuntime.pacer.maxConsecutiveProactiveRepeats,
                injectOverloadRepeatRuntime.pacer.minimumFreshFraction,
                injectOverloadRepeatRuntime.freshServiceMs, injectOverloadRepeatRuntime.freshServiceSamples,
                injectOverloadRepeatRuntime.repeatServiceMs, injectOverloadRepeatRuntime.repeatServiceSamples);
            LogInfo(
                "[Inject CFR QUALITY SUMMARY] TargetSelect=%llu Superseded=%llu TargetHold=%llu "
                "HoldWithCandidate=%llu BufferCapTrim=%llu TargetResidualMax=%uus "
                "PhaseReservePeak=%zu PhaseShiftMax=%lldus PreserveFrontTrim=%llu "
                "DisplayPathTransitions=%llu DisplayPhaseReacquire=%llu",
                static_cast<unsigned long long>(injectTargetSelectTotal),
                static_cast<unsigned long long>(injectTargetSupersededTotal),
                static_cast<unsigned long long>(injectTargetHoldTotal),
                static_cast<unsigned long long>(injectTargetHoldWithCandidateTotal),
                static_cast<unsigned long long>(injectBufferCapTrimTotal), injectTargetResidualMaxUs,
                injectTimestampPhaseReservePeak,
                static_cast<long long>(qpcToUs(injectTimestampPhaseMaxQpc)),
                static_cast<unsigned long long>(injectFrontPreserveTrimTotal),
                static_cast<unsigned long long>(injectTimestampPathTransitionCount),
                static_cast<unsigned long long>(injectDisplayTimingPhaseReacquireCount));
            if (media_main_g_pSharedMem) {
                const auto& contention = media_main_g_pSharedMem->runtimeState;
                LogInfo(
                    "[Inject Contention SUMMARY] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u "
                    "EventSignals=%u ThrottleTransitions=%llu",
                    contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                    contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                    contention.injectFrameReadySignals.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(injectProducerThrottleTransitionCount));
            }
        }
        const auto& phaseLockSummary = summaryUsesScreenGrab ? wgcCfrPhaseLock : injectCfrPhaseLock;
        const char* phaseLockBackend = summaryUsesScreenGrab
                                           ? (media_main_g_WgcCap && media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc")
                                           : "inject";
        LogInfo(
            "[CFR PHASE LOCK SUMMARY] Backend=%s Enabled=%d Locked=%d Offset=%lldus Stable=%u Unstable=%u "
            "Acquire=%llu Rephase=%llu Release=%llu Multiplier=%u",
            phaseLockBackend, captureSyncPhaseLockEnabled ? 1 : 0,
            phaseLockSummary.locked ? 1 : 0,
            static_cast<long long>(qpcToUs(phaseLockSummary.lockedPhaseQpc)),
            phaseLockSummary.stableSourceIntervals, phaseLockSummary.unstableSourceIntervals,
            static_cast<unsigned long long>(phaseLockSummary.acquisitions),
            static_cast<unsigned long long>(phaseLockSummary.rephases),
            static_cast<unsigned long long>(phaseLockSummary.releases), captureSyncMultiplier);
        LogInfo(
            "[RECORDING HEALTH] status=%s cause=%s flags=0x%X currentDebtMs=%u peakDebtMs=%u "
            "capacityDebtMs=%u cfr=%d settingsChanged=0 ptsGrid=immutable audioTimeline=unchanged",
            ce::capture_policy::GetRecordingHealthStatus(recordingHealthState.flags),
            ce::capture_policy::GetRecordingHealthCause(recordingHealthState.flags), recordingHealthState.flags,
            recordingHealthState.currentDebtMs, recordingHealthState.peakDebtMs,
            recordingHealthState.capacityAttributedDebtMs, config.video.useVFR ? 0 : 1);
    }

    SetCapturePipelinePhase(CapturePipelinePhase::kIdle);

    LogInfo("[EncoderThread] Stopped");
}
