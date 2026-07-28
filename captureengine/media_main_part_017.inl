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
                    const int64_t jbDeliveryGapAvgUs = g_WgcCap ? g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t jbDeliveryGapMaxUs = g_WgcCap ? g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t jbSourceJitterAvgUs = g_WgcCap ? g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t jbSourceJitterMaxUs = g_WgcCap ? g_WgcCap->GetSourceJitterMaxUs() : 0;
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
                const bool encoderPressure = g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
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
                    const int64_t wgcInfoCallbackGapAvgUs = g_WgcCap ? g_WgcCap->GetCallbackGapAvgUs() : 0;
                    const int64_t wgcInfoCallbackGapMaxUs = g_WgcCap ? g_WgcCap->GetCallbackGapMaxUs() : 0;
                    const int64_t wgcInfoSourceJitterAvgUs = g_WgcCap ? g_WgcCap->GetSourceJitterAvgUs() : 0;
                    const int64_t wgcInfoSourceJitterMaxUs = g_WgcCap ? g_WgcCap->GetSourceJitterMaxUs() : 0;
                    const uint32_t wgcInfoPoolFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
                    const uint32_t wgcInfoPoolSaturatedDrops = g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u;
                    const uint32_t wgcInfoIngressHard = g_WgcCap ? g_WgcCap->GetIngressHardReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressSoft = g_WgcCap ? g_WgcCap->GetIngressSoftReservePressureCount() : 0u;
                    const uint32_t wgcInfoIngressDecimated = g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u;
                    const bool wgcInfoPoolPressure = wgcInfoPoolSaturatedDrops > 0 || wgcInfoIngressHard > 0 ||
                                                     wgcInfoIngressDecimated > 0 || wgcInfoPoolFreeMin == 0;
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
                        ce::capture_policy::SelectWgcOverlayWarningKind(overloadFlags, wgcCaptureHealthFlags) ==
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
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
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

    if (hTimer) {
        CloseHandle(hTimer);
    }

    if (!bufferedWgcFrames.empty()) {
        ClearBufferedWgcFrames();
    }

    if (g_pSharedMem && liveTicksOutput > 0) {
        auto& state = g_pSharedMem->runtimeState;
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
                "[WGC CFR OVERLOAD PACER] Episodes=%llu RepeatDecisions=%llu Emitted=%llu FreshGrants=%llu MaxRepeatRun=%u "
                "MinFreshFraction=%.3f FreshSvcEma=%.2fms/%u RepeatSvcEma=%.2fms/%u BudgetRatio=%.2f "
                "PtsGrid=immutable AudioTimeline=unchanged",
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.episodes),
                static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats),
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
            const bool wgcLogSnapshotHasPool = g_WgcRuntimeLogSnapshot.hasPoolEvidence.load(std::memory_order_acquire);
            const uint32_t wgcSummarySourceFramePoolBuffers =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceFramePoolBuffers.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSourceFramePoolBufferCount() : 0u);
            const uint32_t wgcSummaryBudgetSurfaces =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.budgetSurfaces.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessBudgetSurfaceCount() : 0u);
            const uint32_t wgcSummarySyncFrames =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.syncFrames.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSyncFrameCount() : 0u);
            const uint32_t wgcSummaryExtraFrames =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.extraFrames.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u);
            const uint32_t wgcSummaryRetainedCap =
                wgcLogSnapshotHasPool
                    ? g_WgcRuntimeLogSnapshot.retainedCap.load(std::memory_order_relaxed)
                    : (g_WgcCap ? g_WgcCap->GetSmoothnessRetainedFrameCap() : wgcSmoothnessRetainedFrameCap);
            const uint32_t wgcSummaryReservedFreeSlots =
                wgcLogSnapshotHasPool
                    ? g_WgcRuntimeLogSnapshot.reservedFreeSlots.load(std::memory_order_relaxed)
                    : (g_WgcCap ? g_WgcCap->GetSmoothnessReservedFreeSlotCount() : wgcSmoothnessReservedFreeSlots);
            const uint32_t wgcSummarySafetySlots =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.safetySlots.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSafetySlotCount() : 0u);
            const uint32_t wgcSummaryPoolLeasedMax =
                wgcLogSnapshotHasPool ? std::max(g_WgcRuntimeLogSnapshot.poolLeasedMax.load(std::memory_order_relaxed),
                                                 g_WgcCap ? g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u)
                                      : (g_WgcCap ? g_WgcCap->GetPoolSlotLeasedMaxCount() : 0u);
            const uint32_t wgcSnapshotFreeMin = g_WgcRuntimeLogSnapshot.poolFreeMin.load(std::memory_order_relaxed);
            const uint32_t wgcCurrentFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
            const uint32_t wgcSummaryPoolFreeMin =
                (wgcLogSnapshotHasPool && wgcSnapshotFreeMin != UINT32_MAX)
                    ? (wgcCurrentFreeMin > 0 ? std::min(wgcSnapshotFreeMin, wgcCurrentFreeMin) : wgcSnapshotFreeMin)
                    : wgcCurrentFreeMin;
            const uint32_t wgcSummaryPoolFreeNow = g_WgcCap ? g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
            const uint32_t wgcSummaryPoolSaturatedDrops =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolSaturatedDrops.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolSaturatedDropCount() : 0u);
            const uint32_t wgcSummaryOverwritePrevented =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolOverwritePrevented.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolSlotOverwritePreventedCount() : 0u);
            const uint32_t wgcSummaryLeaseMismatches =
                wgcLogSnapshotHasPool
                    ? std::max(g_WgcRuntimeLogSnapshot.poolLeaseMismatches.load(std::memory_order_relaxed),
                               g_WgcCap ? g_WgcCap->GetPoolLeaseMismatchCount() : 0u)
                    : (g_WgcCap ? g_WgcCap->GetPoolLeaseMismatchCount() : 0u);
            const uint64_t wgcSummarySmoothVramBytes =
                wgcLogSnapshotHasPool && g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed) > 0
                    ? g_WgcRuntimeLogSnapshot.estimatedVramBytes.load(std::memory_order_relaxed)
                    : wgcSmoothnessEstimatedVramBytes;
            const uint32_t wgcSummarySourceFormat =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceFormat.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceDxgiFormat() : 0u);
            const uint32_t wgcSummaryRetainedFormat =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.retainedFormat.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyDxgiFormat() : 0u);
            const uint32_t wgcSummaryCompactRetained =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.compactRetained.load(std::memory_order_relaxed)
                                      : (g_WgcCap && g_WgcCap->IsCompactRetainedCopyActive() ? 1u : 0u);
            const uint64_t wgcSummarySourceBudgetBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceBudgetBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummaryCopyBudgetBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.copyBudgetBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyEstimatedVramBytes() : 0ull);
            const uint64_t wgcSummarySourceSurfaceBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.sourceSurfaceBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessSourceBytesPerSurface() : 0ull);
            const uint64_t wgcSummaryCopySurfaceBytes =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.copySurfaceBytes.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetSmoothnessCopyBytesPerSurface() : 0ull);
            const int64_t wgcSummaryConvertUs =
                wgcLogSnapshotHasPool ? g_WgcRuntimeLogSnapshot.lastConvertUs.load(std::memory_order_relaxed)
                                      : (g_WgcCap ? g_WgcCap->GetLastPoolConvertTimeUs() : 0);
            const uint32_t wgcSummaryOutputFps = getWgcSmoothnessOutputFps();
            const uint32_t wgcSummaryDuplicateTimestampsSeen =
                std::max(g_WgcRuntimeLogSnapshot.duplicateTimestampsSeen.load(std::memory_order_relaxed),
                         g_WgcCap ? g_WgcCap->GetNormalizedDuplicateTimestampCount() : 0u);
            const uint32_t wgcSummaryDuplicateTimestampsSkipped =
                std::max(g_WgcRuntimeLogSnapshot.duplicateTimestampsSkipped.load(std::memory_order_relaxed),
                         g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : 0u);
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
