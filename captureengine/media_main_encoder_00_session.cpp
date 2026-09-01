#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void EncoderThreadFunc(const AppConfig& config) {
    MediaEncoderSession session(config);
    session.Run();
}

void MediaEncoderSession::Run() {
    if (!Init()) {
        return;
    }
        while (media_main_g_EncoderRunning || media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) || media_main_g_FrameQueue.Size() > 0 ||
               !bufferedWgcFrames.empty() || !bufferedInjectFrames.empty()) {
        LoopStart();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopPressure();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopCatchup();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopWgcTarget();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopWgcSelect();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopStartup();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopEmit();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopEncode();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
        LoopHealth();
        if (continueMainLoop) {
            continueMainLoop = false;
            continue;
        }
        if (breakMainLoop) {
            breakMainLoop = false;
            break;
        }
    }
    Shutdown();
}

bool MediaEncoderSession::Init() {
    LogInfo("[EncoderThread] Started");

    {
        std::lock_guard<std::mutex> lock(media_main_g_WgcCursorPublicationMutex);
        media_main_g_WgcCursorTimeline.Clear();
        media_main_g_DxgiCursorTimelinePublished.store(0, std::memory_order_release);
    }
    media_main_g_InjectCursorTimeline.Clear();

    DisableCurrentThreadPowerThrottling("EncoderThread");
    ScopedMmcssTask encoderMmcssTask(L"Pro Audio", AVRT_PRIORITY_HIGH, "EncoderThread");

    media_main_g_FrameQueue.StartRecording();
    if (!TryArmCapturePipelineWarmup()) {
        const uint32_t phase = media_main_g_pSharedMem
                                   ? media_main_g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire)
                                   : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
        LogInfo("[RecordingLifecycle] Encoder warmup cancelled before arm (phase=%s requested=%d)",
                CapturePipelinePhaseToString(phase), media_main_g_Recording.load(std::memory_order_acquire) ? 1 : 0);
        return false;
    }

    QueryPerformanceFrequency(&qpcFreq);
    privacyRuntime.Reset(config.blackWhenNoFullscreenFocus);
    targetIntervalTicks = qpcFreq.QuadPart / config.video.fps;
    captureSyncMultiplier =
        static_cast<uint32_t>(std::clamp(config.fpsLimiter.captureSyncMultiplier, 1, 8));
    captureSyncPhaseLockEnabled =
        config.fpsLimiter.captureSyncEnabled && !config.video.useVFR && targetIntervalTicks > 0;
    captureSyncSourceIntervalTicks = ce::capture_policy::GetCfrCaptureSyncSourceIntervalQpc(
        targetIntervalTicks, captureSyncMultiplier);
    QueryPerformanceCounter(&nextSampleTime);

    hTimer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!hTimer) {
        hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
        LogInfo("[EncoderThread] Using standard waitable timer");
    } else {
        LogInfo("[EncoderThread] Using high-resolution waitable timer");
    }

    smoothedEncodeMs = 0.0;
    smoothedWgcFreshServiceMs = 0.0;
    smoothedWgcRepeatServiceMs = 0.0;
    wgcFreshServiceSamples = 0;
    wgcRepeatServiceSamples = 0;
    frameIntervalMs = 1000.0 / config.video.fps;
    encoderWakeLateAccumUs = 0;
    encoderWakeLateSamples = 0;
    encoderWakeLateMaxUs = 0;
    drainedScreenGrabFrames.reserve(8);
    drainedWgcCapturedFrames.reserve(8);
    observedWgcSourceEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
    lastSuccessfulWgcCursorEmbedded = false;
    hasSuccessfulWgcCursorMetadata = false;
    wgcFreshCandidateIndices.reserve(64);
    wgcFallbackCandidateIndices.reserve(64);
    wgcRelaxedFreshCandidateIndices.reserve(64);
    wgcRelaxedFallbackCandidateIndices.reserve(64);
    wgcRepeatRescueCandidateIndices.reserve(64);
    drainedInjectFrames.reserve(8);
    smoothedInjectFenceMs = 0.0;
    recordingOutputLive = false;
    pendingLiveActivation = false;
    startupWarmupStartTick = GetTickCount64();
    recordingLiveTick = 0;
    hiddenStartupFrames = 0;
    warmupState = {
        IsActiveScreenGrab(),
        GetTickCount64(),
        0,
    };
    pendingInjectTrimmedLogCount = 0;
    maxBufferedInjectDepthSinceLog = 0;
    lastInjectTrimLog = GetTickCount();
    // Source-rate EMA is telemetry/recovery context only. Live CFR source choice is timestamp-driven.
    pacingInputThisWindow = 0;
    pacingTicksThisWindow = 0;
    pacingEmaUpdates = 0;
    smoothedInputPerTick = 1.0;    // EMA: avg unique frames per encoder tick
    frameCreditAccumulator = 0.0;  // Bresenham error term
    // Output-grid tracking for timestamp-aware frame selection.
    // When multiple buffered frames are available (game fps > target fps),
    // selecting the frame closest to the ideal output grid time produces
    // the smoothest motion in the CFR output.
    encoderGridStartQpc = 0;
    encoderGridTickCount = 0;
    selectionLogCounter = 0;
    lastEncodedInjectFrameIndex = 0;
    injectWorstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
    injectBestSourceFpsX100 = 0;
    injectWorstSourceJitterUs = 0;
    injectWorstSelectionErrorUs = 0;
    smoothedEncCycleMs = 0.0;
    smoothedInjectServiceMs = 0.0;
    injectOverloadRepeatRuntime = {};
    injectServiceMaxUs = 0;
    encCycleMaxMs = 0;
    encodeSpikeCountThisSecond = 0;
    dupTimestampCount = 0;
    lastWgcDuplicateTimestampSkipCountForCadence = 0;
    lastDuplicateReasonNoSource = 0;
    lastDuplicateReasonDeferred = 0;
    lastDuplicateReasonTimerRebase = 0;
    lastDuplicateReasonDrain = 0;
    lastInvalidMetaCount = 0;
    lastInvalidHandleCount = 0;
    lastTimestampRegressionCount = 0;
    lastTimestampStallCount = 0;
    lastPacketClampCount = 0;
    lastNegativePtsCount = 0;
    lastNonMonotonicPtsCount = 0;
    injectDeferredRequeuedThisWindow = 0;
    injectDeferredDroppedThisWindow = 0;
    injectDeferredRequeuedTotal = 0;
    injectDeferredDroppedTotal = 0;
    injectFreshCatchupThisWindow = 0;
    injectRepeatCatchupThisWindow = 0;
    injectLiveStaleTrimThisWindow = 0;
    injectTargetSelectThisWindow = 0;
    injectTargetSupersededThisWindow = 0;
    injectTargetHoldThisWindow = 0;
    injectTargetHoldWithCandidateThisWindow = 0;
    activePathMismatchDiscardThisWindow = 0;
    injectFreshCatchupTotal = 0;
    injectRepeatCatchupTotal = 0;
    injectLiveStaleTrimTotal = 0;
    injectTargetSelectTotal = 0;
    injectTargetSupersededTotal = 0;
    injectTargetHoldTotal = 0;
    injectTargetHoldWithCandidateTotal = 0;
    injectBufferCapTrimTotal = 0;
    injectTargetResidualMaxUs = 0;
    injectDisplayTimingObservations.clear();
    injectDisplayTimingActiveGeneration = 0;
    injectDisplayTimingLastMatchedSequence = 0;
    injectDisplayTimingOffsetValid = false;
    injectDisplayTimingOffsetQpc = 0;
    injectDisplayTimingResolvedCount = 0;
    injectDisplayTimingFallbackCount = 0;
    injectDisplayTimingPendingCount = 0;
    injectDisplayTimingLastLog = GetTickCount();
    injectTimestampPathKnown = false;
    injectTimestampFinalOutputPathActive = false;
    injectNonFinalTimestampOffsetQpc = 0;
    injectTimestampPathTransitionCount = 0;
    injectTimestampRetentionLimit = 0;
    injectTimestampPhaseReservePeak = 0;
    injectTimestampPhaseCurrentQpc = 0;
    injectTimestampPhaseMaxQpc = 0;
    injectFrontPreserveTrimTotal = 0;
    injectCfrRecoveryActive = false;
    injectEncoderServiceTooSlowCurrent = false;
    injectCfrRecoveryEpisodesThisWindow = 0;
    injectCfrRecoveryEpisodesTotal = 0;
    injectCfrRecoveryStartTick = 0;
    injectCfrRecoveryStartDebt = 0;
    injectCfrRecoveryBestDebt = 0;
    injectCfrRecoveryStartFreshCatchup = 0;
    injectCfrRecoveryStartRepeatCatchup = 0;
    injectCfrRecoveryLastProgressLogTick = 0;
    activePathMismatchDiscardTotal = 0;
    pendingLiveInjectReadyFrames = 0;
    lastHealthLog = GetTickCount();
    liveTicksOutput = 0;
    liveTicksScheduled = 0;
    liveTicksDiscardedByTimerRebase = 0;
    wgcVisualDebtMaxExcessTicks = 0;
    wgcLiveSchedulerRebaseTotal = 0;
    wgcLiveSchedulerRebaseMaxTicks = 0;
    wgcLiveSchedulerRebaseThisWindow = 0;
    wgcStopDrainHeldFrameLogged = false;
    wgcSelectionErrorAccumUs = 0;
    wgcSelectionErrorSignedAccumUs = 0;
    wgcSelectionErrorSamples = 0;
    wgcSelectionErrorMaxUs = 0;
    wgcSelectionEarlyMaxUs = 0;
    wgcSelectionLateMaxUs = 0;
    wgcSelectionTargetClampCount = 0;
    wgcSelectionTargetClampMaxUs = 0;
    wgcHoldForNextTickCount = 0;
    wgcSelectionDelayTickCount = 0;
    wgcSyncDelayHoldCount = 0;
    wgcSyncDelayHoldTotal = 0;
    wgcSyncDelaySourceLimitedHoldCount = 0;
    wgcSyncDelaySourceLimitedHoldTotal = 0;
    wgcSyncDelayPolicyHoldCount = 0;
    wgcSyncDelayPolicyHoldTotal = 0;
    wgcTooNewLeadMaxUs = 0;
    wgcTooNewLeadSessionMaxUs = 0;
    wgcStartupReserveFrames = 0;
    wgcStartupReserveSpanUs = 0;
    wgcStartupDelayTargetUs = 0;
    wgcStartupSelectedByDelayReserve = false;
    wgcStartupReserveReason = "not-run";
    wgcSmoothnessActiveDelayQpc = 0;
    pendingWgcStartContractGeneration = 0;
    committedWgcStartContractGeneration = 0;
    wgcEncoderPrewarmAttempted = false;
    wgcEncoderPrewarmSucceeded = false;
    wgcEncoderPrewarmElapsedUs = 0;
    // Smoothness FLOOR diagnostics/state (resolved once at startup, then fixed for the session).
    wgcSmoothnessFloorDelayQpc = 0;            // resolved floor delay target (QPC); 0 = floor inactive
    wgcSmoothnessFloorRequestedQpc = 0;        // pre-clamp requested floor (QPC), for logging
    wgcSmoothnessFloorSource = "off";      // off | auto | config
    wgcSmoothnessFloorClampedBy = "none";  // none | min | max_ms | reservoir
    wgcSmoothnessFloorLogged = false;  // latch: the one-time floor decision log fires once per recording
    wgcSmoothnessDesiredFrames = 0;
    wgcSmoothnessRetainedFrames = 0;
    wgcSmoothnessActualFrames = 0;
    wgcSmoothnessPoolSlots = 0;
    wgcSmoothnessRetainedFrameCap = 0;
    wgcSmoothnessReservedFreeSlots = 0;
    wgcSmoothnessEstimatedVramBytes = 0;
    wgcSmoothnessCapLimited = false;
    wgcSmoothnessBufferReason = "not-run";
    wgcDelayReservoirLowWaterTickCount = 0;
    wgcDelayReservoirLowWaterTickTotal = 0;
    // WGC selection-timestamp smoothing telemetry (monotonic bounded-deviation
    // smoother over raw compositor timestamps; see InputFrameRatePredictor::
    // SmoothMonotonicTimestamp). Deviation = |selection - raw normalized|.
    wgcTsSmoothSamplesWindow = 0;
    wgcTsSmoothDevAccumUsWindow = 0;
    wgcTsSmoothDevMaxUsWindow = 0;
    wgcTsSmoothDevMaxUsTotal = 0;
    wgcTsSmoothSnapCountWindow = 0;
    wgcTsSmoothSnapCountTotal = 0;
    wgcDelayResidualSamples = 0;
    wgcDelayResidualAbsAccumUs = 0;
    wgcDelayResidualSignedAccumUs = 0;
    wgcDelayResidualAbsMaxUs = 0;
    wgcDelayResidualLateMaxUs = 0;
    wgcDelayResidualEarlyMaxUs = 0;
    wgcDelayRealizedAccumUs = 0;
    wgcDelayRealizedMinUs = UINT32_MAX;
    wgcDelayRealizedMaxUs = 0;
    wgcDelayResidualWindowSamples = 0;
    wgcDelayResidualWindowAbsAccumUs = 0;
    wgcDelayResidualWindowSignedAccumUs = 0;
    wgcDelayResidualWindowAbsMaxUs = 0;
    wgcDelayResidualWindowLateMaxUs = 0;

    wgcDelayRawResidualSamples = 0;
    wgcDelayRawResidualAbsAccumUs = 0;
    wgcDelayRawResidualSignedAccumUs = 0;
    wgcDelayRawResidualAbsMaxUs = 0;
    wgcDelayRawResidualLateMaxUs = 0;
    wgcDelayRawResidualEarlyMaxUs = 0;
    wgcDelayRawResidualWindowSamples = 0;
    wgcDelayRawResidualWindowAbsAccumUs = 0;
    wgcDelayRawResidualWindowSignedAccumUs = 0;
    wgcDelayRawResidualWindowAbsMaxUs = 0;
    wgcDelayRawResidualWindowLateMaxUs = 0;
    wgcDelayRawMinusPredictedSamples = 0;
    wgcDelayRawMinusPredictedSignedAccumUs = 0;
    wgcDelayRawMinusPredictedAbsMaxUs = 0;
    wgcDelayRawMinusPredictedWindowSamples = 0;
    wgcDelayRawMinusPredictedWindowSignedAccumUs = 0;
    wgcDelayRawMinusPredictedWindowAbsMaxUs = 0;
    wgcDelayRelaxedSelectionCount = 0;
    wgcDelayRelaxedSelectionWindowCount = 0;
    wgcDelayRelaxedSelectionMaxUs = 0;
    wgcDelayRelaxedBetterTargetTotal = 0;
    wgcDelayRelaxedBetterTargetWindow = 0;
    wgcDelayRelaxedRepeatClusterTotal = 0;
    wgcDelayRelaxedRepeatClusterWindow = 0;
    wgcDelayRelaxedRejectedSyncRiskTotal = 0;
    wgcDelayRelaxedRejectedSyncRiskWindow = 0;
    wgcDelayRelaxedRejectedResidualHeadroomTotal = 0;
    wgcDelayRelaxedRejectedResidualHeadroomWindow = 0;
    wgcDelayRelaxedRejectedRepeatCostTotal = 0;
    wgcDelayRelaxedRejectedRepeatCostWindow = 0;
    wgcDelaySoftLateRejectedTotal = 0;
    wgcDelaySoftLateRejectedWindow = 0;
    wgcDelaySoftLateAcceptedTotal = 0;
    wgcDelaySoftLateAcceptedWindow = 0;
    wgcDelayNearCapAcceptedTotal = 0;
    wgcDelayNearCapAcceptedWindow = 0;
    // Frames selected under uniform-cadence active-delay mode (reserve-defense perturbations
    // skipped, closest-to-target with monotonic + hard-cap guards). Lets the GPU-bound judder
    // fix be confirmed from logs and distinguishes it from per-tick reserve defense.
    wgcDelayUniformCadenceTotal = 0;
    wgcDelayUniformCadenceWindow = 0;
    wgcDelayUniformHoldTotal = 0;
    wgcDelayUniformHoldWindow = 0;
    // Reservoir depth-cap trims: surplus oldest frames the uniform-cadence pacer dropped to keep the
    // realized content delay from inflating when a VRR source transiently delivered above output.
    wgcDelayPaceCapTrimTotal = 0;
    wgcDelayPaceCapTrimWindow = 0;
    wgcDelayPaceCapTrimLastLogTick = 0;
    // Deep encoder debt can leave every retained source frame newer than the immutable content target.
    // Count the resulting sync-protected holds and their worst grid-relative lead; held-repeat catch-up
    // advances the output grid without relabelling those future pixels into an older audio-aligned slot.
    wgcUniformGridDebtHoldTotal = 0;
    wgcUniformGridDebtLeadMaxUs = 0;
    wgcRetainedCapTrimTotal = 0;
    wgcRetainedCapTrimWindow = 0;
    wgcRetainedCapTrimLastLogTick = 0;
    wgcPoolPressureTrimTotal = 0;
    wgcPoolPressureTrimWindow = 0;
    wgcPoolPressureTrimLastLogTick = 0;
    wgcDelayOlderFrameAvoidedRepeatTotal = 0;
    wgcDelayOlderFrameAvoidedRepeatWindow = 0;
    wgcDelaySourceLimitedRepeatTotal = 0;
    wgcDelaySourceLimitedRepeatWindow = 0;
    wgcDelayRepeatRescueAttemptTotal = 0;
    wgcDelayRepeatRescueAttemptWindow = 0;
    wgcDelayRepeatRescueSuccessTotal = 0;
    wgcDelayRepeatRescueSuccessWindow = 0;
    wgcDelayRepeatRescueRejectedSyncTotal = 0;
    wgcDelayRepeatRescueRejectedSyncWindow = 0;
    wgcDelayRepeatRescueRejectedHeadroomTotal = 0;
    wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
    wgcDelayRepeatRescueRejectedCostTotal = 0;
    wgcDelayRepeatRescueRejectedCostWindow = 0;
    wgcDelayRepeatPromotedBeforeRepeatTotal = 0;
    wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
    wgcDelayRepeatPromotionAttemptTotal = 0;
    wgcDelayRepeatPromotionAttemptWindow = 0;
    wgcDelayRepeatPromotionRejectedSoftTotal = 0;
    wgcDelayRepeatPromotionRejectedSoftWindow = 0;
    wgcDelayRepeatSafeAfterPromotionTotal = 0;
    wgcDelayRepeatSafeAfterPromotionWindow = 0;
    wgcDelayRepeatWithSafeCandidateTotal = 0;
    wgcDelayRepeatWithSafeCandidateWindow = 0;
    wgcDelayRepeatWithoutSafeCandidateTotal = 0;
    wgcDelayRepeatWithoutSafeCandidateWindow = 0;
    wgcDelayRepeatWithSoftSafeCandidateTotal = 0;
    wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
    wgcDelayRepeatWithoutSoftSafeCandidateTotal = 0;
    wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
    wgcDelayRepeatHardOnlyCandidateTotal = 0;
    wgcDelayRepeatHardOnlyCandidateWindow = 0;
    wgcDelaySyncProtectedRepeatTotal = 0;
    wgcDelaySyncProtectedRepeatWindow = 0;
    wgcDelayWindowHealthyRepeatTotal = 0;
    wgcDelayWindowHealthyRepeatWindow = 0;
    wgcDelayWindowRecoverableRepeatTotal = 0;
    wgcDelayWindowRecoverableRepeatWindow = 0;
    wgcDelayWindowSourceLimitedRepeatTotal = 0;
    wgcDelayWindowSourceLimitedRepeatWindow = 0;
    wgcDelayWindowHardStallRepeatTotal = 0;
    wgcDelayWindowHardStallRepeatWindow = 0;
    wgcDelayWindowPostStallRepeatTotal = 0;
    wgcDelayWindowPostStallRepeatWindow = 0;
    wgcDelayPostStallSafeFrameTotal = 0;
    wgcDelayPostStallSafeFrameWindow = 0;
    wgcDelayRepeatReserveDepthMax = 0;
    wgcDelayRepeatReserveDepthWindowMax = 0;
    wgcDelayRepeatReserveSpanMaxUs = 0;
    wgcDelayRepeatReserveSpanWindowMaxUs = 0;
    wgcDelayOldestSoftSafeAgeMaxUs = 0;
    wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
    wgcDelayPostSelectionRejectedSyncRiskTotal = 0;
    wgcDelayPostSelectionRejectedSyncRiskWindow = 0;
    wgcDelayPostSelectionRescuedSyncRiskTotal = 0;
    wgcDelayPostSelectionRescuedSyncRiskWindow = 0;
    wgcDelayRepeatClusterPressureTotal = 0;
    wgcDelayRepeatClusterPressureWindow = 0;
    wgcDelayRepeatClusterPressureWindowMaxTicks = 0;
    wgcDelayRepeatClusterPressureMaxTicks = 0;
    wgcSourceRepeatLowerBoundTotal = 0;
    wgcSourceRepeatLowerBoundWindow = 0;
    wgcRollingSourceSlotIndex = 0;
    wgcRollingSourceSlotCount = 0;
    wgcRollingSourceAcceptedSum = 0;
    wgcRollingSourceCfrTickSum = 0;
    wgcRollingSourceAcceptedWindow = 0;
    wgcRollingSourceCfrTicksWindow = 0;
    wgcRollingSourceDeficitFrames = 0;
    wgcRollingSourceSurplusFrames = 0;
    wgcRollingSourceAcceptedTotal = 0;
    wgcRollingSourceCfrTickTotal = 0;
    wgcRollingSourceLastIngressAccepted = 0;
    wgcRollingSourceWindowPrimed = false;
    wgcExcessRepeatTotal = 0;
    wgcExcessRepeatWindow = 0;
    wgcPolicyAddedRepeatTotal = 0;
    wgcPolicyAddedRepeatWindow = 0;
    wgcExcessRepeatClusterTotal = 0;
    wgcExcessRepeatClusterWindow = 0;
    wgcExcessRepeatClusterMaxTicks = 0;
    wgcExcessRepeatClusterWindowMaxTicks = 0;
    wgcSyncDelaySourceRecoveryHoldTotal = 0;
    wgcSyncDelaySourceRecoveryHoldCount = 0;
    wgcActiveDelaySourceRecoveryUntilTick = 0;
    wgcActiveDelaySourceRecoveryTicks = 0;
    wgcProducerRateRetuneCount = 0;
    wgcProducerRateRetuneTotal = 0;
    wgcRecentDeliveredFps = 0;
    wgcRecentDeliveredMin250Fps = 0;
    wgcRecentDeliveredMin500Fps = 0;
    wgcRecentInputMin250Fps = 0;
    wgcRecentInputMin500Fps = 0;
    wgcNoFreshTickCount = 0;
    wgcQueueTickSampleCount = 0;
    wgcNoFreshTickPermille = 0;
    wgcBufferedAtTickSum = 0;
    wgcBufferedAtTickMin = UINT32_MAX;
    wgcNoReserveTickCount = 0;
    wgcAncientSelectionCount = 0;
    wgcFreshSelectionMissCount = 0;
    wgcHeldFreshFrameTickCount = 0;
    cfrCatchupTicksExecuted = 0;
    wgcWarmupUntilQpc = 0;
    wgcBiasAccumQpc = 0;
    wgcBiasClampCount = 0;
    wgcReserveSpendTickCount = 0;
    wgcStaleUniqueFallbackCount = 0;
    wgcRepeatNoFreshCount = 0;
    wgcRepeatPolicyHoldCount = 0;
    wgcRepeatPolicyHoldTotal = 0;
    wgcRepeatTimerLateCount = 0;
    wgcRepeatCatchupCount = 0;
    wgcFreshCatchupCount = 0;
    wgcRepeatCatchupTotal = 0;
    wgcFreshCatchupTotal = 0;
    wgcSelectFreshCount = 0;
    wgcSelectDuplicateSourceCount = 0;
    wgcDropObsoleteCount = 0;
    wgcDropStaleDebtCount = 0;
    wgcDropStaleDebtTotal = 0;
    wgcDropStaleDebtMaxUs = 0;
    wgcEncoderLimitedSourceDropThisWindow = 0;
    wgcEncoderLimitedSourceDropTotal = 0;
    wgcEncoderLimitedSourceDropMaxTicks = 0;
    wgcEncoderLimitedCadenceEventCount = 0;
    wgcEncoderLimitedSuppressedByLowSourceThisWindow = 0;
    wgcEncoderLimitedSuppressedByLowSourceTotal = 0;
    wgcCapacityPressureModeMismatchThisWindow = 0;
    wgcCapacityPressureModeMismatchTotal = 0;
    wgcSelectedSourceBacktrackThisWindow = 0;
    wgcSelectedSourceBacktrackTotal = 0;
    wgcPostStopFrameDropTotal = 0;
    wgcPostStopFrameDropMaxUs = 0;
    wgcCoverageRepeatHoldCount = 0;
    wgcCoverageDelayTicksCurrent = 0;
    wgcAudioLeadExcessMsCurrent = 0.0;
    wgcCoverageRepeatActiveCurrent = false;
    encoderTooSlowForTargetCurrent = false;
    wgcLowSourceModeActive = false;
    wgcLiveRecoveryModeActive = false;
    wgcReservePressureActive = false;
    wgcLowSourceStateChangeTick = 0;
    wgcLiveRecoveryStateChangeTick = 0;
    wgcSourceStarvedCurrent = false;
    wgcSchedulerLimitedCurrent = false;
    wgcEncoderRecoveryLimitedCurrent = false;
    wgcActiveDelayRepeatClassKnown = false;
    wgcActiveDelayLastRepeatClass =
        ce::capture_policy::WgcActiveDelayWindowClass::kHealthy;
    wgcActiveDelayLastRepeatClassLogTick = 0;
    lastEmittedWgcSourceQpc = 0;
    // Selection-domain (smoothed) twin of lastEmittedWgcSourceQpc. The uniform
    // active-delay playout makes its emit/hold/drop decisions in the smoothed
    // selection-timestamp domain (strictly monotonic by construction), so its
    // monotonicity guard must compare in the same domain; raw stays in
    // lastEmittedWgcSourceQpc for the legacy path and sync diagnostics.
    lastEmittedWgcSelectionQpc = 0;
    lastEmittedInjectSourceQpc = 0;
    lastWarmupWgcSourceQpc = 0;
    wgcStartupBarrierQpc = 0;
    wgcStartupBarrierDroppedFrames = 0;
    wgcStartupPreLiveDelayComplete = false;
    wgcStartupPreLiveDelayDroppedFrames = 0;
    wgcAvSyncScheduleOffsetQpc = 0;
    wgcAvSyncStartupAudioAnchorQpc = 0;
    wgcAvSyncStartupVideoQpc = 0;
    wgcAvSyncStartupEffectiveDelayQpc = 0;
    wgcStartupReserveWaitStartQpc = 0;
    wgcStartupReserveWaitCount = 0;
    wgcStartupHistoryProtectionLogged = false;
    wgcStartupReserveWaitInitialSpanUs = 0;
    wgcStartupReserveWaitFreshenedMax = 0;
    wgcFreshWarmupFrameCount = 0;
    wgcOldestBufferedFrameAgeUs = 0;
    wgcCoverageRepeatAccumulator = 0.0;
    InitState();
    return true;
}

