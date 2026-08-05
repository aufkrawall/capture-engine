#include "media_main_internal.h"

void WgcCaptureThreadFunc(const AppConfig& config) {
    LogInfo("[WGC CaptureThread] Started (diagnostics logger)");
    media_main_g_WgcCaptureRunning = true;

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

    while (!media_main_g_WgcCaptureShutdown) {
        Sleep(1000);

        if (!media_main_g_Recording || !media_main_g_WgcCap) {
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
            lastInputCount = media_main_g_WgcCap->GetInputFrameCount();
            lastCallbackCount = media_main_g_WgcCap->GetCallbackFrameCount();
            lastHostDroppedCount = media_main_g_FrameQueue.GetDroppedCount();
            lastPacingSkipCount = media_main_g_WgcCap->GetPacingSkipCount();
            lastThrottleSkipCount = media_main_g_WgcCap->GetThrottleSkipCount();
            lastStaleSkipCount = media_main_g_WgcCap->GetStaleSkipCount();
            lastStaleDuplicateTsCount = media_main_g_WgcCap->GetStaleDuplicateTimestampCount();
            lastStaleOutOfOrderTsCount = media_main_g_WgcCap->GetStaleOutOfOrderTimestampCount();
            lastNormalizedDuplicateTsCount = media_main_g_WgcCap->GetNormalizedDuplicateTimestampCount();
            lastDuplicateTsSkipCount = media_main_g_WgcCap->GetDuplicateTimestampSkipCount();
            lastCursorSkipCount = media_main_g_WgcCap->GetCursorOnlySkipCount();
            lastPoolDropCount = media_main_g_WgcCap->GetPoolDropCount();
            lastKeyedAcquireFailCount = media_main_g_WgcCap->GetKeyedMutexAcquireFailCount();
            lastKeyedReleaseFailCount = media_main_g_WgcCap->GetKeyedMutexReleaseFailCount();
            lastKeyedAbandonedReclaimCount = media_main_g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            lastSplitFlushCount = media_main_g_WgcCap->GetSplitDeviceFlushCount();
            lastSplitFlushSkippedCount = media_main_g_WgcCap->GetSplitDeviceFlushSkippedCount();
            lastPoolSlotFastRewriteCount = media_main_g_WgcCap->GetPoolSlotFastRewriteCount();
            lastPoolSaturatedDropCount = media_main_g_WgcCap->GetPoolSaturatedDropCount();
            lastPoolOverwritePreventedCount = media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
            lastIngressAcceptedCount = media_main_g_WgcCap->GetIngressAcceptedCount();
            lastIngressDecimatedCount = media_main_g_WgcCap->GetIngressDecimatedCount();
            lastIngressAcceptedLowWaterCount = media_main_g_WgcCap->GetIngressAcceptedLowWaterCount();
            lastIngressAcceptedRecoveryCount = media_main_g_WgcCap->GetIngressAcceptedRecoveryCount();
            lastIngressAcceptedSourceBelowCount = media_main_g_WgcCap->GetIngressAcceptedSourceBelowCount();
            lastIngressAcceptedHealthyCount = media_main_g_WgcCap->GetIngressAcceptedHealthyCount();
            lastIngressAcceptedUniformPlayoutSoftReserveCount =
                media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            lastIngressAcceptedUniformPlayoutCreditCount = media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            lastIngressDecimatedSoftReserveCount = media_main_g_WgcCap->GetIngressDecimatedSoftReserveCount();
            lastIngressDecimatedHardReserveCount = media_main_g_WgcCap->GetIngressDecimatedHardReserveCount();
            lastIngressDecimatedCreditCount = media_main_g_WgcCap->GetIngressDecimatedCreditCount();
            lastIngressSoftReservePressureCount = media_main_g_WgcCap->GetIngressSoftReservePressureCount();
            lastIngressHardReservePressureCount = media_main_g_WgcCap->GetIngressHardReservePressureCount();
            SnapshotPublishedWgcRuntimeLogState();
            if (media_main_g_pSharedMem) {
                lastDuplicateCount = media_main_g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                lastLateCount = media_main_g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
            }
            lastDiagTime = GetTickCount();
            sessionPrimed = true;
            continue;
        }

        DWORD now = GetTickCount();
        if (now - lastDiagTime >= 1000) {
            uint32_t currentInputCount = media_main_g_WgcCap->GetInputFrameCount();
            uint32_t currentCount = media_main_g_WgcCap->GetCallbackFrameCount();
            uint64_t queueDropped = media_main_g_FrameQueue.GetDroppedCount();
            uint32_t currentPacingSkipCount = media_main_g_WgcCap->GetPacingSkipCount();
            uint32_t currentThrottleSkipCount = media_main_g_WgcCap->GetThrottleSkipCount();
            uint32_t currentStaleSkipCount = media_main_g_WgcCap->GetStaleSkipCount();
            uint32_t currentStaleDuplicateTsCount = media_main_g_WgcCap->GetStaleDuplicateTimestampCount();
            uint32_t currentStaleOutOfOrderTsCount = media_main_g_WgcCap->GetStaleOutOfOrderTimestampCount();
            uint32_t currentNormalizedDuplicateTsCount = media_main_g_WgcCap->GetNormalizedDuplicateTimestampCount();
            uint32_t currentDuplicateTsSkipCount = media_main_g_WgcCap->GetDuplicateTimestampSkipCount();
            uint32_t currentCursorSkipCount = media_main_g_WgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = media_main_g_WgcCap->GetPoolDropCount();
            uint32_t currentKeyedAcquireFailCount = media_main_g_WgcCap->GetKeyedMutexAcquireFailCount();
            uint32_t currentKeyedReleaseFailCount = media_main_g_WgcCap->GetKeyedMutexReleaseFailCount();
            uint32_t currentKeyedAbandonedReclaimCount = media_main_g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            uint32_t currentSplitFlushCount = media_main_g_WgcCap->GetSplitDeviceFlushCount();
            uint32_t currentSplitFlushSkippedCount = media_main_g_WgcCap->GetSplitDeviceFlushSkippedCount();
            uint32_t currentPoolSlotFastRewriteCount = media_main_g_WgcCap->GetPoolSlotFastRewriteCount();
            uint32_t currentPoolSaturatedDropCount = media_main_g_WgcCap->GetPoolSaturatedDropCount();
            uint32_t currentPoolOverwritePreventedCount = media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
            uint32_t currentIngressAcceptedCount = media_main_g_WgcCap->GetIngressAcceptedCount();
            uint32_t currentIngressDecimatedCount = media_main_g_WgcCap->GetIngressDecimatedCount();
            uint32_t currentIngressAcceptedLowWaterCount = media_main_g_WgcCap->GetIngressAcceptedLowWaterCount();
            uint32_t currentIngressAcceptedRecoveryCount = media_main_g_WgcCap->GetIngressAcceptedRecoveryCount();
            uint32_t currentIngressAcceptedSourceBelowCount = media_main_g_WgcCap->GetIngressAcceptedSourceBelowCount();
            uint32_t currentIngressAcceptedHealthyCount = media_main_g_WgcCap->GetIngressAcceptedHealthyCount();
            uint32_t currentIngressAcceptedUniformPlayoutSoftReserveCount =
                media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            uint32_t currentIngressAcceptedUniformPlayoutCreditCount =
                media_main_g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            uint32_t currentIngressDecimatedSoftReserveCount = media_main_g_WgcCap->GetIngressDecimatedSoftReserveCount();
            uint32_t currentIngressDecimatedHardReserveCount = media_main_g_WgcCap->GetIngressDecimatedHardReserveCount();
            uint32_t currentIngressDecimatedCreditCount = media_main_g_WgcCap->GetIngressDecimatedCreditCount();
            uint32_t currentIngressSoftReservePressureCount = media_main_g_WgcCap->GetIngressSoftReservePressureCount();
            uint32_t currentIngressHardReservePressureCount = media_main_g_WgcCap->GetIngressHardReservePressureCount();
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
            int64_t copyUs = media_main_g_WgcCap->GetLastCopyTimeUs();
            int64_t srcIntervalAvgUs = media_main_g_WgcCap->GetSourceIntervalAvgUs();
            int64_t srcJitterAvgUs = media_main_g_WgcCap->GetSourceJitterAvgUs();
            int64_t srcJitterMaxUs = media_main_g_WgcCap->GetSourceJitterMaxUs();
            int64_t srcToCopyAvgUs = media_main_g_WgcCap->GetSourceToCopyLatencyAvgUs();
            int64_t srcToCopyMaxUs = media_main_g_WgcCap->GetSourceToCopyLatencyMaxUs();
            int64_t poolSlotRewriteUs = media_main_g_WgcCap->GetLastPoolSlotRewriteUs();
            int64_t callbackGapAvgUs = media_main_g_WgcCap->GetCallbackGapAvgUs();
            int64_t callbackGapMaxUs = media_main_g_WgcCap->GetCallbackGapMaxUs();
            int64_t callbackProcessAvgUs = media_main_g_WgcCap->GetCallbackProcessAvgUs();
            int64_t callbackProcessMaxUs = media_main_g_WgcCap->GetCallbackProcessMaxUs();
            uint32_t callbackDrainMax = media_main_g_WgcCap->GetCallbackDrainMaxCount();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(media_main_g_FrameQueue.Size());
            uint32_t cadenceSelAvgUs = 0;
            int32_t cadenceSelBiasUs = 0;
            uint32_t wgcSelAvgUs = 0;
            int32_t wgcSelBiasUs = 0;
            uint32_t throttleTargetFps = media_main_g_WgcCap->GetProducerTargetFps();
            const uint32_t deliveredRatePerSec = media_main_g_WgcCap->GetDeliveredRatePerSec();
            const uint32_t deliveredMin250Fps = media_main_g_WgcCap->GetDeliveredMin250Fps();
            const uint32_t deliveredMin500Fps = media_main_g_WgcCap->GetDeliveredMin500Fps();
            const uint32_t inputMin250Fps = media_main_g_WgcCap->GetInputMin250Fps();
            const uint32_t inputMin500Fps = media_main_g_WgcCap->GetInputMin500Fps();
            const uint32_t queueEmptyPermille =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickAvgPermille =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickMin =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed) : 0u;
            const uint32_t starvedTicks =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.wgcStarvedTickCount.load(std::memory_order_relaxed) : 0u;
            const uint32_t singleFrameTicks =
                media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed) : 0u;
            if (media_main_g_pSharedMem) {
                uint32_t currentDup = media_main_g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = media_main_g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                overloadFlags = media_main_g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = media_main_g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = media_main_g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                cadenceSelAvgUs = media_main_g_pSharedMem->runtimeState.selectionErrorAvgUs.load(std::memory_order_relaxed);
                cadenceSelBiasUs = media_main_g_pSharedMem->runtimeState.selectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                wgcSelAvgUs = media_main_g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed);
                wgcSelBiasUs = media_main_g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcSourceFrameIntervalAvgUs.store(SaturatingToUint32(srcIntervalAvgUs),
                                                                             std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcSourceFrameJitterAvgUs.store(SaturatingToUint32(srcJitterAvgUs),
                                                                           std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcSourceFrameJitterMaxUs.store(SaturatingToUint32(srcJitterMaxUs),
                                                                           std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcSourceToCopyLatencyAvgUs.store(SaturatingToUint32(srcToCopyAvgUs),
                                                                             std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcSourceToCopyLatencyMaxUs.store(SaturatingToUint32(srcToCopyMaxUs),
                                                                             std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcTargetFps.store(throttleTargetFps, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcDeliveredFramesPerSec.store(deliveredRatePerSec,
                                                                          std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcDeliveredMin250Fps.store(deliveredMin250Fps, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcDeliveredMin500Fps.store(deliveredMin500Fps, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcInputMin250Fps.store(inputMin250Fps, std::memory_order_relaxed);
                media_main_g_pSharedMem->runtimeState.wgcInputMin500Fps.store(inputMin500Fps, std::memory_order_relaxed);
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
                cursorSkipDelta, poolDropDelta, ingressDecimatedDelta, static_cast<uint32_t>(media_main_g_FrameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs, srcJitterAvgUs, srcJitterMaxUs,
                srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps, inputMin500Fps, deliveredMin250Fps,
                deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille, bufferedAtTickMin, starvedTicks,
                singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, wgcSelAvgUs, wgcSelBiasUs, callbackGapAvgUs,
                callbackGapMaxUs, callbackProcessAvgUs, callbackProcessMaxUs, callbackDrainMax, copyUs,
                poolSlotRewriteUs, poolSlotFastRewriteDelta, media_main_g_WgcCap->GetPoolSlotLeasedMaxCount(),
                media_main_g_WgcCap->GetPoolSlotFreeMinCount(), poolSaturatedDropDelta, poolOverwritePreventedDelta,
                media_main_g_WgcCap->GetPoolLeaseMismatchCount(), media_main_g_WgcCap->GetSourceFramePoolBufferCount(),
                media_main_g_WgcCap->GetTexturePoolSlotCount(), media_main_g_WgcCap->GetSmoothnessBudgetSurfaceCount(),
                media_main_g_WgcCap->GetSmoothnessSyncFrameCount(), media_main_g_WgcCap->GetSmoothnessRetainedFrameCount(),
                media_main_g_WgcCap->GetSmoothnessRetainedFrameCap(), media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount(),
                media_main_g_WgcCap->GetSmoothnessSafetySlotCount(), media_main_g_WgcCap->GetSmoothnessSourceDxgiFormat(),
                media_main_g_WgcCap->GetSmoothnessCopyDxgiFormat(), media_main_g_WgcCap->IsCompactRetainedCopyActive() ? 1 : 0,
                static_cast<double>(media_main_g_WgcCap->GetSmoothnessSourceEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(media_main_g_WgcCap->GetSmoothnessCopyEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(media_main_g_WgcCap->GetSmoothnessSourceBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<double>(media_main_g_WgcCap->GetSmoothnessCopyBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<long long>(media_main_g_WgcCap->GetLastPoolConvertTimeUs()), ingressAcceptedDelta,
                ingressDecimatedDelta, media_main_g_WgcCap->GetIngressRetainedFrameCount(), media_main_g_WgcCap->GetIngressRetainedFrameCap(),
                media_main_g_WgcCap->GetIngressLowWaterFrameCount(),
                WgcIngressAdmissionReasonName(media_main_g_WgcCap->GetIngressAdmissionReasonCode()), ingressAcceptedLowWaterDelta,
                ingressAcceptedRecoveryDelta, ingressAcceptedSourceBelowDelta, ingressAcceptedHealthyDelta,
                ingressAcceptedUniformPlayoutSoftReserveDelta, ingressAcceptedUniformPlayoutCreditDelta,
                ingressDecimatedSoftReserveDelta, ingressDecimatedHardReserveDelta, ingressDecimatedCreditDelta,
                ingressSoftReservePressureDelta, ingressHardReservePressureDelta, keyedAcquireFailDelta,
                keyedReleaseFailDelta, keyedAbandonedReclaimDelta, splitFlushDelta, splitFlushSkippedDelta,
                media_main_g_WgcCap->IsUsingDedicatedCaptureDevice() ? 1 : 0, encodeUs, fenceUs, throttleTargetFps,
                (muxQueueBytes + 1023u) / 1024u, overloadFlags,
                media_main_g_WgcCap->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
                static_cast<unsigned long long>(media_main_g_WgcCap->GetDuplicationAcquireTimeoutCount()),
                static_cast<unsigned long long>(media_main_g_WgcCap->GetDuplicationAccumulatedMissedFrameCount()),
                media_main_g_WgcCap->IsDuplicationSeparatePointerVisible() ? 1 : 0,
                media_main_g_WgcCap->IsDuplicationCursorEmbedded() ? 1 : 0,
                static_cast<unsigned long long>(media_main_g_WgcCap->GetDuplicationPointerStateTransitionCount()),
                static_cast<unsigned long long>(media_main_g_WgcCap->GetDuplicationPointerUpdateCount()),
                static_cast<unsigned long long>(media_main_g_WgcCap->GetDuplicationForwardedPointerUpdateCount()),
                static_cast<unsigned long long>(media_main_g_DxgiCursorTimelinePublished.load(std::memory_order_relaxed)));

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

    media_main_g_WgcCaptureRunning = false;
    LogInfo("[WGC CaptureThread] Stopped");
}
