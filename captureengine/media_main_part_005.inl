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

    while (!g_WgcCaptureShutdown) {
        Sleep(1000);

        if (!g_Recording || !g_WgcCap) {
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
            lastInputCount = g_WgcCap->GetInputFrameCount();
            lastCallbackCount = g_WgcCap->GetCallbackFrameCount();
            lastHostDroppedCount = g_FrameQueue.GetDroppedCount();
            lastPacingSkipCount = g_WgcCap->GetPacingSkipCount();
            lastThrottleSkipCount = g_WgcCap->GetThrottleSkipCount();
            lastStaleSkipCount = g_WgcCap->GetStaleSkipCount();
            lastStaleDuplicateTsCount = g_WgcCap->GetStaleDuplicateTimestampCount();
            lastStaleOutOfOrderTsCount = g_WgcCap->GetStaleOutOfOrderTimestampCount();
            lastNormalizedDuplicateTsCount = g_WgcCap->GetNormalizedDuplicateTimestampCount();
            lastDuplicateTsSkipCount = g_WgcCap->GetDuplicateTimestampSkipCount();
            lastCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            lastPoolDropCount = g_WgcCap->GetPoolDropCount();
            lastKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            lastKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            lastKeyedAbandonedReclaimCount = g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            lastSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            lastSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            lastPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
            lastPoolSaturatedDropCount = g_WgcCap->GetPoolSaturatedDropCount();
            lastPoolOverwritePreventedCount = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            lastIngressAcceptedCount = g_WgcCap->GetIngressAcceptedCount();
            lastIngressDecimatedCount = g_WgcCap->GetIngressDecimatedCount();
            lastIngressAcceptedLowWaterCount = g_WgcCap->GetIngressAcceptedLowWaterCount();
            lastIngressAcceptedRecoveryCount = g_WgcCap->GetIngressAcceptedRecoveryCount();
            lastIngressAcceptedSourceBelowCount = g_WgcCap->GetIngressAcceptedSourceBelowCount();
            lastIngressAcceptedHealthyCount = g_WgcCap->GetIngressAcceptedHealthyCount();
            lastIngressAcceptedUniformPlayoutSoftReserveCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            lastIngressAcceptedUniformPlayoutCreditCount = g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            lastIngressDecimatedSoftReserveCount = g_WgcCap->GetIngressDecimatedSoftReserveCount();
            lastIngressDecimatedHardReserveCount = g_WgcCap->GetIngressDecimatedHardReserveCount();
            lastIngressDecimatedCreditCount = g_WgcCap->GetIngressDecimatedCreditCount();
            lastIngressSoftReservePressureCount = g_WgcCap->GetIngressSoftReservePressureCount();
            lastIngressHardReservePressureCount = g_WgcCap->GetIngressHardReservePressureCount();
            SnapshotPublishedWgcRuntimeLogState();
            if (g_pSharedMem) {
                lastDuplicateCount = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                lastLateCount = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
            }
            lastDiagTime = GetTickCount();
            sessionPrimed = true;
            continue;
        }

        DWORD now = GetTickCount();
        if (now - lastDiagTime >= 1000) {
            uint32_t currentInputCount = g_WgcCap->GetInputFrameCount();
            uint32_t currentCount = g_WgcCap->GetCallbackFrameCount();
            uint64_t queueDropped = g_FrameQueue.GetDroppedCount();
            uint32_t currentPacingSkipCount = g_WgcCap->GetPacingSkipCount();
            uint32_t currentThrottleSkipCount = g_WgcCap->GetThrottleSkipCount();
            uint32_t currentStaleSkipCount = g_WgcCap->GetStaleSkipCount();
            uint32_t currentStaleDuplicateTsCount = g_WgcCap->GetStaleDuplicateTimestampCount();
            uint32_t currentStaleOutOfOrderTsCount = g_WgcCap->GetStaleOutOfOrderTimestampCount();
            uint32_t currentNormalizedDuplicateTsCount = g_WgcCap->GetNormalizedDuplicateTimestampCount();
            uint32_t currentDuplicateTsSkipCount = g_WgcCap->GetDuplicateTimestampSkipCount();
            uint32_t currentCursorSkipCount = g_WgcCap->GetCursorOnlySkipCount();
            uint32_t currentPoolDropCount = g_WgcCap->GetPoolDropCount();
            uint32_t currentKeyedAcquireFailCount = g_WgcCap->GetKeyedMutexAcquireFailCount();
            uint32_t currentKeyedReleaseFailCount = g_WgcCap->GetKeyedMutexReleaseFailCount();
            uint32_t currentKeyedAbandonedReclaimCount = g_WgcCap->GetKeyedMutexAbandonedReclaimCount();
            uint32_t currentSplitFlushCount = g_WgcCap->GetSplitDeviceFlushCount();
            uint32_t currentSplitFlushSkippedCount = g_WgcCap->GetSplitDeviceFlushSkippedCount();
            uint32_t currentPoolSlotFastRewriteCount = g_WgcCap->GetPoolSlotFastRewriteCount();
            uint32_t currentPoolSaturatedDropCount = g_WgcCap->GetPoolSaturatedDropCount();
            uint32_t currentPoolOverwritePreventedCount = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            uint32_t currentIngressAcceptedCount = g_WgcCap->GetIngressAcceptedCount();
            uint32_t currentIngressDecimatedCount = g_WgcCap->GetIngressDecimatedCount();
            uint32_t currentIngressAcceptedLowWaterCount = g_WgcCap->GetIngressAcceptedLowWaterCount();
            uint32_t currentIngressAcceptedRecoveryCount = g_WgcCap->GetIngressAcceptedRecoveryCount();
            uint32_t currentIngressAcceptedSourceBelowCount = g_WgcCap->GetIngressAcceptedSourceBelowCount();
            uint32_t currentIngressAcceptedHealthyCount = g_WgcCap->GetIngressAcceptedHealthyCount();
            uint32_t currentIngressAcceptedUniformPlayoutSoftReserveCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount();
            uint32_t currentIngressAcceptedUniformPlayoutCreditCount =
                g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount();
            uint32_t currentIngressDecimatedSoftReserveCount = g_WgcCap->GetIngressDecimatedSoftReserveCount();
            uint32_t currentIngressDecimatedHardReserveCount = g_WgcCap->GetIngressDecimatedHardReserveCount();
            uint32_t currentIngressDecimatedCreditCount = g_WgcCap->GetIngressDecimatedCreditCount();
            uint32_t currentIngressSoftReservePressureCount = g_WgcCap->GetIngressSoftReservePressureCount();
            uint32_t currentIngressHardReservePressureCount = g_WgcCap->GetIngressHardReservePressureCount();
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
            int64_t copyUs = g_WgcCap->GetLastCopyTimeUs();
            int64_t srcIntervalAvgUs = g_WgcCap->GetSourceIntervalAvgUs();
            int64_t srcJitterAvgUs = g_WgcCap->GetSourceJitterAvgUs();
            int64_t srcJitterMaxUs = g_WgcCap->GetSourceJitterMaxUs();
            int64_t srcToCopyAvgUs = g_WgcCap->GetSourceToCopyLatencyAvgUs();
            int64_t srcToCopyMaxUs = g_WgcCap->GetSourceToCopyLatencyMaxUs();
            int64_t poolSlotRewriteUs = g_WgcCap->GetLastPoolSlotRewriteUs();
            int64_t callbackGapAvgUs = g_WgcCap->GetCallbackGapAvgUs();
            int64_t callbackGapMaxUs = g_WgcCap->GetCallbackGapMaxUs();
            int64_t callbackProcessAvgUs = g_WgcCap->GetCallbackProcessAvgUs();
            int64_t callbackProcessMaxUs = g_WgcCap->GetCallbackProcessMaxUs();
            uint32_t callbackDrainMax = g_WgcCap->GetCallbackDrainMaxCount();
            int64_t encodeUs = MediaEngine_GetLastFrameEncodeTimeUs();
            int64_t fenceUs = MediaEngine_GetLastFrameFenceWaitUs();
            uint32_t dupDelta = 0;
            uint32_t lateDelta = 0;
            uint32_t overloadFlags = 0;
            uint32_t muxQueueBytes = 0;
            uint32_t encoderQueueDepth = static_cast<uint32_t>(g_FrameQueue.Size());
            uint32_t cadenceSelAvgUs = 0;
            int32_t cadenceSelBiasUs = 0;
            uint32_t wgcSelAvgUs = 0;
            int32_t wgcSelBiasUs = 0;
            uint32_t throttleTargetFps = g_WgcCap->GetProducerTargetFps();
            const uint32_t deliveredRatePerSec = g_WgcCap->GetDeliveredRatePerSec();
            const uint32_t deliveredMin250Fps = g_WgcCap->GetDeliveredMin250Fps();
            const uint32_t deliveredMin500Fps = g_WgcCap->GetDeliveredMin500Fps();
            const uint32_t inputMin250Fps = g_WgcCap->GetInputMin250Fps();
            const uint32_t inputMin500Fps = g_WgcCap->GetInputMin500Fps();
            const uint32_t queueEmptyPermille =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcQueueEmptyTickPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickAvgPermille =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcBufferedAtTickAvgPermille.load(std::memory_order_relaxed)
                             : 0u;
            const uint32_t bufferedAtTickMin =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcBufferedAtTickMin.load(std::memory_order_relaxed) : 0u;
            const uint32_t starvedTicks =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcStarvedTickCount.load(std::memory_order_relaxed) : 0u;
            const uint32_t singleFrameTicks =
                g_pSharedMem ? g_pSharedMem->runtimeState.wgcSingleFrameTickCount.load(std::memory_order_relaxed) : 0u;
            if (g_pSharedMem) {
                uint32_t currentDup = g_pSharedMem->runtimeState.duplicateFrames.load(std::memory_order_relaxed);
                uint32_t currentLate = g_pSharedMem->runtimeState.lateFrames.load(std::memory_order_relaxed);
                dupDelta = currentDup - lastDuplicateCount;
                lateDelta = currentLate - lastLateCount;
                lastDuplicateCount = currentDup;
                lastLateCount = currentLate;
                overloadFlags = g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                muxQueueBytes = g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                encoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
                cadenceSelAvgUs = g_pSharedMem->runtimeState.selectionErrorAvgUs.load(std::memory_order_relaxed);
                cadenceSelBiasUs = g_pSharedMem->runtimeState.selectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                wgcSelAvgUs = g_pSharedMem->runtimeState.wgcSelectionErrorAvgUs.load(std::memory_order_relaxed);
                wgcSelBiasUs = g_pSharedMem->runtimeState.wgcSelectionErrorSignedAvgUs.load(std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameIntervalAvgUs.store(SaturatingToUint32(srcIntervalAvgUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameJitterAvgUs.store(SaturatingToUint32(srcJitterAvgUs),
                                                                           std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceFrameJitterMaxUs.store(SaturatingToUint32(srcJitterMaxUs),
                                                                           std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceToCopyLatencyAvgUs.store(SaturatingToUint32(srcToCopyAvgUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcSourceToCopyLatencyMaxUs.store(SaturatingToUint32(srcToCopyMaxUs),
                                                                             std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcTargetFps.store(throttleTargetFps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredFramesPerSec.store(deliveredRatePerSec,
                                                                          std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredMin250Fps.store(deliveredMin250Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcDeliveredMin500Fps.store(deliveredMin500Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcInputMin250Fps.store(inputMin250Fps, std::memory_order_relaxed);
                g_pSharedMem->runtimeState.wgcInputMin500Fps.store(inputMin500Fps, std::memory_order_relaxed);
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
                "DupPtrTransitions: %llu | TimingBasis: Copy/Convert/Encode/Fence=CPU-wall-or-submit",
                inputFrames, queuedFrames, hostDropDelta, pacingSkipDelta, throttleSkipDelta, staleSkipDelta,
                staleDuplicateTsDelta, staleOutOfOrderTsDelta, normalizedDuplicateTsDelta, duplicateTsSkipDelta,
                cursorSkipDelta, poolDropDelta, ingressDecimatedDelta, static_cast<uint32_t>(g_FrameQueue.Size()),
                encoderQueueDepth, dupDelta, lateDelta, srcIntervalAvgUs, srcJitterAvgUs, srcJitterMaxUs,
                srcToCopyAvgUs, srcToCopyMaxUs, deliveredRatePerSec, inputMin250Fps, inputMin500Fps, deliveredMin250Fps,
                deliveredMin500Fps, queueEmptyPermille, bufferedAtTickAvgPermille, bufferedAtTickMin, starvedTicks,
                singleFrameTicks, cadenceSelAvgUs, cadenceSelBiasUs, wgcSelAvgUs, wgcSelBiasUs, callbackGapAvgUs,
                callbackGapMaxUs, callbackProcessAvgUs, callbackProcessMaxUs, callbackDrainMax, copyUs,
                poolSlotRewriteUs, poolSlotFastRewriteDelta, g_WgcCap->GetPoolSlotLeasedMaxCount(),
                g_WgcCap->GetPoolSlotFreeMinCount(), poolSaturatedDropDelta, poolOverwritePreventedDelta,
                g_WgcCap->GetPoolLeaseMismatchCount(), g_WgcCap->GetSourceFramePoolBufferCount(),
                g_WgcCap->GetTexturePoolSlotCount(), g_WgcCap->GetSmoothnessBudgetSurfaceCount(),
                g_WgcCap->GetSmoothnessSyncFrameCount(), g_WgcCap->GetSmoothnessRetainedFrameCount(),
                g_WgcCap->GetSmoothnessRetainedFrameCap(), g_WgcCap->GetSmoothnessReservedFreeSlotCount(),
                g_WgcCap->GetSmoothnessSafetySlotCount(), g_WgcCap->GetSmoothnessSourceDxgiFormat(),
                g_WgcCap->GetSmoothnessCopyDxgiFormat(), g_WgcCap->IsCompactRetainedCopyActive() ? 1 : 0,
                static_cast<double>(g_WgcCap->GetSmoothnessSourceEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessCopyEstimatedVramBytes()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessSourceBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<double>(g_WgcCap->GetSmoothnessCopyBytesPerSurface()) / (1024.0 * 1024.0),
                static_cast<long long>(g_WgcCap->GetLastPoolConvertTimeUs()), ingressAcceptedDelta,
                ingressDecimatedDelta, g_WgcCap->GetIngressRetainedFrameCount(), g_WgcCap->GetIngressRetainedFrameCap(),
                g_WgcCap->GetIngressLowWaterFrameCount(),
                WgcIngressAdmissionReasonName(g_WgcCap->GetIngressAdmissionReasonCode()), ingressAcceptedLowWaterDelta,
                ingressAcceptedRecoveryDelta, ingressAcceptedSourceBelowDelta, ingressAcceptedHealthyDelta,
                ingressAcceptedUniformPlayoutSoftReserveDelta, ingressAcceptedUniformPlayoutCreditDelta,
                ingressDecimatedSoftReserveDelta, ingressDecimatedHardReserveDelta, ingressDecimatedCreditDelta,
                ingressSoftReservePressureDelta, ingressHardReservePressureDelta, keyedAcquireFailDelta,
                keyedReleaseFailDelta, keyedAbandonedReclaimDelta, splitFlushDelta, splitFlushSkippedDelta,
                g_WgcCap->IsUsingDedicatedCaptureDevice() ? 1 : 0, encodeUs, fenceUs, throttleTargetFps,
                (muxQueueBytes + 1023u) / 1024u, overloadFlags,
                g_WgcCap->IsUsingDesktopDuplication() ? "DxgiDuplication" : "WGC",
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationAcquireTimeoutCount()),
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationAccumulatedMissedFrameCount()),
                g_WgcCap->IsDuplicationSeparatePointerVisible() ? 1 : 0,
                g_WgcCap->IsDuplicationCursorEmbedded() ? 1 : 0,
                static_cast<unsigned long long>(g_WgcCap->GetDuplicationPointerStateTransitionCount()));

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

    g_WgcCaptureRunning = false;
    LogInfo("[WGC CaptureThread] Stopped");
}

void EncoderThreadFunc(const AppConfig& config) {
    LogInfo("[EncoderThread] Started");

    g_WgcCursorTimeline.Clear();
    g_InjectCursorTimeline.Clear();

    DisableCurrentThreadPowerThrottling("EncoderThread");
    ScopedMmcssTask encoderMmcssTask(L"Pro Audio", AVRT_PRIORITY_HIGH, "EncoderThread");

    g_FrameQueue.StartRecording();
    if (!TryArmCapturePipelineWarmup()) {
        const uint32_t phase = g_pSharedMem
                                   ? g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire)
                                   : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
        LogInfo("[RecordingLifecycle] Encoder warmup cancelled before arm (phase=%s requested=%d)",
                CapturePipelinePhaseToString(phase), g_Recording.load(std::memory_order_acquire) ? 1 : 0);
        return;
    }

    LARGE_INTEGER qpcFreq;
    QueryPerformanceFrequency(&qpcFreq);
    ce::screen_grab_privacy::ScreenGrabPrivacyRuntime privacyRuntime;
    privacyRuntime.Reset(config.blackWhenNoFullscreenFocus);
    int64_t targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
    const uint32_t captureSyncMultiplier =
        static_cast<uint32_t>(std::clamp(config.fpsLimiter.captureSyncMultiplier, 1, 8));
    const bool captureSyncPhaseLockEnabled =
        config.fpsLimiter.captureSyncEnabled && !config.video.useVFR && targetIntervalTicks > 0;
    const int64_t captureSyncSourceIntervalTicks = ce::capture_policy::GetCfrCaptureSyncSourceIntervalQpc(
        targetIntervalTicks, captureSyncMultiplier);
    LARGE_INTEGER nextSampleTime;
    QueryPerformanceCounter(&nextSampleTime);

    HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer) {
        hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
        LogInfo("[EncoderThread] Using standard waitable timer");
    } else {
        LogInfo("[EncoderThread] Using high-resolution waitable timer");
    }

    double smoothedEncodeMs = 0.0;
    double smoothedWgcFreshServiceMs = 0.0;
    double smoothedWgcRepeatServiceMs = 0.0;
    uint32_t wgcFreshServiceSamples = 0;
    uint32_t wgcRepeatServiceSamples = 0;
    ce::capture_policy::WgcOverloadRepeatPacerState wgcOverloadRepeatPacer;
    double frameIntervalMs = 1000.0 / config.video.fps;
    uint64_t encoderWakeLateAccumUs = 0;
    uint64_t encoderWakeLateSamples = 0;
    uint32_t encoderWakeLateMaxUs = 0;
    auto ReleaseQueuedFrameTexture = [](QueuedFrame& queuedFrame) {
        if (!queuedFrame.isInjectMode && queuedFrame.texture) {
            queuedFrame.texture->Release();
            queuedFrame.texture = nullptr;
        }
        if (!queuedFrame.isInjectMode) {
            queuedFrame.wgcPoolLease.Reset();
            queuedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
            queuedFrame.wgcPoolGeneration = 0;
        }
    };
    auto DiscardQueuedFrame = [&](QueuedFrame& queuedFrame) {
        if (!queuedFrame.isInjectMode) {
            ReleaseQueuedFrameTexture(queuedFrame);
        }
        queuedFrame = QueuedFrame{};
    };
    std::vector<QueuedFrame> drainedScreenGrabFrames;
    drainedScreenGrabFrames.reserve(8);
    std::vector<WGCCapturedFrame> drainedWgcCapturedFrames;
    drainedWgcCapturedFrames.reserve(8);
    std::deque<QueuedFrame> bufferedWgcFrames;
    uint64_t observedWgcSourceEpoch = g_WgcSourceEpoch.load(std::memory_order_acquire);
    bool lastSuccessfulWgcCursorEmbedded = false;
    bool hasSuccessfulWgcCursorMetadata = false;
    std::vector<size_t> wgcFreshCandidateIndices;
    wgcFreshCandidateIndices.reserve(64);
    std::vector<size_t> wgcFallbackCandidateIndices;
    wgcFallbackCandidateIndices.reserve(64);
    std::vector<size_t> wgcRelaxedFreshCandidateIndices;
    wgcRelaxedFreshCandidateIndices.reserve(64);
    std::vector<size_t> wgcRelaxedFallbackCandidateIndices;
    wgcRelaxedFallbackCandidateIndices.reserve(64);
    std::vector<size_t> wgcRepeatRescueCandidateIndices;
    wgcRepeatRescueCandidateIndices.reserve(64);
    std::vector<QueuedFrame> drainedInjectFrames;
    drainedInjectFrames.reserve(8);
    std::deque<QueuedFrame> bufferedInjectFrames;
    double smoothedInjectFenceMs = 0.0;
    bool recordingOutputLive = false;
    bool pendingLiveActivation = false;
    uint64_t startupWarmupStartTick = GetTickCount64();
    uint64_t recordingLiveTick = 0;
    uint32_t hiddenStartupFrames = 0;
    ce::capture_policy::WarmupTransitionState warmupState = {
        IsActiveScreenGrab(),
        GetTickCount64(),
        0,
    };
    uint32_t pendingInjectTrimmedLogCount = 0;
    size_t maxBufferedInjectDepthSinceLog = 0;
    DWORD lastInjectTrimLog = GetTickCount();
    // Source-rate EMA is telemetry/recovery context only. Live CFR source choice is timestamp-driven.
    uint32_t pacingInputThisWindow = 0;
    uint32_t pacingTicksThisWindow = 0;
    uint32_t pacingEmaUpdates = 0;
    double smoothedInputPerTick = 1.0;    // EMA: avg unique frames per encoder tick
    double frameCreditAccumulator = 0.0;  // Bresenham error term
    // Output-grid tracking for timestamp-aware frame selection.
    // When multiple buffered frames are available (game fps > target fps),
    // selecting the frame closest to the ideal output grid time produces
    // the smoothest motion in the CFR output.
    int64_t encoderGridStartQpc = 0;
    int64_t encoderGridTickCount = 0;
    uint64_t selectionLogCounter = 0;
    uint32_t lastEncodedInjectFrameIndex = 0;
    std::array<uint32_t, kInjectTextureSlotCount> lastEncodedFrameByTextureIndex{};
    InjectFrameLineage lastDeferredLineage;
    InjectFrameLineage lastSuccessfullyEncodedInjectLineage;
    CadenceHealthCounters cadenceCounters;
    InputFrameRatePredictor wgcInputPredictor;
    InputFrameRatePredictor injectInputPredictor;
    ce::capture_policy::CfrCadencePhaseLockState injectCfrPhaseLock;
    ce::capture_policy::CfrCadencePhaseLockState wgcCfrPhaseLock;
    uint32_t injectWorstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
    uint32_t injectBestSourceFpsX100 = 0;
    uint32_t injectWorstSourceJitterUs = 0;
    uint32_t injectWorstSelectionErrorUs = 0;
    double smoothedEncCycleMs = 0.0;
    double smoothedInjectServiceMs = 0.0;
    uint32_t injectServiceMaxUs = 0;
    uint32_t encCycleMaxMs = 0;
    uint32_t encodeSpikeCountThisSecond = 0;
    uint32_t dupTimestampCount = 0;
    uint32_t lastWgcDuplicateTimestampSkipCountForCadence = 0;
    uint32_t lastDuplicateReasonNoSource = 0;
    uint32_t lastDuplicateReasonDeferred = 0;
    uint32_t lastDuplicateReasonTimerRebase = 0;
    uint32_t lastDuplicateReasonDrain = 0;
    uint32_t lastInvalidMetaCount = 0;
    uint32_t lastInvalidHandleCount = 0;
    uint32_t lastTimestampRegressionCount = 0;
    uint32_t lastTimestampStallCount = 0;
    uint32_t lastPacketClampCount = 0;
    uint32_t lastNegativePtsCount = 0;
    uint32_t lastNonMonotonicPtsCount = 0;
    uint32_t injectDeferredRequeuedThisWindow = 0;
    uint32_t injectDeferredDroppedThisWindow = 0;
    uint64_t injectDeferredRequeuedTotal = 0;
    uint64_t injectDeferredDroppedTotal = 0;
    uint32_t injectFreshCatchupThisWindow = 0;
    uint32_t injectRepeatCatchupThisWindow = 0;
    uint32_t injectLiveStaleTrimThisWindow = 0;
    uint32_t injectTargetSelectThisWindow = 0;
    uint32_t injectTargetSupersededThisWindow = 0;
    uint32_t injectTargetHoldThisWindow = 0;
    uint32_t injectTargetHoldWithCandidateThisWindow = 0;
    uint32_t activePathMismatchDiscardThisWindow = 0;
    uint64_t injectFreshCatchupTotal = 0;
    uint64_t injectRepeatCatchupTotal = 0;
    uint64_t injectLiveStaleTrimTotal = 0;
    uint64_t injectTargetSelectTotal = 0;
    uint64_t injectTargetSupersededTotal = 0;
    uint64_t injectTargetHoldTotal = 0;
    uint64_t injectTargetHoldWithCandidateTotal = 0;
    uint64_t injectBufferCapTrimTotal = 0;
    uint32_t injectTargetResidualMaxUs = 0;
    bool injectCfrRecoveryActive = false;
    bool injectEncoderServiceTooSlowCurrent = false;
    uint32_t injectCfrRecoveryEpisodesThisWindow = 0;
    uint64_t injectCfrRecoveryEpisodesTotal = 0;
    uint64_t injectCfrRecoveryStartTick = 0;
    uint32_t injectCfrRecoveryStartDebt = 0;
    uint32_t injectCfrRecoveryBestDebt = 0;
    uint64_t injectCfrRecoveryStartFreshCatchup = 0;
    uint64_t injectCfrRecoveryStartRepeatCatchup = 0;
    uint64_t injectCfrRecoveryLastProgressLogTick = 0;
    uint64_t activePathMismatchDiscardTotal = 0;
    size_t pendingLiveInjectReadyFrames = 0;
    DWORD lastHealthLog = GetTickCount();
    LARGE_INTEGER liveStartQpc = {};
    uint64_t liveTicksOutput = 0;
    uint64_t liveTicksScheduled = 0;
    uint64_t liveTicksDiscardedByTimerRebase = 0;
    uint64_t wgcVisualDebtMaxExcessTicks = 0;
    uint64_t wgcLiveSchedulerRebaseTotal = 0;
    uint32_t wgcLiveSchedulerRebaseMaxTicks = 0;
    uint32_t wgcLiveSchedulerRebaseThisWindow = 0;
    bool wgcStopDrainHeldFrameLogged = false;
    uint64_t wgcSelectionErrorAccumUs = 0;
    int64_t wgcSelectionErrorSignedAccumUs = 0;
    uint32_t wgcSelectionErrorSamples = 0;
    uint32_t wgcSelectionErrorMaxUs = 0;
    uint32_t wgcSelectionEarlyMaxUs = 0;
    uint32_t wgcSelectionLateMaxUs = 0;
    uint32_t wgcSelectionTargetClampCount = 0;
    uint32_t wgcSelectionTargetClampMaxUs = 0;
    uint32_t wgcHoldForNextTickCount = 0;
    uint32_t wgcSelectionDelayTickCount = 0;
    uint32_t wgcSyncDelayHoldCount = 0;
    uint64_t wgcSyncDelayHoldTotal = 0;
    uint32_t wgcSyncDelaySourceLimitedHoldCount = 0;
    uint64_t wgcSyncDelaySourceLimitedHoldTotal = 0;
    uint32_t wgcSyncDelayPolicyHoldCount = 0;
    uint64_t wgcSyncDelayPolicyHoldTotal = 0;
    uint32_t wgcTooNewLeadMaxUs = 0;
    uint32_t wgcTooNewLeadSessionMaxUs = 0;
    uint32_t wgcStartupReserveFrames = 0;
    int64_t wgcStartupReserveSpanUs = 0;
    int64_t wgcStartupDelayTargetUs = 0;
    bool wgcStartupSelectedByDelayReserve = false;
    std::string wgcStartupReserveReason = "not-run";
    int64_t wgcSmoothnessActiveDelayQpc = 0;
    ce::capture_policy::CfrTimelineStartContract pendingWgcStartContract{};
    uint64_t pendingWgcStartContractGeneration = 0;
    uint64_t committedWgcStartContractGeneration = 0;
    bool wgcEncoderPrewarmAttempted = false;
    bool wgcEncoderPrewarmSucceeded = false;
    int64_t wgcEncoderPrewarmElapsedUs = 0;
    // Smoothness FLOOR diagnostics/state (resolved once at startup, then fixed for the session).
    int64_t wgcSmoothnessFloorDelayQpc = 0;            // resolved floor delay target (QPC); 0 = floor inactive
    int64_t wgcSmoothnessFloorRequestedQpc = 0;        // pre-clamp requested floor (QPC), for logging
    const char* wgcSmoothnessFloorSource = "off";      // off | auto | config
    const char* wgcSmoothnessFloorClampedBy = "none";  // none | min | max_ms | reservoir
    bool wgcSmoothnessFloorLogged = false;  // latch: the one-time floor decision log fires once per recording
    ce::capture_policy::WgcSmoothnessFloorJitter wgcSmoothnessFloorJitter{};  // measured jitter used for auto
    uint32_t wgcSmoothnessDesiredFrames = 0;
    uint32_t wgcSmoothnessRetainedFrames = 0;
    uint32_t wgcSmoothnessActualFrames = 0;
    uint32_t wgcSmoothnessPoolSlots = 0;
    uint32_t wgcSmoothnessRetainedFrameCap = 0;
    uint32_t wgcSmoothnessReservedFreeSlots = 0;
    uint64_t wgcSmoothnessEstimatedVramBytes = 0;
    bool wgcSmoothnessCapLimited = false;
    std::string wgcSmoothnessBufferReason = "not-run";
    uint32_t wgcDelayReservoirLowWaterTickCount = 0;
    uint64_t wgcDelayReservoirLowWaterTickTotal = 0;
    // WGC selection-timestamp smoothing telemetry (monotonic bounded-deviation
    // smoother over raw compositor timestamps; see InputFrameRatePredictor::
    // SmoothMonotonicTimestamp). Deviation = |selection - raw normalized|.
    uint64_t wgcTsSmoothSamplesWindow = 0;
    uint64_t wgcTsSmoothDevAccumUsWindow = 0;
    uint32_t wgcTsSmoothDevMaxUsWindow = 0;
    uint32_t wgcTsSmoothDevMaxUsTotal = 0;
    uint32_t wgcTsSmoothSnapCountWindow = 0;
    uint64_t wgcTsSmoothSnapCountTotal = 0;
    uint64_t wgcDelayResidualSamples = 0;
    uint64_t wgcDelayResidualAbsAccumUs = 0;
    int64_t wgcDelayResidualSignedAccumUs = 0;
    uint32_t wgcDelayResidualAbsMaxUs = 0;
    uint32_t wgcDelayResidualLateMaxUs = 0;
    uint32_t wgcDelayResidualEarlyMaxUs = 0;
    uint64_t wgcDelayRealizedAccumUs = 0;
    uint32_t wgcDelayRealizedMinUs = UINT32_MAX;
    uint32_t wgcDelayRealizedMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayResidualAbsHistogram{};
    uint64_t wgcDelayResidualWindowSamples = 0;
    uint64_t wgcDelayResidualWindowAbsAccumUs = 0;
    int64_t wgcDelayResidualWindowSignedAccumUs = 0;
    uint32_t wgcDelayResidualWindowAbsMaxUs = 0;
    uint32_t wgcDelayResidualWindowLateMaxUs = 0;
