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
                g_WgcCap ? g_WgcCap->GetProducerTargetFps() : 0u,
                static_cast<unsigned long long>(wgcProducerRateRetuneTotal), config.wgcSmoothnessBufferEnabled ? 1 : 0,
                config.wgcSmoothnessBufferMaxMs, wgcSmoothnessActualFrames, wgcSmoothnessRetainedFrames,
                wgcSmoothnessDesiredFrames, static_cast<double>(wgcSummarySmoothActualDelayUs) / 1000.0,
                wgcSmoothnessPoolSlots, wgcSummarySourceFramePoolBuffers, wgcSummaryBudgetSurfaces,
                wgcSummarySyncFrames, wgcSummaryExtraFrames, wgcSummaryRetainedCap, wgcSummaryReservedFreeSlots,
                wgcSummarySafetySlots, static_cast<unsigned long long>(wgcRetainedCapTrimTotal),
                g_WgcCap ? g_WgcCap->GetIngressAcceptedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressLowWaterFrameCount() : 0u, wgcSummaryPoolLeasedMax,
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
            const uint32_t wgcSummaryIngressHard = g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressSoft = g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
            const uint32_t wgcSummaryIngressDecimated = g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u;
            const uint32_t wgcSummaryOverloadFlags =
                g_pSharedMem ? g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
            const uint32_t wgcSummaryMuxBackpressure =
                g_pSharedMem ? g_pSharedMem->runtimeState.muxBackpressureCount.load(std::memory_order_relaxed) : 0u;
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
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u, wgcSummaryOverwritePrevented,
                static_cast<unsigned long long>(wgcDelaySyncProtectedRepeatTotal),
                static_cast<unsigned long long>(wgcPolicyAddedRepeatTotal),
                static_cast<unsigned long long>(wgcCombinedExcessRepeatTotal),
                static_cast<long long>(wgcSummarySmoothDelayDeficitUs),
                static_cast<long long>(wgcSummaryStartupDelayDeficitUs), wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped, wgcSummaryOverloadFlags, wgcSummaryMuxBackpressure,
                wgcSummaryCompactRetained, wgcSummarySourceFormat, wgcSummaryRetainedFormat,
                static_cast<long long>(wgcSummaryConvertUs),
                g_WgcCap && g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc");
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
                g_WgcCap ? g_WgcCap->GetIngressAcceptedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressLowWaterFrameCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedLowWaterCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedRecoveryCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedSourceBelowCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedHealthyCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressAcceptedUniformPlayoutCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedSoftReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedHardReserveCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressDecimatedCreditCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u,
                g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u, wgcSummaryDuplicateTimestampsSeen,
                wgcSummaryDuplicateTimestampsSkipped,
                g_WgcCap ? WgcIngressAdmissionReasonName(g_WgcCap->GetIngressAdmissionReasonCode()) : "none");
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
                static_cast<unsigned long long>(g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
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
                "[Inject CFR QUALITY SUMMARY] TargetSelect=%llu Superseded=%llu TargetHold=%llu "
                "HoldWithCandidate=%llu BufferCapTrim=%llu TargetResidualMax=%uus",
                static_cast<unsigned long long>(injectTargetSelectTotal),
                static_cast<unsigned long long>(injectTargetSupersededTotal),
                static_cast<unsigned long long>(injectTargetHoldTotal),
                static_cast<unsigned long long>(injectTargetHoldWithCandidateTotal),
                static_cast<unsigned long long>(injectBufferCapTrimTotal), injectTargetResidualMaxUs);
            if (g_pSharedMem) {
                const auto& contention = g_pSharedMem->runtimeState;
                LogInfo(
                    "[Inject Contention SUMMARY] CaptureLock=%u CpuLease=%u GpuBusy=%u RingFull=%u "
                    "EventSignals=%u",
                    contention.injectProducerCaptureLockDrops.load(std::memory_order_relaxed),
                    contention.injectProducerCpuLeaseBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerGpuBusyDrops.load(std::memory_order_relaxed),
                    contention.injectProducerMetadataFullDrops.load(std::memory_order_relaxed),
                    contention.injectFrameReadySignals.load(std::memory_order_relaxed));
            }
        }
        const auto& phaseLockSummary = summaryUsesScreenGrab ? wgcCfrPhaseLock : injectCfrPhaseLock;
        const char* phaseLockBackend = summaryUsesScreenGrab
                                           ? (g_WgcCap && g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc")
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

bool StartRecording(const AppConfig& config) {
    if (g_Recording)
        return true;

    g_PrivacyFailClosedStopRequested.store(false, std::memory_order_release);
    ResetRecordingHealthPublication();
    LogInfo("[Media] Starting recording...");

    timeBeginPeriod(1);

    // A prior interrupted session must never leave hook-side video publication
    // armed. The selected live path below explicitly enables it only for inject.
    SetInjectVideoCaptureRequestedState(false, "recording start reset");

    if (g_AudioOnly) {
        LogInfo("[Media] Audio-only recording mode - skipping video capture");

        // Clear any stale shared memory state
        if (g_pSharedMem) {
            StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
            StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
            StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::None));
            SetRecordingVisibleState(false);
        }

        if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
            LogError("[Media] Failed to start MediaEngine audio-only recording");
            SetRecordingVisibleState(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                         "MediaEngine audio-only initialization");
            timeEndPeriod(1);
            g_AudioOnly = false;
            return false;
        }

        g_Recording = true;
        SetRecordingVisibleState(true);
        SetCapturePipelinePhase(CapturePipelinePhase::kLive);

        LogInfo("[Media] Audio-only recording active");
        return true;
    }

    if (IsVideoCaptureDisabledMethod(config.captureMethod)) {
        LogError("[Media] Video recording is disabled by the active application profile");
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                     "active profile has video_capture=none");
        timeEndPeriod(1);
        return false;
    }

    bool useScreenGrab = IsPreferredScreenGrab();
    if (IsScreenGrabCaptureMethod(config.captureMethod)) {
        useScreenGrab = true;
    } else if (IsInjectCaptureMethod(config.captureMethod)) {
        useScreenGrab = false;
    }
    SetActiveScreenGrab(useScreenGrab);

    if (useScreenGrab && !g_WgcCap) {
        LogError("[Media] WGC capture requested but no WGC target is available");
        SetActiveScreenGrab(false);
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC target unavailable");
        timeEndPeriod(1);
        return false;
    }

    if (!useScreenGrab && g_pSharedMem) {
        const uint32_t writeIndex = g_pSharedMem->frameRing.writeIndex.load(std::memory_order_acquire);
        const uint32_t readIndex = g_pSharedMem->frameRing.readIndex.load(std::memory_order_acquire);
        if (!IsFrameRingWindowValid(writeIndex, readIndex)) {
            LogError(
                "[Media] Inject recording rejected corrupt shared frame ring "
                "(write=%u read=%u distance=%u version=%u ABI=0x%08X)",
                writeIndex, readIndex, static_cast<uint32_t>(writeIndex - readIndex),
                g_pSharedMem->GetVersion(), g_pSharedMem->abiSignature.load(std::memory_order_acquire));
            StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                         static_cast<uint32_t>(RecordingFailureCode::SharedMemoryProtocolIntegrity));
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            PublishRecordingStartFailure(RecordingFailureCode::SharedMemoryProtocolIntegrity,
                                         "corrupt shared frame ring");
            timeEndPeriod(1);
            return false;
        }
    }

    // Clear any stale shared memory commands/state from previous (possibly crashed)
    // recording sessions. If a previous media process crashed, cmdStopRecording
    // may still be true, causing the new recording to stop immediately.
    if (g_pSharedMem) {
        StoreRelease(g_pSharedMem->runtimeState.cmdStartRecording, false);
        StoreRelease(g_pSharedMem->runtimeState.cmdStopRecording, false);
        StoreRelease(g_pSharedMem->runtimeState.recordingFailureCode,
                     static_cast<uint32_t>(RecordingFailureCode::None));
        SetRecordingVisibleState(false);
    }

    // Reset inject session state so main loop re-initializes on new recording
    g_InjectSessionReset.store(true, std::memory_order_release);

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    ResetInjectFrameRingToLatest("recording start");

    g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_IsEncoderBottlenecked.store(false, std::memory_order_relaxed);
    g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
    g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
    g_InjectDeferredFrames.store(0, std::memory_order_relaxed);
    g_ActivePathMismatchFramesDiscarded.store(0, std::memory_order_relaxed);

    if (g_pSharedMem) {
        ResetRuntimeDiagnostics(g_pSharedMem);
        g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
        g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
    }

    EnsureInjectCaptureEvents();
    if (g_InjectCaptureShutdownEvent) {
        ResetEvent(g_InjectCaptureShutdownEvent);
    }
    if (g_InjectFrameReadyEvent) {
        ResetEvent(g_InjectFrameReadyEvent);
    }

    SetInjectVideoCaptureRequestedState(!useScreenGrab,
                                        useScreenGrab ? "screen-grab recording path" : "inject recording path");
    SetCaptureRequestedState(true);

    if (!MediaEngine_StartRecording || !MediaEngine_StartRecording()) {
        LogError("[Media] Failed to start MediaEngine recording");
        SetInjectVideoCaptureRequestedState(false, "recording start failure");
        SetCaptureRequestedState(false);
        SetRecordingVisibleState(false);
        PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "MediaEngine initialization");
        timeEndPeriod(1);
        return false;
    }

    if (useScreenGrab) {
        // Last point before any frame of this recording can be captured. Screen-grab
        // capture records the composited desktop, so CE's own recording-start status has
        // to be off screen now: the encoder thread's warmup reservoir is captured from
        // here on and is handed to the live output intact.
        RequestStatusOverlayDarkForCapture("screen-grab capture start");
    }

    g_Recording = true;
    g_EncoderRunning = true;
    g_RecordingUsesVfr.store(config.video.useVFR, std::memory_order_release);
    g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
    g_CfrDrainStopQpc.store(0, std::memory_order_release);

    // Recording-lifetime config snapshot (see StartWgcRecordingCapture): the
    // main thread may reassign `config` mid-recording; encoder-thread settings
    // are fixed per session by design, so it reads an owned copy.
    {
        auto configSnapshot = std::make_shared<const AppConfig>(config);
        g_EncoderThread = std::thread([configSnapshot]() { EncoderThreadFunc(*configSnapshot); });
    }

    if (useScreenGrab && g_WgcCap) {
        if (!StartWgcRecordingCapture(config)) {
            LogError("[Media] Failed to start WGC capture");
            g_EncoderRunning = false;
            JoinThreadWithTimeout(g_EncoderThread, 10000, "encoder");
            g_Recording = false;
            SetCaptureRequestedState(false);
            SetRecordingVisibleState(false);
            MediaEngine_StopRecording(true);
            SetActiveScreenGrab(false);
            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed, "WGC capture initialization");
            timeEndPeriod(1);
            return false;
        }
        LogInfo("[Media] Active recording path: %s bounded pull-drain CFR (%d fps output)",
                g_WgcCap->IsUsingDesktopDuplication() ? "DXGI-duplication" : "WGC", config.video.fps);
    } else if (!useScreenGrab) {
        if (config.captureMethod == "auto" && g_WgcCap && g_AutoWgcFallbackArmed.load(std::memory_order_acquire)) {
            LogInfo("[Media] Active recording path: inject shared-memory capture (WGC auto-fallback armed)");
        } else {
            LogInfo("[Media] Active recording path: inject shared-memory capture");
        }
        ApplyMediaGpuSchedulingPriorityForSharedAdapter(config);
        g_InjectCaptureShutdown = false;
        EnsureInjectCaptureEvents();
        // Recording-lifetime config snapshot (see StartWgcRecordingCapture).
        {
            auto configSnapshot = std::make_shared<const AppConfig>(config);
            g_InjectCaptureThread = std::thread([configSnapshot]() { InjectCaptureThreadFunc(*configSnapshot); });
        }
    }

    LogInfo("[Media] Recording warmup armed");
    return true;
}