void MediaEncoderSession::InitState() {
    wgcStarvedEpisode.Reset();
    captureSessionSummary.Reset();
    minLoggedWgcStarvedEpisodeMs =
        static_cast<uint64_t>(std::llround(std::max(100.0, frameIntervalMs * 8.0)));

    // A/V content delay: align video content with inherently-late loopback audio by biasing
    // WGC source-frame selection back by the loopback capture latency (= the slowest audio
    // source's latency, so faster sources can be equalized up to it). Audio samples and the
    // CFR PTS grid are untouched, so track length/start/end and zero-drift are preserved.
    // Video buffer self-builds/holds via the bounded "too new for slot" path. QPC ticks.
    maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    avContentDelayQpc =
        (maxAudioCaptureLatencyMs > 0.0f && qpcFreq.QuadPart > 0)
            ? static_cast<int64_t>(std::llround(static_cast<double>(maxAudioCaptureLatencyMs) / 1000.0 *
                                                static_cast<double>(qpcFreq.QuadPart)))
            : 0;
    avContentDelayActive = avContentDelayQpc > 0;
    // Smoothness FLOOR (WGC only): engage the active-delay jitter-absorbing playout even when there
    // is no audio-latency content delay. Configured = auto or explicit > 0; only meaningful for the
    // Screen-grab smoothness can arm without an A/V delay; the floor magnitude is resolved once
    // at the startup barrier from measured delivery jitter.
    wgcSmoothnessFloorConfigured = IsActiveScreenGrab() && config.wgcSmoothnessBufferEnabled &&
                                              !config.video.useVFR &&
                                              (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    wgcSmoothnessDelayDesired =
        ce::capture_policy::WgcSmoothnessDelayDesired(avContentDelayActive, wgcSmoothnessFloorConfigured);
    // Inject delays content by retaining whole extra frames above its oldest-first reserve.
    injectContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0)
            ? static_cast<size_t>(std::ceil(static_cast<double>(maxAudioCaptureLatencyMs) / frameIntervalMs))
            : 0;
    injectResidualEstimateMs = (injectContentDelayFrames > 0 && frameIntervalMs > 0.0)
                                                ? static_cast<double>(injectContentDelayFrames) * frameIntervalMs -
                                                      static_cast<double>(maxAudioCaptureLatencyMs)
                                                : 0.0;
    avContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0) ? maxAudioCaptureLatencyMs / frameIntervalMs : 0.0;
    if (avContentDelayActive) {
        LogInfo(
            "[AVSyncApply] armed: maxAudioCaptureLatencyMs=%.3f delayUs=%lld method=%s injectDelayFrames=%zu "
            "videoDelayFrames=%.2f residualEstimateMs=%.3f confidence=%s reason=%s "
            "(delays video content to match late loopback audio; audio/PTS unchanged)",
            static_cast<double>(maxAudioCaptureLatencyMs),
            (long long)((avContentDelayQpc * 1000000) / qpcFreq.QuadPart),
            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", injectContentDelayFrames,
            avContentDelayFrames, IsActiveScreenGrab() ? 0.0 : injectResidualEstimateMs,
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
        if (IsActiveScreenGrab()) {
            LogInfo(
                "[AVSyncApply] wgc_cadence_policy: uniformCadence=%d (when active, WGC paces the content "
                "delay like the inject path: a delay-deep frame floor with unique frames advanced at the "
                "source input rate, so a VRR/under-delivering source keeps the realized delay stable and "
                "the source-limited repeats uniform instead of clustering into delay-slot holds)",
                config.wgcActiveDelayUniformCadence ? 1 : 0);
        }
    } else {
        LogWarn(
            "[AVSyncApply] inactive: maxAudioCaptureLatencyMs=%.3f delayUs=0 method=%s injectDelayFrames=0 "
            "residualEstimateMs=0.000 confidence=%s reason=%s",
            static_cast<double>(maxAudioCaptureLatencyMs),

            IsActiveScreenGrab() ? "wgc-selection-bias" : "inject-buffer-reserve", config.avSyncConfidence.c_str(),
            config.avSyncReason.c_str());
    }

}

void MediaEncoderSession::ReleaseQueuedFrameTexture(QueuedFrame& queuedFrame) {

if (!queuedFrame.isInjectMode && queuedFrame.texture) {
    queuedFrame.texture->Release();
    queuedFrame.texture = nullptr;
}
if (!queuedFrame.isInjectMode) {
    queuedFrame.wgcPoolLease.Reset();
    queuedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
    queuedFrame.wgcPoolGeneration = 0;
}

}

void MediaEncoderSession::DiscardQueuedFrame(QueuedFrame& queuedFrame) {

if (!queuedFrame.isInjectMode) {
    ReleaseQueuedFrameTexture(queuedFrame);
}
queuedFrame = QueuedFrame{};

}
