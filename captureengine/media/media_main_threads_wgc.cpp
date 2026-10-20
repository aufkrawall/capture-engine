#include "media_main_internal.h"

void WgcCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[WGC CaptureThread] Started (diagnostics logger)");
    MediaState().wgcCaptureRunning = true;

    DisableCurrentThreadPowerThrottling("WGC CaptureThread");
    ScopedMmcssTask wgcMmcssTask(L"Capture", AVRT_PRIORITY_HIGH, "WGC CaptureThread");

    DWORD lastDiagTime = 0;
    uint32_t lastInputCount = 0;
    uint32_t lastCallbackCount = 0;
    uint64_t lastHostDroppedCount = 0;
    uint32_t lastPacingSkipCount = 0;
    uint32_t lastThrottleSkipCount = 0;
    uint32_t lastStaleSkipCount = 0;
    uint32_t lastStaleDuplicateTsCount = 0;
    uint32_t lastStaleOutOfOrderTsCount = 0;
    uint32_t lastNormalizedDuplicateTsCount = 0;
    uint32_t lastDuplicateTsSkipCount = 0;
    uint32_t lastCursorSkipCount = 0;
    uint32_t lastPoolDropCount = 0;
    uint32_t lastKeyedAcquireFailCount = 0;
    uint32_t lastKeyedReleaseFailCount = 0;
    uint32_t lastKeyedAbandonedReclaimCount = 0;
    uint32_t lastSplitFlushCount = 0;
    uint32_t lastSplitFlushSkippedCount = 0;
    uint32_t lastPoolSlotFastRewriteCount = 0;
    uint32_t lastPoolSaturatedDropCount = 0;
    uint32_t lastPoolOverwritePreventedCount = 0;
    uint32_t lastIngressAcceptedCount = 0;
    uint32_t lastIngressDecimatedCount = 0;
    uint32_t lastIngressAcceptedLowWaterCount = 0;
    uint32_t lastIngressAcceptedRecoveryCount = 0;
    uint32_t lastIngressAcceptedSourceBelowCount = 0;
    uint32_t lastIngressAcceptedHealthyCount = 0;
    uint32_t lastIngressAcceptedUniformPlayoutSoftReserveCount = 0;
    uint32_t lastIngressAcceptedUniformPlayoutCreditCount = 0;
    uint32_t lastIngressDecimatedSoftReserveCount = 0;
    uint32_t lastIngressDecimatedHardReserveCount = 0;
    uint32_t lastIngressDecimatedCreditCount = 0;
    uint32_t lastIngressSoftReservePressureCount = 0;
    uint32_t lastIngressHardReservePressureCount = 0;
    uint32_t lastDuplicateCount = 0;
    uint32_t lastLateCount = 0;
    bool sessionPrimed = false;

    while (!MediaState().wgcCaptureShutdown) {
        Sleep(1000);

        if (!MediaState().recording || !MediaState().wgcCap) {
            sessionPrimed = false;
            lastInputCount = 0;
            lastCallbackCount = 0;
            lastHostDroppedCount = 0;
            lastPacingSkipCount = 0;
            lastThrottleSkipCount = 0;
            lastStaleSkipCount = 0;
            lastStaleDuplicateTsCount = 0;
            lastStaleOutOfOrderTsCount = 0;
            lastNormalizedDuplicateTsCount = 0;
            lastDuplicateTsSkipCount = 0;
            lastCursorSkipCount = 0;
            lastPoolDropCount = 0;
            lastKeyedAcquireFailCount = 0;
            lastKeyedReleaseFailCount = 0;
            lastKeyedAbandonedReclaimCount = 0;
            lastSplitFlushCount = 0;
            lastSplitFlushSkippedCount = 0;
            lastPoolSlotFastRewriteCount = 0;
            lastPoolSaturatedDropCount = 0;
            lastPoolOverwritePreventedCount = 0;
            lastIngressAcceptedCount = 0;
            lastIngressDecimatedCount = 0;
            lastIngressAcceptedLowWaterCount = 0;
            lastIngressAcceptedRecoveryCount = 0;
            lastIngressAcceptedSourceBelowCount = 0;
            lastIngressAcceptedHealthyCount = 0;
            lastIngressAcceptedUniformPlayoutSoftReserveCount = 0;
            lastIngressAcceptedUniformPlayoutCreditCount = 0;
            lastIngressDecimatedSoftReserveCount = 0;
            lastIngressDecimatedHardReserveCount = 0;
            lastIngressDecimatedCreditCount = 0;
            lastIngressSoftReservePressureCount = 0;
            lastIngressHardReservePressureCount = 0;
            lastDuplicateCount = 0;
            lastLateCount = 0;
            lastDiagTime = 0;
            continue;
        }

        if (!sessionPrimed) {
            lastInputCount = MediaState().wgcCap->GetInputFrameCount();
            lastCallbackCount = MediaState().wgcCap->GetCallbackFrameCount();
            lastHostDroppedCount = MediaState().frameQueue.GetDroppedCount();
            lastPacingSkipCount = MediaState().wgcCap->GetPacingSkipCount();
            lastThrottleSkipCount = MediaState().wgcCap->GetThrottleSkipCount();
            lastStaleSkipCount = MediaState().wgcCap->GetStaleSkipCount();
            lastStaleDuplicateTsCount = MediaState().wgcCap->GetStaleDuplicateTimestampCount();
            lastStaleOutOfOrderTsCount = MediaState().wgcCap->GetStaleOutOfOrderTimestampCount();
            lastNormalizedDuplicateTsCount = MediaState().wgcCap->GetNormalizedDuplicateTimestampCount();
            lastDuplicateTsSkipCount = MediaState().wgcCap->GetDuplicateTimestampSkipCount();
            lastCursorSkipCount = MediaState().wgcCap->GetCursorOnlySkipCount();
            lastPoolDropCount = MediaState().wgcCap->GetPoolDropCount();
            lastKeyedAcquireFailCount = MediaState().wgcCap->GetKeyedMutexAcquireFailCount();
            lastKeyedReleaseFailCount = MediaState().wgcCap->GetKeyedMutexReleaseFailCount();
            lastKeyedAbandonedReclaimCount = MediaState().wgcCap->GetKeyedMutexAbandonedReclaimCount();
            lastSplitFlushCount = MediaState().wgcCap->GetSplitDeviceFlushCount();
            lastSplitFlushSkippedCount = MediaState().wgcCap->GetSplitDeviceFlushSkippedCount();
            lastPoolSlotFastRewriteCount = MediaState().wgcCap->GetPoolSlotFastRewriteCount();
            lastPoolSaturatedDropCount = MediaState().wgcCap->GetPoolSaturatedDropCount();
            lastPoolOverwritePreventedCount = MediaState().wgcCap->GetPoolSlotOverwritePreventedCount();
            lastIngressAcceptedCount = MediaState().wgcCap->GetIngressAcceptedCount();
            lastIngressDecimatedCount = MediaState().wgcCap->GetIngressDecimatedCount();
            lastIngressAcceptedLowWaterCount = MediaState().wgcCap->GetIngressAcceptedLowWaterCount();
            lastIngressAcceptedRecoveryCount = MediaState().wgcCap->GetIngressAcceptedRecoveryCount();
            lastIngressAcceptedSourceBelowCount = MediaState().wgcCap->GetIngressAcceptedSourceBelowCount();
            lastIngressAcceptedHealthyCount = MediaState().wgcCap->GetIngressAcceptedHealthyCount();
            lastIngressAcceptedUniformPlayoutSoftReserveCount =
                MediaState().wgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            lastIngressAcceptedUniformPlayoutCreditCount = MediaState().wgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            lastIngressDecimatedSoftReserveCount = MediaState().wgcCap->GetIngressDecimatedSoftReserveCount();
            lastIngressDecimatedHardReserveCount = MediaState().wgcCap->GetIngressDecimatedHardReserveCount();
            lastIngressDecimatedCreditCount = MediaState().wgcCap->GetIngressDecimatedCreditCount();
            lastIngressSoftReservePressureCount = MediaState().wgcCap->GetIngressSoftReservePressureCount();
            lastIngressHardReservePressureCount = MediaState().wgcCap->GetIngressHardReservePressureCount();
            SnapshotPublishedWgcRuntimeLogState();
            if (MediaState().pSharedMem) {
                lastDuplicateCount = MediaState().pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                lastLateCount = MediaState().pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
            }
            lastDiagTime = GetTickCount();
            sessionPrimed = true;
            continue;
        }

        DWORD now = GetTickCount();
        if (now - lastDiagTime >= 1000) {
            uint32_t currentInputCount = MediaState().wgcCap->GetInputFrameCount();
            uint32_t currentCount = MediaState().wgcCap->GetCallbackFrameCount();
            uint64_t queueDropped = MediaState().frameQueue.GetDroppedCount();
            uint32_t currentPacingSkipCount = MediaState().wgcCap->GetPacingSkipCount();
            uint32_t currentThrottleSkipCount = MediaState().wgcCap->GetThrottleSkipCount();
            uint32_t currentStaleSkipCount = MediaState().wgcCap->GetStaleSkipCount();
            uint32_t currentStaleDuplicateTsCount = MediaState().wgcCap->GetStaleDuplicateTimestampCount();
            uint32_t currentStaleOutOfOrderTsCount = MediaState().wgcCap->GetStaleOutOfOrderTimestampCount();
            uint32_t currentNormalizedDuplicateTsCount = MediaState().wgcCap->GetNormalizedDuplicateTimestampCount();
            uint32_t currentDuplicateTsSkipCount = MediaState().wgcCap->GetDuplicateTimestampSkipCount();
            uint32_t currentCursorSkipCount = MediaState().wgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = MediaState().wgcCap->GetPoolDropCount();
            uint32_t currentKeyedAcquireFailCount = MediaState().wgcCap->GetKeyedMutexAcquireFailCount();
            uint32_t currentKeyedReleaseFailCount = MediaState().wgcCap->GetKeyedMutexReleaseFailCount();
            uint32_t currentKeyedAbandonedReclaimCount = MediaState().wgcCap->GetKeyedMutexAbandonedReclaimCount();
            uint32_t currentSplitFlushCount = MediaState().wgcCap->GetSplitDeviceFlushCount();
            uint32_t currentSplitFlushSkippedCount = MediaState().wgcCap->GetSplitDeviceFlushSkippedCount();
            uint32_t currentPoolSlotFastRewriteCount = MediaState().wgcCap->GetPoolSlotFastRewriteCount();
            uint32_t currentPoolSaturatedDropCount = MediaState().wgcCap->GetPoolSaturatedDropCount();
            uint32_t currentPoolOverwritePreventedCount = MediaState().wgcCap->GetPoolSlotOverwritePreventedCount();
            uint32_t currentIngressAcceptedCount = MediaState().wgcCap->GetIngressAcceptedCount();
            uint32_t currentIngressDecimatedCount = MediaState().wgcCap->GetIngressDecimatedCount();
            uint32_t currentIngressAcceptedLowWaterCount = MediaState().wgcCap->GetIngressAcceptedLowWaterCount();
            uint32_t currentIngressAcceptedRecoveryCount = MediaState().wgcCap->GetIngressAcceptedRecoveryCount();
            uint32_t currentIngressAcceptedSourceBelowCount = MediaState().wgcCap->GetIngressAcceptedSourceBelowCount();
            uint32_t currentIngressAcceptedHealthyCount = MediaState().wgcCap->GetIngressAcceptedHealthyCount();
            uint32_t currentIngressAcceptedUniformPlayoutSoftReserveCount =
                MediaState().wgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            uint32_t currentIngressAcceptedUniformPlayoutCreditCount =
                MediaState().wgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            uint32_t currentIngressDecimatedSoftReserveCount = MediaState().wgcCap->GetIngressDecimatedSoftReserveCount();
            uint32_t currentIngressDecimatedHardReserveCount = MediaState().wgcCap->GetIngressDecimatedHardReserveCount();
            uint32_t currentIngressDecimatedCreditCount = MediaState().wgcCap->GetIngressDecimatedCreditCount();
            uint32_t currentIngressSoftReservePressureCount = MediaState().wgcCap->GetIngressSoftReservePressureCount();
            uint32_t currentIngressHardReservePressureCount = MediaState().wgcCap->GetIngressHardReservePressureCount();
            uint32_t inputFrames = currentInputCount - lastInputCount;
            uint32_t deliveredFrames = currentCount - lastCallbackCount;
            uint32_t hostDropDelta =
                static_cast<uint32_t>(queueDropped >= lastHostDroppedCount ? (queueDropped - lastHostDroppedCount) : 0);
            uint32_t pacingSkipDelta = currentPacingSkipCount - lastPacingSkipCount;
            uint32_t throttleSkipDelta = currentThrottleSkipCount - lastThrottleSkipCount;
            uint32_t staleSkipDelta = currentStaleSkipCount - lastStaleSkipCount;
            uint32_t staleDuplicateTsDelta = currentStaleDuplicateTsCount - lastStaleDuplicateTsCount;
            uint32_t staleOutOfOrderTsDelta = currentStaleOutOfOrderTsCount - lastStaleOutOfOrderTsCount;
            uint32_t normalizedDuplicateTsDelta = currentNormalizedDuplicateTsCount - lastNormalizedDuplicateTsCount;
            uint32_t duplicateTsSkipDelta = currentDuplicateTsSkipCount - lastDuplicateTsSkipCount;
            uint32_t cursorSkipDelta = currentCursorSkipCount - lastCursorSkipCount;
            uint32_t poolDropDelta = currentPoolDropCount - lastPoolDropCount;
            uint32_t keyedAcquireFailDelta = currentKeyedAcquireFailCount - lastKeyedAcquireFailCount;
            uint32_t keyedReleaseFailDelta = currentKeyedReleaseFailCount - lastKeyedReleaseFailCount;
            uint32_t keyedAbandonedReclaimDelta = currentKeyedAbandonedReclaimCount - lastKeyedAbandonedReclaimCount;
            uint32_t splitFlushDelta = currentSplitFlushCount - lastSplitFlushCount;
            uint32_t splitFlushSkippedDelta = currentSplitFlushSkippedCount - lastSplitFlushSkippedCount;
            uint32_t poolSlotFastRewriteDelta = currentPoolSlotFastRewriteCount - lastPoolSlotFastRewriteCount;
            uint32_t poolSaturatedDropDelta = currentPoolSaturatedDropCount - lastPoolSaturatedDropCount;
            uint32_t poolOverwritePreventedDelta = currentPoolOverwritePreventedCount - lastPoolOverwritePreventedCount;
            uint32_t ingressAcceptedDelta = currentIngressAcceptedCount - lastIngressAcceptedCount;
            uint32_t ingressDecimatedDelta = currentIngressDecimatedCount - lastIngressDecimatedCount;
            uint32_t ingressAcceptedLowWaterDelta =
                currentIngressAcceptedLowWaterCount - lastIngressAcceptedLowWaterCount;
            uint32_t ingressAcceptedRecoveryDelta =
                currentIngressAcceptedRecoveryCount - lastIngressAcceptedRecoveryCount;
            uint32_t ingressAcceptedSourceBelowDelta =
                currentIngressAcceptedSourceBelowCount - lastIngressAcceptedSourceBelowCount;
            uint32_t ingressAcceptedHealthyDelta = currentIngressAcceptedHealthyCount - lastIngressAcceptedHealthyCount;
            uint32_t ingressAcceptedUniformPlayoutSoftReserveDelta =
                currentIngressAcceptedUniformPlayoutSoftReserveCount -
                lastIngressAcceptedUniformPlayoutSoftReserveCount;
            uint32_t ingressAcceptedUniformPlayoutCreditDelta =
                currentIngressAcceptedUniformPlayoutCreditCount - lastIngressAcceptedUniformPlayoutCreditCount;
            uint32_t ingressDecimatedSoftReserveDelta =
                currentIngressDecimatedSoftReserveCount - lastIngressDecimatedSoftReserveCount;
            uint32_t ingressDecimatedHardReserveDelta =
                currentIngressDecimatedHardReserveCount - lastIngressDecimatedHardReserveCount;
            uint32_t ingressDecimatedCreditDelta = currentIngressDecimatedCreditCount - lastIngressDecimatedCreditCount;
            uint32_t ingressSoftReservePressureDelta =
                currentIngressSoftReservePressureCount - lastIngressSoftReservePressureCount;
            uint32_t ingressHardReservePressureDelta =
                currentIngressHardReservePressureCount - lastIngressHardReservePressureCount;
            uint32_t queuedFrames = deliveredFrames >= hostDropDelta ? (deliveredFrames - hostDropDelta) : 0;
            int64_t copyUs = MediaState().wgcCap->GetLastCopyTimeUs();
            int64_t srcIntervalAvgUs = MediaState().wgcCap->GetSourceIntervalAvgUs();
            int64_t srcJitterAvgUs = MediaState().wgcCap->GetSourceJitterAvgUs();
            int64_t srcJitterMaxUs = MediaState().wgcCap->GetSourceJitterMaxUs();
            int64_t srcToCopyAvgUs = MediaState().wgcCap->GetSourceToCopyLatencyAvgUs();
            int64_t srcToCopyMaxUs = MediaState().wgcCap->GetSourceToCopyLatencyMaxUs();
            int64_t poolSlotRewriteUs = MediaState().wgcCap->GetLastPoolSlotRewriteUs();
            int64_t callbackGapAvgUs = MediaState().wgcCap->GetCallbackGapAvgUs();
            int64_t callbackGapMaxUs = MediaState().wgcCap->GetCallbackGapMaxUs();
            int64_t callbackProcessAvgUs = MediaState().wgcCap->GetCallbackProcessAvgUs();
            int64_t callbackProcessMaxUs = MediaState().wgcCap->GetCallbackProcessMaxUs();
            uint32_t callbackDrainMax = MediaState().wgcCap->GetCallbackDrainMaxCount();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(MediaState().frameQueue.Size());
            uint32_t cadenceSelAvgUs = 0;
            int32_t cadenceSelBiasUs = 0;
            uint32_t wgcSelAvgUs = 0;
            int32_t wgcSelBiasUs = 0;
            uint32_t throttleTargetFps = MediaState().wgcCap->GetProducerTargetFps();
            const uint32_t deliveredRatePerSec = MediaState().wgcCap->GetDeliveredRatePerSec();
            const uint32_t deliveredMin250Fps = MediaState().wgcCap->GetDeliveredMin250Fps();
            const uint32_t deliveredMin500Fps = MediaState().wgcCap->GetDeliveredMin500Fps();
            const uint32_t inputMin250Fps = MediaState().wgcCap->GetInputMin250Fps();
            const uint32_t inputMin500Fps = MediaState().wgcCap->GetInputMin500Fps();
            const uint32_t queueEmptyPermille =
                MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickAvgPermille =
                MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickMin =
                MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed) : 0u;
            const uint32_t starvedTicks =
                MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.wgcStarvedTickCount.load(std::memory_order_relaxed) : 0u;
            const uint32_t singleFrameTicks =
                MediaState().pSharedMem ? MediaState().pSharedMem->runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed) : 0u;
            if (MediaState().pSharedMem) {
                uint32_t currentDup = MediaState().pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = MediaState().pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                overloadFlags = MediaState().pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = MediaState().pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = MediaState().pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                cadenceSelAvgUs = MediaState().pSharedMem->runtimeState.selectionErrorAvgUs.load(std::memory_order_relaxed);
                cadenceSelBiasUs = MediaState().pSharedMem->runtimeState.selectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                wgcSelAvgUs = MediaState().pSharedMem->runtimeState.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed);
                wgcSelBiasUs = MediaState().pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcSourceFrameIntervalAvgUs.store(SaturatingToUint32(srcIntervalAvgUs),
                                                                             std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcSourceFrameJitterAvgUs.store(SaturatingToUint32(srcJitterAvgUs),
                                                                           std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcSourceFrameJitterMaxUs.store(SaturatingToUint32(srcJitterMaxUs),
                                                                           std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcSourceToCopyLatencyAvgUs.store(SaturatingToUint32(srcToCopyAvgUs),
                                                                             std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcSourceToCopyLatencyMaxUs.store(SaturatingToUint32(srcToCopyMaxUs),
                                                                             std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcTargetFps.store(throttleTargetFps, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcDeliveredFramesPerSec.store(deliveredRatePerSec,
                                                                          std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcDeliveredMin250Fps.store(deliveredMin250Fps, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcDeliveredMin500Fps.store(deliveredMin500Fps, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcInputMin250Fps.store(inputMin250Fps, std::memory_order_relaxed);
                MediaState().pSharedMem->runtimeState.wgcInputMin500Fps.store(inputMin500Fps, std::memory_order_relaxed);
            }

            SnapshotPublishedWgcRuntimeLogState();
            LogInfo(
                "[WGC Perf] Input: %u | Queued: %u | DropFull: %u | DropPace: %u | DropThrottle: %u | "
                "DropStale: %u (DupTs=%u OOO=%u) | SrcDupTs: seen=%u skip=%u | DropCursor: %u | "
                "DropPool: %u | DropIngress: %u | "
                "HostQ: %u | EncQ: %u | Dup: %u | Late: %u | "
                "SrcAvg: %lldus | JitAvg: %lldus | JitMax: %lldus | Src->Copy: %lld/%lldus | Deliv: %u | "
                "MinIn250/500: %u/%u | MinDel250/500: %u/%u | FreshMiss: %upm | BufAvg: %upm | BufMin: %u | "
                "NoFresh: %u | NoReserve: %u | SchedSelAvg: %uus "
                "SchedSelBias: %dus | WgcSelAvg: %uus WgcSelBias: %dus | CbGap: %lld/%lldus "
                "CbProc: %lld/%lldus CbDrainMax: %u | Copy: %lldus | "
                "SlotAge: %lldus FastSlot: %u | PoolLease: max=%u freeMin=%u satDrop=%u overwritePrevented=%u "
                "mismatch=%u sourceFramePoolBuffers=%u copyPoolSlots=%u budgetSurfaces=%u syncFrames=%u "
                "extraFrames=%u retainedCap=%u reservedFree=%u safetySlots=%u "
                "sourceFmt=%u copyFmt=%u compactRetained=%d sourceBudgetMB=%.1f copyBudgetMB=%.1f "
                "sourceSurfaceMB=%.1f copySurfaceMB=%.1f convertUs=%lld | "
                "Ingress: accepted=%u decimated=%u retained=%u/%u lowWater=%u reason=%s "
                "accLow=%u accRec=%u accSrcBelow=%u accHealthy=%u accPlaySoft=%u accPlayCredit=%u "
                "decSoft=%u decHard=%u decCredit=%u softPress=%u hardPress=%u | "
                "KMFail: %u/%u KMReclaim: %u | Flush: %u/%u | "
                "Dedicated: %d | Encode: %lldus | Fence: %lldus | Throttle: %u | Mux: %uKB | Overload: 0x%X | "
                "Backend: %s DupIdleTimeouts: %llu DupMissed: %llu DupHwCursor: %d DupCursorEmbedded: %d "
                "DupPtrTransitions: %llu DupPtrUpdates: %llu DupPtrForwarded: %llu DupPtrPublished: %llu | "
                "TimingBasis: Copy/Convert/Encode/Fence=CPU-wall-or-submit",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                staleDuplicateTsDelta, staleOutOfOrderTsDelta, normalizedDuplicateTsDelta, duplicateTsSkipDelta,
                cursorSkipDelta, poolDropDelta, ingressDecimatedDelta, static_cast<uint32_t>(MediaState().frameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs, srcJitterAvgUs, srcJitterMaxUs,
                srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps, inputMin500Fps, deliveredMin250Fps,
                deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille, bufferedAtTickMin, starvedTicks,
                singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, wgcSelAvgUs, wgcSelBiasUs, callbackGapAvgUs,
                callbackGapMaxUs, callbackProcessAvgUs, callbackProcessMaxUs, callbackDrainMax, copyUs,
                poolSlotRewriteUs, poolSlotFastRewriteDelta, MediaState().wgcCap->GetPoolSlotLeasedMaxCount(),
                MediaState().wgcCap->GetPoolSlotFreeMinCount(), poolSaturatedDropDelta, poolOverwritePreventedDelta,
                MediaState().wgcCap->GetPoolLeaseMismatchCount(), MediaState().wgcCap->GetSourceFramePoolBufferCount(),
                MediaState().wgcCap->GetTexturePoolSlotCount(), MediaState().wgcCap->GetSmoothnessBudgetSurfaceCount(),
                MediaState().wgcCap->GetSmoothnessSyncFrameCount(), MediaState().wgcCap->GetSmoothnessRetainedFrameCount(),
                MediaState().wgcCap->GetSmoothnessRetainedFrameCap(), MediaState().wgcCap->GetSmoothnessReservedFreeSlotCount(),
                MediaState().wgcCap->GetSmoothnessSafetySlotCount(), MediaState().wgcCap->GetSmoothnessSourceDxgiFormat(),
                MediaState().wgcCap->GetSmoothnessCopyDxgiFormat(), MediaState().wgcCap->IsCompactRetainedCopyActive() ? 1 : 0,
                static_cast<double>(MediaState().wgcCap->GetSmoothnessSourceEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(MediaState().wgcCap->GetSmoothnessCopyEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(MediaState().wgcCap->GetSmoothnessSourceBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<double>(MediaState().wgcCap->GetSmoothnessCopyBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<long long>(MediaState().wgcCap->GetLastPoolConvertTimeUs()), ingressAcceptedDelta,
                ingressDecimatedDelta, MediaState().wgcCap->GetIngressRetainedFrameCount(), MediaState().wgcCap->GetIngressRetainedFrameCap(),
                MediaState().wgcCap->GetIngressLowWaterFrameCount(),
                WgcIngressAdmissionReasonName(MediaState().wgcCap->GetIngressAdmissionReasonCode()), ingressAcceptedLowWaterDelta,
                ingressAcceptedRecoveryDelta, ingressAcceptedSourceBelowDelta, ingressAcceptedHealthyDelta,
                ingressAcceptedUniformPlayoutSoftReserveDelta, ingressAcceptedUniformPlayoutCreditDelta,
                ingressDecimatedSoftReserveDelta, ingressDecimatedHardReserveDelta, ingressDecimatedCreditDelta,
                ingressSoftReservePressureDelta, ingressHardReservePressureDelta, keyedAcquireFailDelta,
                keyedReleaseFailDelta, keyedAbandonedReclaimDelta, splitFlushDelta, splitFlushSkippedDelta,
                MediaState().wgcCap->IsUsingDedicatedCaptureDevice() ? 1 : 0, encodeUs, fenceUs, throttleTargetFps,
                (muxQueueBytes + 1023u) / 1024u, overloadFlags,
                MediaState().wgcCap->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
                static_cast<unsigned long long>(MediaState().wgcCap->GetDuplicationAcquireTimeoutCount()),
                static_cast<unsigned long long>(MediaState().wgcCap->GetDuplicationAccumulatedMissedFrameCount()),
                MediaState().wgcCap->IsDuplicationSeparatePointerVisible() ? 1 : 0,
                MediaState().wgcCap->IsDuplicationCursorEmbedded() ? 1 : 0,
                static_cast<unsigned long long>(MediaState().wgcCap->GetDuplicationPointerStateTransitionCount()),
                static_cast<unsigned long long>(MediaState().wgcCap->GetDuplicationPointerUpdateCount()),
                static_cast<unsigned long long>(MediaState().wgcCap->GetDuplicationForwardedPointerUpdateCount()),
                static_cast<unsigned long long>(MediaState().dxgiCursorTimelinePublished.load(std::memory_order_relaxed)));

            lastInputCount = currentInputCount;
            lastCallbackCount = currentCount;
            lastHostDroppedCount = queueDropped;
            lastPacingSkipCount = currentPacingSkipCount;
            lastThrottleSkipCount = currentThrottleSkipCount;
            lastStaleSkipCount = currentStaleSkipCount;
            lastStaleDuplicateTsCount = currentStaleDuplicateTsCount;
            lastStaleOutOfOrderTsCount = currentStaleOutOfOrderTsCount;
            lastNormalizedDuplicateTsCount = currentNormalizedDuplicateTsCount;
            lastDuplicateTsSkipCount = currentDuplicateTsSkipCount;
            lastCursorSkipCount = currentCursorSkipCount;
            lastPoolDropCount = currentPoolDropCount;
            lastKeyedAcquireFailCount = currentKeyedAcquireFailCount;
            lastKeyedReleaseFailCount = currentKeyedReleaseFailCount;
            lastKeyedAbandonedReclaimCount = currentKeyedAbandonedReclaimCount;
            lastSplitFlushCount = currentSplitFlushCount;
            lastSplitFlushSkippedCount = currentSplitFlushSkippedCount;
            lastPoolSlotFastRewriteCount = currentPoolSlotFastRewriteCount;
            lastPoolSaturatedDropCount = currentPoolSaturatedDropCount;
            lastPoolOverwritePreventedCount = currentPoolOverwritePreventedCount;
            lastIngressAcceptedCount = currentIngressAcceptedCount;
            lastIngressDecimatedCount = currentIngressDecimatedCount;
            lastIngressAcceptedLowWaterCount = currentIngressAcceptedLowWaterCount;
            lastIngressAcceptedRecoveryCount = currentIngressAcceptedRecoveryCount;
            lastIngressAcceptedSourceBelowCount = currentIngressAcceptedSourceBelowCount;
            lastIngressAcceptedHealthyCount = currentIngressAcceptedHealthyCount;
            lastIngressAcceptedUniformPlayoutSoftReserveCount = currentIngressAcceptedUniformPlayoutSoftReserveCount;
            lastIngressAcceptedUniformPlayoutCreditCount = currentIngressAcceptedUniformPlayoutCreditCount;
            lastIngressDecimatedSoftReserveCount = currentIngressDecimatedSoftReserveCount;
            lastIngressDecimatedHardReserveCount = currentIngressDecimatedHardReserveCount;
            lastIngressDecimatedCreditCount = currentIngressDecimatedCreditCount;
            lastIngressSoftReservePressureCount = currentIngressSoftReservePressureCount;
            lastIngressHardReservePressureCount = currentIngressHardReservePressureCount;
            lastDiagTime = now;
        }
    }

    MediaState().wgcCaptureRunning = false;
    LogInfo("[WGC CaptureThread] Stopped");
}