void StopRecording() {
    if (g_pSharedMem) {
        g_pSharedMem->runtimeState.SetRecordingStartIntent(RecordingStartIntent::Idle);
    }
    if (!g_Recording)
        return;

    LogInfo("[Media] Stopping recording...");

    SetInjectVideoCaptureRequestedState(false, "recording stop");

    // Audio-only: skip all video/capture cleanup
    if (g_AudioOnly) {
        g_Recording = false;
        SetRecordingVisibleState(false);
        SetCapturePipelinePhase(CapturePipelinePhase::kStopping);

        const bool outputSaved = MediaEngine_StopRecording(false);
        CompleteRecordingFinalization(false, outputSaved);

        if (g_pSharedMem) {
            ResetRuntimeDiagnostics(g_pSharedMem);
            g_pSharedMem->encoderQueueDepth.store(0, std::memory_order_relaxed);
            g_pSharedMem->throttleCapture.store(false, std::memory_order_release);
        }

        SetActiveScreenGrab(false);
        g_AudioOnly = false;
        timeEndPeriod(1);
        LogInfo("[Media] Audio-only recording stopped");
        return;
    }

    const bool wasActiveScreenGrab = IsActiveScreenGrab();
    const bool recordingUsesVfr = g_RecordingUsesVfr.load(std::memory_order_acquire);
    g_Recording = false;
    const CapturePipelinePhase stopTransition = BeginCapturePipelineStop();
    const bool cancelBeforeLive = stopTransition == CapturePipelinePhase::kCancelling;
    const bool drainOutstandingCfrTicks =
        !cancelBeforeLive &&
        ce::capture_policy::ShouldDrainOutstandingCfrTicksAtStop(wasActiveScreenGrab, recordingUsesVfr);
    LogInfo("[RecordingLifecycle] Stop accepted as %s (path=%s liveFrames=%u)",
            cancelBeforeLive ? "pre-live cancellation" : "live finalization",
            wasActiveScreenGrab ? "WGC" : "inject",
            g_pSharedMem ? g_pSharedMem->runtimeState.liveFramesEncoded.load(std::memory_order_acquire) : 0u);
    int64_t drainStopQpc = 0;
    if (drainOutstandingCfrTicks) {
        LARGE_INTEGER stopQpc;
        QueryPerformanceCounter(&stopQpc);
        drainStopQpc = stopQpc.QuadPart;
    }

    SetCaptureRequestedState(false);
    SetRecordingVisibleState(false);
    g_CfrDrainStopQpc.store(drainStopQpc, std::memory_order_release);
    g_DrainOutstandingCfrTicks.store(drainOutstandingCfrTicks && drainStopQpc > 0, std::memory_order_release);
    if (wasActiveScreenGrab) {
        g_EncoderRunning = false;
    }
    if (drainOutstandingCfrTicks && drainStopQpc > 0) {
        LogInfo("[Media] CFR stop drain armed at QPC=%lld path=%s", drainStopQpc,
                IsActiveScreenGrab() ? "WGC" : "inject");
    } else if (wasActiveScreenGrab && recordingUsesVfr) {
        LogInfo("[Media] WGC VFR exact-stop: no CFR debt to drain");
    }

    StopWgcCapturePipeline();
    StopInjectCapturePipeline();
    if (!wasActiveScreenGrab) {
        g_EncoderRunning = false;
    }

    g_InjectDeliveredFirstFrame.store(false, std::memory_order_release);
    g_RejectInjectFrames.store(false, std::memory_order_release);
    g_AutoWgcFallbackArmed.store(false, std::memory_order_release);

    const bool encoderJoined = JoinThreadWithTimeout(g_EncoderThread, 60000, "encoder");

    if (wasActiveScreenGrab && !encoderJoined) {
        LogWarn("[Media] WGC encoder join timed out after exact-stop shutdown");
    }

    g_FrameQueue.Clear();
    ResetLastQueuedFrameCache();
    ResetInjectFrameRingToLatest("recording stop");

    if (g_pSharedMem) {
        // Recording has stopped, so zero-copy encoder textures must not stay
        // live. Clear the handshake before stopping the encoder so the DLL
        // tears down all preserved D3D11/KMT resources immediately.
        g_pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        g_pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        g_pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
