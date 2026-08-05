#include "media_main_internal.h"

void EncoderThreadFunc(const AppConfig& config) {
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
    ce::capture_policy::RecordingHealthState recordingHealthState;
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
    uint64_t observedWgcSourceEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
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
    std::array<uint32_t, media_main_kInjectTextureSlotCount> lastEncodedFrameByTextureIndex{};
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

    std::array<uint32_t, 256> wgcDelayResidualWindowAbsHistogram{};
    uint64_t wgcDelayRawResidualSamples = 0;
    uint64_t wgcDelayRawResidualAbsAccumUs = 0;
    int64_t wgcDelayRawResidualSignedAccumUs = 0;
    uint32_t wgcDelayRawResidualAbsMaxUs = 0;
    uint32_t wgcDelayRawResidualLateMaxUs = 0;
    uint32_t wgcDelayRawResidualEarlyMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayRawResidualAbsHistogram{};
    uint64_t wgcDelayRawResidualWindowSamples = 0;
    uint64_t wgcDelayRawResidualWindowAbsAccumUs = 0;
    int64_t wgcDelayRawResidualWindowSignedAccumUs = 0;
    uint32_t wgcDelayRawResidualWindowAbsMaxUs = 0;
    uint32_t wgcDelayRawResidualWindowLateMaxUs = 0;
    std::array<uint32_t, 256> wgcDelayRawResidualWindowAbsHistogram{};
    uint64_t wgcDelayRawMinusPredictedSamples = 0;
    int64_t wgcDelayRawMinusPredictedSignedAccumUs = 0;
    uint32_t wgcDelayRawMinusPredictedAbsMaxUs = 0;
    uint64_t wgcDelayRawMinusPredictedWindowSamples = 0;
    int64_t wgcDelayRawMinusPredictedWindowSignedAccumUs = 0;
    uint32_t wgcDelayRawMinusPredictedWindowAbsMaxUs = 0;
    uint64_t wgcDelayRelaxedSelectionCount = 0;
    uint32_t wgcDelayRelaxedSelectionWindowCount = 0;
    uint32_t wgcDelayRelaxedSelectionMaxUs = 0;
    uint64_t wgcDelayRelaxedBetterTargetTotal = 0;
    uint32_t wgcDelayRelaxedBetterTargetWindow = 0;
    uint64_t wgcDelayRelaxedRepeatClusterTotal = 0;
    uint32_t wgcDelayRelaxedRepeatClusterWindow = 0;
    uint64_t wgcDelayRelaxedRejectedSyncRiskTotal = 0;
    uint32_t wgcDelayRelaxedRejectedSyncRiskWindow = 0;
    uint64_t wgcDelayRelaxedRejectedResidualHeadroomTotal = 0;
    uint32_t wgcDelayRelaxedRejectedResidualHeadroomWindow = 0;
    uint64_t wgcDelayRelaxedRejectedRepeatCostTotal = 0;
    uint32_t wgcDelayRelaxedRejectedRepeatCostWindow = 0;
    uint64_t wgcDelaySoftLateRejectedTotal = 0;
    uint32_t wgcDelaySoftLateRejectedWindow = 0;
    uint64_t wgcDelaySoftLateAcceptedTotal = 0;
    uint32_t wgcDelaySoftLateAcceptedWindow = 0;
    uint64_t wgcDelayNearCapAcceptedTotal = 0;
    uint32_t wgcDelayNearCapAcceptedWindow = 0;
    // Frames selected under uniform-cadence active-delay mode (reserve-defense perturbations
    // skipped, closest-to-target with monotonic + hard-cap guards). Lets the GPU-bound judder
    // fix be confirmed from logs and distinguishes it from per-tick reserve defense.
    uint64_t wgcDelayUniformCadenceTotal = 0;
    uint32_t wgcDelayUniformCadenceWindow = 0;
    uint64_t wgcDelayUniformHoldTotal = 0;
    uint32_t wgcDelayUniformHoldWindow = 0;
    // Reservoir depth-cap trims: surplus oldest frames the uniform-cadence pacer dropped to keep the
    // realized content delay from inflating when a VRR source transiently delivered above output.
    uint64_t wgcDelayPaceCapTrimTotal = 0;
    uint32_t wgcDelayPaceCapTrimWindow = 0;
    DWORD wgcDelayPaceCapTrimLastLogTick = 0;
    // Deep encoder debt can leave every retained source frame newer than the immutable content target.
    // Count the resulting sync-protected holds and their worst grid-relative lead; held-repeat catch-up
    // advances the output grid without relabelling those future pixels into an older audio-aligned slot.
    uint64_t wgcUniformGridDebtHoldTotal = 0;
    uint64_t wgcUniformGridDebtLeadMaxUs = 0;
    uint64_t wgcRetainedCapTrimTotal = 0;
    uint32_t wgcRetainedCapTrimWindow = 0;
    DWORD wgcRetainedCapTrimLastLogTick = 0;
    uint64_t wgcPoolPressureTrimTotal = 0;
    uint32_t wgcPoolPressureTrimWindow = 0;
    DWORD wgcPoolPressureTrimLastLogTick = 0;
    uint64_t wgcDelayOlderFrameAvoidedRepeatTotal = 0;
    uint32_t wgcDelayOlderFrameAvoidedRepeatWindow = 0;
    uint64_t wgcDelaySourceLimitedRepeatTotal = 0;
    uint32_t wgcDelaySourceLimitedRepeatWindow = 0;
    uint64_t wgcDelayRepeatRescueAttemptTotal = 0;
    uint32_t wgcDelayRepeatRescueAttemptWindow = 0;
    uint64_t wgcDelayRepeatRescueSuccessTotal = 0;
    uint32_t wgcDelayRepeatRescueSuccessWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedSyncTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedSyncWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedHeadroomTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedHeadroomWindow = 0;
    uint64_t wgcDelayRepeatRescueRejectedCostTotal = 0;
    uint32_t wgcDelayRepeatRescueRejectedCostWindow = 0;
    uint64_t wgcDelayRepeatPromotedBeforeRepeatTotal = 0;
    uint32_t wgcDelayRepeatPromotedBeforeRepeatWindow = 0;
    uint64_t wgcDelayRepeatPromotionAttemptTotal = 0;
    uint32_t wgcDelayRepeatPromotionAttemptWindow = 0;
    uint64_t wgcDelayRepeatPromotionRejectedSoftTotal = 0;
    uint32_t wgcDelayRepeatPromotionRejectedSoftWindow = 0;
    uint64_t wgcDelayRepeatSafeAfterPromotionTotal = 0;
    uint32_t wgcDelayRepeatSafeAfterPromotionWindow = 0;
    uint64_t wgcDelayRepeatWithSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithoutSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithoutSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithSoftSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithSoftSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatWithoutSoftSafeCandidateTotal = 0;
    uint32_t wgcDelayRepeatWithoutSoftSafeCandidateWindow = 0;
    uint64_t wgcDelayRepeatHardOnlyCandidateTotal = 0;
    uint32_t wgcDelayRepeatHardOnlyCandidateWindow = 0;
    uint64_t wgcDelaySyncProtectedRepeatTotal = 0;
    uint32_t wgcDelaySyncProtectedRepeatWindow = 0;
    uint64_t wgcDelayWindowHealthyRepeatTotal = 0;
    uint32_t wgcDelayWindowHealthyRepeatWindow = 0;
    uint64_t wgcDelayWindowRecoverableRepeatTotal = 0;
    uint32_t wgcDelayWindowRecoverableRepeatWindow = 0;
    uint64_t wgcDelayWindowSourceLimitedRepeatTotal = 0;
    uint32_t wgcDelayWindowSourceLimitedRepeatWindow = 0;
    uint64_t wgcDelayWindowHardStallRepeatTotal = 0;
    uint32_t wgcDelayWindowHardStallRepeatWindow = 0;
    uint64_t wgcDelayWindowPostStallRepeatTotal = 0;
    uint32_t wgcDelayWindowPostStallRepeatWindow = 0;
    uint64_t wgcDelayPostStallSafeFrameTotal = 0;
    uint32_t wgcDelayPostStallSafeFrameWindow = 0;
    uint32_t wgcDelayRepeatReserveDepthMax = 0;
    uint32_t wgcDelayRepeatReserveDepthWindowMax = 0;
    uint32_t wgcDelayRepeatReserveSpanMaxUs = 0;
    uint32_t wgcDelayRepeatReserveSpanWindowMaxUs = 0;
    uint32_t wgcDelayOldestSoftSafeAgeMaxUs = 0;
    uint32_t wgcDelayOldestSoftSafeAgeWindowMaxUs = 0;
    uint64_t wgcDelayPostSelectionRejectedSyncRiskTotal = 0;
    uint32_t wgcDelayPostSelectionRejectedSyncRiskWindow = 0;
    uint64_t wgcDelayPostSelectionRescuedSyncRiskTotal = 0;
    uint32_t wgcDelayPostSelectionRescuedSyncRiskWindow = 0;
    uint64_t wgcDelayRepeatClusterPressureTotal = 0;
    uint32_t wgcDelayRepeatClusterPressureWindow = 0;
    uint32_t wgcDelayRepeatClusterPressureWindowMaxTicks = 0;
    uint32_t wgcDelayRepeatClusterPressureMaxTicks = 0;
    uint64_t wgcSourceRepeatLowerBoundTotal = 0;
    uint32_t wgcSourceRepeatLowerBoundWindow = 0;
    constexpr size_t kWgcRollingSourceWindowSlots = 5;
    std::array<uint32_t, kWgcRollingSourceWindowSlots> wgcRollingSourceAcceptedSlots{};
    std::array<uint32_t, kWgcRollingSourceWindowSlots> wgcRollingSourceCfrTickSlots{};
    size_t wgcRollingSourceSlotIndex = 0;
    size_t wgcRollingSourceSlotCount = 0;
    uint32_t wgcRollingSourceAcceptedSum = 0;
    uint32_t wgcRollingSourceCfrTickSum = 0;
    uint32_t wgcRollingSourceAcceptedWindow = 0;
    uint32_t wgcRollingSourceCfrTicksWindow = 0;
    uint32_t wgcRollingSourceDeficitFrames = 0;
    uint32_t wgcRollingSourceSurplusFrames = 0;
    uint64_t wgcRollingSourceAcceptedTotal = 0;
    uint64_t wgcRollingSourceCfrTickTotal = 0;
    uint32_t wgcRollingSourceLastIngressAccepted = 0;
    bool wgcRollingSourceWindowPrimed = false;
    uint64_t wgcExcessRepeatTotal = 0;
    uint32_t wgcExcessRepeatWindow = 0;
    uint64_t wgcPolicyAddedRepeatTotal = 0;
    uint32_t wgcPolicyAddedRepeatWindow = 0;
    uint64_t wgcExcessRepeatClusterTotal = 0;
    uint32_t wgcExcessRepeatClusterWindow = 0;
    uint32_t wgcExcessRepeatClusterMaxTicks = 0;
    uint32_t wgcExcessRepeatClusterWindowMaxTicks = 0;
    uint64_t wgcSyncDelaySourceRecoveryHoldTotal = 0;
    uint32_t wgcSyncDelaySourceRecoveryHoldCount = 0;
    uint64_t wgcActiveDelaySourceRecoveryUntilTick = 0;
    uint64_t wgcActiveDelaySourceRecoveryTicks = 0;
    uint32_t wgcProducerRateRetuneCount = 0;
    uint64_t wgcProducerRateRetuneTotal = 0;
    uint32_t wgcRecentDeliveredFps = 0;
    uint32_t wgcRecentDeliveredMin250Fps = 0;
    uint32_t wgcRecentDeliveredMin500Fps = 0;
    uint32_t wgcRecentInputMin250Fps = 0;
    uint32_t wgcRecentInputMin500Fps = 0;
    uint32_t wgcNoFreshTickCount = 0;
    uint32_t wgcQueueTickSampleCount = 0;
    uint32_t wgcNoFreshTickPermille = 0;
    uint32_t wgcBufferedAtTickSum = 0;
    uint32_t wgcBufferedAtTickMin = UINT32_MAX;
    uint32_t wgcNoReserveTickCount = 0;
    uint32_t wgcAncientSelectionCount = 0;
    uint32_t wgcFreshSelectionMissCount = 0;
    uint32_t wgcHeldFreshFrameTickCount = 0;
    uint32_t cfrCatchupTicksExecuted = 0;
    int64_t wgcWarmupUntilQpc = 0;
    int64_t wgcBiasAccumQpc = 0;
    uint32_t wgcBiasClampCount = 0;
    uint32_t wgcReserveSpendTickCount = 0;
    uint32_t wgcStaleUniqueFallbackCount = 0;
    uint32_t wgcRepeatNoFreshCount = 0;
    uint32_t wgcRepeatPolicyHoldCount = 0;
    uint64_t wgcRepeatPolicyHoldTotal = 0;
    uint32_t wgcRepeatTimerLateCount = 0;
    uint32_t wgcRepeatCatchupCount = 0;
    uint32_t wgcFreshCatchupCount = 0;
    uint64_t wgcRepeatCatchupTotal = 0;
    uint64_t wgcFreshCatchupTotal = 0;
    uint32_t wgcSelectFreshCount = 0;
    uint32_t wgcSelectDuplicateSourceCount = 0;
    uint32_t wgcDropObsoleteCount = 0;
    uint32_t wgcDropStaleDebtCount = 0;
    uint64_t wgcDropStaleDebtTotal = 0;
    uint32_t wgcDropStaleDebtMaxUs = 0;
    uint32_t wgcEncoderLimitedSourceDropThisWindow = 0;
    uint64_t wgcEncoderLimitedSourceDropTotal = 0;
    uint32_t wgcEncoderLimitedSourceDropMaxTicks = 0;
    uint64_t wgcEncoderLimitedCadenceEventCount = 0;
    uint32_t wgcEncoderLimitedSuppressedByLowSourceThisWindow = 0;
    uint64_t wgcEncoderLimitedSuppressedByLowSourceTotal = 0;
    uint32_t wgcCapacityPressureModeMismatchThisWindow = 0;
    uint64_t wgcCapacityPressureModeMismatchTotal = 0;
    uint32_t wgcSelectedSourceBacktrackThisWindow = 0;
    uint64_t wgcSelectedSourceBacktrackTotal = 0;
    uint64_t wgcPostStopFrameDropTotal = 0;
    uint32_t wgcPostStopFrameDropMaxUs = 0;
    uint32_t wgcCoverageRepeatHoldCount = 0;
    uint32_t wgcCoverageDelayTicksCurrent = 0;
    double wgcAudioLeadExcessMsCurrent = 0.0;
    bool wgcCoverageRepeatActiveCurrent = false;
    bool encoderTooSlowForTargetCurrent = false;
    bool wgcLowSourceModeActive = false;
    bool wgcLiveRecoveryModeActive = false;
    bool wgcReservePressureActive = false;
    uint64_t wgcLowSourceStateChangeTick = 0;
    uint64_t wgcLiveRecoveryStateChangeTick = 0;
    bool wgcSourceStarvedCurrent = false;
    bool wgcSchedulerLimitedCurrent = false;
    bool wgcEncoderRecoveryLimitedCurrent = false;
    bool wgcActiveDelayRepeatClassKnown = false;
    ce::capture_policy::WgcActiveDelayWindowClass wgcActiveDelayLastRepeatClass =
        ce::capture_policy::WgcActiveDelayWindowClass::kHealthy;
    uint64_t wgcActiveDelayLastRepeatClassLogTick = 0;
    int64_t lastEmittedWgcSourceQpc = 0;
    // Selection-domain (smoothed) twin of lastEmittedWgcSourceQpc. The uniform
    // active-delay playout makes its emit/hold/drop decisions in the smoothed
    // selection-timestamp domain (strictly monotonic by construction), so its
    // monotonicity guard must compare in the same domain; raw stays in
    // lastEmittedWgcSourceQpc for the legacy path and sync diagnostics.
    int64_t lastEmittedWgcSelectionQpc = 0;
    int64_t lastEmittedInjectSourceQpc = 0;
    int64_t lastWarmupWgcSourceQpc = 0;
    int64_t wgcStartupBarrierQpc = 0;
    uint32_t wgcStartupBarrierDroppedFrames = 0;
    bool wgcStartupPreLiveDelayComplete = false;
    uint32_t wgcStartupPreLiveDelayDroppedFrames = 0;
    int64_t wgcAvSyncScheduleOffsetQpc = 0;
    int64_t wgcAvSyncStartupAudioAnchorQpc = 0;
    int64_t wgcAvSyncStartupVideoQpc = 0;
    int64_t wgcAvSyncStartupEffectiveDelayQpc = 0;
    int64_t wgcStartupReserveWaitStartQpc = 0;
    uint32_t wgcStartupReserveWaitCount = 0;
    bool wgcStartupHistoryProtectionLogged = false;
    int64_t wgcStartupReserveWaitInitialSpanUs = 0;
    uint32_t wgcStartupReserveWaitFreshenedMax = 0;
    uint32_t wgcFreshWarmupFrameCount = 0;
    uint32_t wgcOldestBufferedFrameAgeUs = 0;
    double wgcCoverageRepeatAccumulator = 0.0;
    struct WgcStarvedEpisodeSummary {
        bool active = false;
        uint64_t startTickMs = 0;
        int64_t startQpc = 0;
        uint64_t startLiveTicks = 0;
        uint64_t startDuplicateTicks = 0;
        uint32_t minInputFps = std::numeric_limits<uint32_t>::max();
        uint32_t minDeliveredFps = std::numeric_limits<uint32_t>::max();
        uint32_t peakFreshMissPermille = 0;
        uint32_t minBufferedFrames = std::numeric_limits<uint32_t>::max();
        uint32_t maxCallbackGapUs = 0;
        uint32_t maxCopyUs = 0;
        uint32_t maxFenceUs = 0;
        uint32_t maxMuxBackpressureCount = 0;
        uint32_t maxMuxBackpressureWaitUs = 0;
        uint32_t maxMuxQueueKb = 0;
        uint32_t peakOverloadFlags = 0;
        uint32_t startPoolSaturatedDrops = 0;
        uint32_t startPoolOverwritePrevented = 0;
        uint32_t startIngressDecimated = 0;
        double maxEncodeEmaMs = 0.0;

        void Reset() {
            *this = {};
            minInputFps = std::numeric_limits<uint32_t>::max();
            minDeliveredFps = std::numeric_limits<uint32_t>::max();
            minBufferedFrames = std::numeric_limits<uint32_t>::max();
        }
    };
    struct CaptureSessionSummary {
        uint64_t duplicateTicks = 0;
        uint64_t duplicateNoSourceTicks = 0;
        uint64_t duplicateDeferredTicks = 0;
        uint64_t duplicateTimerTicks = 0;
        uint64_t duplicateDrainTicks = 0;
        uint64_t queueTickSamples = 0;
        uint64_t noFreshTicks = 0;
        uint64_t noReserveTicks = 0;
        uint64_t starvedEpisodes = 0;
        uint64_t longestStarvedEpisodeMs = 0;
        uint64_t longestStarvedEpisodeOutputTicks = 0;
        uint64_t longestStarvedEpisodeDuplicateTicks = 0;
        // Longest CONTIGUOUS duplicate (held-frame) run for the whole session -- the true visible
        // freeze duration. Unlike the per-window cadence DupStreak, this survives the per-second
        // cadence reset, so a freeze that crosses a window boundary is not split/undercounted.
        // (longestStarvedEpisode* above measure a below-target *episode* and dups *within* it, which
        // overstate a freeze because the source still delivers new frames during the episode.)
        uint64_t longestContiguousDupTicks = 0;
        uint32_t currentContiguousDupTicks = 0;  // running counter (per-tick), reset on any fresh frame
        uint32_t longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
        uint32_t longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
        uint32_t worstFreshMissPermille = 0;
        uint32_t worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
        uint32_t bestSourceFpsX100 = 0;
        uint32_t worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
        uint32_t worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
        uint32_t worstSourceJitterUs = 0;
        uint32_t worstSelectionErrorUs = 0;
        uint32_t worstWgcSelectionErrorUs = 0;
        uint32_t worstOldestBufferedFrameAgeUs = 0;
        uint32_t worstOneSecondEmitCount = 0;
        uint32_t worstOneSecondUniqueCount = 0;
        uint32_t worstOneSecondRepeatCount = 0;
        uint32_t lowSourceImmediateExits = 0;
        double maxShortfallDurationMs = 0.0;
        double maxEncodeEmaMs = 0.0;
        double minEncoderSustainFps = std::numeric_limits<double>::max();
        uint32_t maxWgcContentPhaseErrorUs = 0;

        void Reset() {
            *this = {};
            longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
            longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
            worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
            worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
            worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
            minEncoderSustainFps = std::numeric_limits<double>::max();
        }
    };
    WgcStarvedEpisodeSummary wgcStarvedEpisode;
    wgcStarvedEpisode.Reset();
    CaptureSessionSummary captureSessionSummary;
    captureSessionSummary.Reset();
    const auto accumulateCaptureSummarySample =
        [&](bool useScreenGrabSession, uint32_t srcFpsX100Val, uint32_t srcJitterUsVal, uint32_t dupNoSource,
            uint32_t dupDeferred, uint32_t dupTimer, uint32_t dupDrain, uint32_t oldestBufferedFrameAgeUs,
            double shortfallDurationMs, double sustainableOutputFps) {
            captureSessionSummary.duplicateTicks += cadenceCounters.liveTickDuplicateCount;
            if (cadenceCounters.liveTickEmitCount > 0 &&
                (cadenceCounters.liveTickDuplicateCount > captureSessionSummary.worstOneSecondRepeatCount ||
                 (cadenceCounters.liveTickDuplicateCount == captureSessionSummary.worstOneSecondRepeatCount &&
                  (captureSessionSummary.worstOneSecondEmitCount == 0 ||
                   cadenceCounters.liveTickUniqueCount < captureSessionSummary.worstOneSecondUniqueCount)))) {
                captureSessionSummary.worstOneSecondEmitCount = cadenceCounters.liveTickEmitCount;
                captureSessionSummary.worstOneSecondUniqueCount = cadenceCounters.liveTickUniqueCount;
                captureSessionSummary.worstOneSecondRepeatCount = cadenceCounters.liveTickDuplicateCount;
            }
            captureSessionSummary.duplicateNoSourceTicks += dupNoSource - lastDuplicateReasonNoSource;
            captureSessionSummary.duplicateDeferredTicks += dupDeferred - lastDuplicateReasonDeferred;
            captureSessionSummary.duplicateTimerTicks += dupTimer - lastDuplicateReasonTimerRebase;
            captureSessionSummary.duplicateDrainTicks += dupDrain - lastDuplicateReasonDrain;
            captureSessionSummary.maxEncodeEmaMs = std::max(captureSessionSummary.maxEncodeEmaMs, smoothedEncodeMs);
            captureSessionSummary.minEncoderSustainFps =
                std::min(captureSessionSummary.minEncoderSustainFps, sustainableOutputFps);

            if (useScreenGrabSession) {
                captureSessionSummary.queueTickSamples += wgcQueueTickSampleCount;
                captureSessionSummary.noFreshTicks += wgcNoFreshTickCount;
                captureSessionSummary.noReserveTicks += wgcNoReserveTickCount;
                captureSessionSummary.worstFreshMissPermille =
                    std::max(captureSessionSummary.worstFreshMissPermille, wgcNoFreshTickPermille);
                if (srcFpsX100Val > 0) {
                    captureSessionSummary.worstSourceFpsX100 =
                        std::min(captureSessionSummary.worstSourceFpsX100, srcFpsX100Val);
                    captureSessionSummary.bestSourceFpsX100 =
                        std::max(captureSessionSummary.bestSourceFpsX100, srcFpsX100Val);
                }
                if (wgcRecentInputMin250Fps > 0) {
                    captureSessionSummary.worstInputMin250Fps =
                        std::min(captureSessionSummary.worstInputMin250Fps, wgcRecentInputMin250Fps);
                }
                if (wgcRecentDeliveredMin250Fps > 0) {
                    captureSessionSummary.worstDeliveredMin250Fps =
                        std::min(captureSessionSummary.worstDeliveredMin250Fps, wgcRecentDeliveredMin250Fps);
                }
                captureSessionSummary.worstSourceJitterUs =
                    std::max(captureSessionSummary.worstSourceJitterUs, srcJitterUsVal);
                captureSessionSummary.worstSelectionErrorUs =
                    std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
                captureSessionSummary.worstWgcSelectionErrorUs =
                    std::max(captureSessionSummary.worstWgcSelectionErrorUs, wgcSelectionErrorMaxUs);
                if (wgcSelectionErrorSamples > 0) {
                    const int64_t avgContentPhaseErrorUs =
                        wgcSelectionErrorSignedAccumUs / static_cast<int64_t>(wgcSelectionErrorSamples);
                    captureSessionSummary.maxWgcContentPhaseErrorUs =
                        std::max(captureSessionSummary.maxWgcContentPhaseErrorUs,
                                 SaturatingToUint32(static_cast<uint64_t>(
                                     avgContentPhaseErrorUs >= 0 ? avgContentPhaseErrorUs : -avgContentPhaseErrorUs)));
                }
                captureSessionSummary.worstOldestBufferedFrameAgeUs =
                    std::max(captureSessionSummary.worstOldestBufferedFrameAgeUs, oldestBufferedFrameAgeUs);
                captureSessionSummary.maxShortfallDurationMs =
                    std::max(captureSessionSummary.maxShortfallDurationMs, shortfallDurationMs);
            } else {
                captureSessionSummary.worstSelectionErrorUs =
                    std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
                injectWorstSelectionErrorUs =
                    std::max(injectWorstSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs);
                if (srcFpsX100Val > 0) {
                    injectWorstSourceFpsX100 = std::min(injectWorstSourceFpsX100, srcFpsX100Val);
                    injectBestSourceFpsX100 = std::max(injectBestSourceFpsX100, srcFpsX100Val);
                }
                injectWorstSourceJitterUs = std::max(injectWorstSourceJitterUs, srcJitterUsVal);
            }
        };
    const uint64_t minLoggedWgcStarvedEpisodeMs =
        static_cast<uint64_t>(std::llround(std::max(100.0, frameIntervalMs * 8.0)));
    const auto shouldLogWgcStarvedEpisode = [&](uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks,
                                                uint32_t peakFreshMissPermille) {
        if (duplicateTicks > 0 || durationMs >= minLoggedWgcStarvedEpisodeMs) {
            return true;
        }

        // Suppress single-tick/no-duplicate blips that can occur when the rolling
        // no-fresh telemetry briefly spikes without a visible cadence miss.
        return outputTicks > 1 && peakFreshMissPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille;
    };
    const auto finishWgcStarvedEpisode = [&](uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks) {
        ++captureSessionSummary.starvedEpisodes;
        const uint32_t minInputFps =
            wgcStarvedEpisode.minInputFps == std::numeric_limits<uint32_t>::max() ? 0u : wgcStarvedEpisode.minInputFps;
        const uint32_t minDeliveredFps = wgcStarvedEpisode.minDeliveredFps == std::numeric_limits<uint32_t>::max()
                                             ? 0u
                                             : wgcStarvedEpisode.minDeliveredFps;
        const uint32_t minBufferedFrames = wgcStarvedEpisode.minBufferedFrames == std::numeric_limits<uint32_t>::max()
                                               ? 0u
                                               : wgcStarvedEpisode.minBufferedFrames;
        const uint32_t targetOutputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        wgcStarvedEpisode.maxEncodeEmaMs = std::max(wgcStarvedEpisode.maxEncodeEmaMs, smoothedEncodeMs);
        wgcStarvedEpisode.maxFenceUs = std::max(
            wgcStarvedEpisode.maxFenceUs,
            SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, MediaEngine_GetLastFrameFenceWaitUs()))));
        if (media_main_g_WgcCap) {
            wgcStarvedEpisode.maxCallbackGapUs = std::max(
                wgcStarvedEpisode.maxCallbackGapUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, media_main_g_WgcCap->GetCallbackGapMaxUs()))));
            wgcStarvedEpisode.maxCopyUs = std::max(
                wgcStarvedEpisode.maxCopyUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, media_main_g_WgcCap->GetLastCopyTimeUs()))));
        }
        if (media_main_g_pSharedMem) {
            const auto& runtimeState = media_main_g_pSharedMem->runtimeState;
            const uint32_t muxQueueBytes = runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
            wgcStarvedEpisode.maxMuxQueueKb =
                std::max(wgcStarvedEpisode.maxMuxQueueKb, (muxQueueBytes + 1023u) / 1024u);
            wgcStarvedEpisode.maxMuxBackpressureCount =
                std::max(wgcStarvedEpisode.maxMuxBackpressureCount,
                         runtimeState.muxBackpressureCount.load(std::memory_order_relaxed));
            wgcStarvedEpisode.maxMuxBackpressureWaitUs =
                std::max(wgcStarvedEpisode.maxMuxBackpressureWaitUs,
                         runtimeState.muxBackpressureMaxWaitUs.load(std::memory_order_relaxed));
            wgcStarvedEpisode.peakOverloadFlags |= runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        }
        LARGE_INTEGER endQpc = {};
        QueryPerformanceCounter(&endQpc);
        const uint32_t frameBudgetUs = static_cast<uint32_t>(std::max(1.0, frameIntervalMs * 1000.0));
        const bool muxPressure =
            wgcStarvedEpisode.maxMuxBackpressureCount > 0 || wgcStarvedEpisode.maxMuxBackpressureWaitUs > 0;
        const bool capacityPressure = wgcStarvedEpisode.peakOverloadFlags != 0 || muxPressure;
        const bool sourceBelowCfrTarget = minInputFps > 0 && minInputFps < targetOutputFps;
        const bool deliveredBelowCfrTarget = minDeliveredFps > 0 && minDeliveredFps < targetOutputFps;
        const bool callbackDeliveryGap = wgcStarvedEpisode.maxCallbackGapUs > frameBudgetUs * 2u;
        uint32_t poolSaturatedDrops = 0;
        uint32_t poolOverwritePrevented = 0;
        uint32_t ingressDecimated = 0;
        if (media_main_g_WgcCap) {
            const uint32_t currentPoolSaturatedDrops = media_main_g_WgcCap->GetPoolSaturatedDropCount();
            const uint32_t currentPoolOverwritePrevented = media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
            const uint32_t currentIngressDecimated = media_main_g_WgcCap->GetIngressDecimatedCount();
            poolSaturatedDrops = currentPoolSaturatedDrops - wgcStarvedEpisode.startPoolSaturatedDrops;
            poolOverwritePrevented = currentPoolOverwritePrevented - wgcStarvedEpisode.startPoolOverwritePrevented;
            ingressDecimated = currentIngressDecimated - wgcStarvedEpisode.startIngressDecimated;
        }
        const bool wgcFramepoolPressure = poolSaturatedDrops > 0 || ingressDecimated > 0;
        const bool wgcDeliveryGap = deliveredBelowCfrTarget || callbackDeliveryGap;
        const char* faultHint = capacityPressure               ? "ce_capacity_pressure"
                                : wgcFramepoolPressure          ? "wgc_framepool_pressure"
                                : sourceBelowCfrTarget          ? "source_below_cfr_target"
                                : wgcDeliveryGap                ? "wgc_delivery_gap"
                                : poolOverwritePrevented > 0    ? "wgc_pool_lease_contention"
                                                               : "source_starved";
        const bool copySlow = wgcStarvedEpisode.maxCopyUs > frameBudgetUs;
        const bool fenceSlow = wgcStarvedEpisode.maxFenceUs > frameBudgetUs;
        if (durationMs >= captureSessionSummary.longestStarvedEpisodeMs) {
            captureSessionSummary.longestStarvedEpisodeMs = durationMs;
            captureSessionSummary.longestStarvedEpisodeOutputTicks = outputTicks;
            captureSessionSummary.longestStarvedEpisodeDuplicateTicks = duplicateTicks;
            captureSessionSummary.longestStarvedEpisodeMinInputFps = minInputFps;
            captureSessionSummary.longestStarvedEpisodeMinDeliveredFps = minDeliveredFps;
        }
        if (shouldLogWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks,
                                       wgcStarvedEpisode.peakFreshMissPermille)) {
            LogInfo(
                "[WGC CFR] Source-starved episode: duration=%llums out=%llu dup=%llu minIn=%u minDel=%u "
                "freshMiss=%upm minBuf=%u",
                static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
                static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
                wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames);
            LogInfo(
                "[WGC CFR ATTRIBUTION] fault_hint=%s qpc=%lld..%lld duration=%llums out=%llu dup=%llu "
                "minIn=%u minDel=%u freshMiss=%upm minBuf=%u cbGapMax=%uus encEmaMax=%.2fms "
                "muxBp=%u waitMax=%uus muxMax=%uKB overload=0x%X copyMax=%uus copyHealth=%s "
                "fenceMax=%uus fenceHealth=%s poolSat=%u overwritePrevented=%u ingressDecimated=%u",
                faultHint, static_cast<long long>(wgcStarvedEpisode.startQpc), static_cast<long long>(endQpc.QuadPart),
                static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
                static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
                wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames, wgcStarvedEpisode.maxCallbackGapUs,
                wgcStarvedEpisode.maxEncodeEmaMs, wgcStarvedEpisode.maxMuxBackpressureCount,
                wgcStarvedEpisode.maxMuxBackpressureWaitUs, wgcStarvedEpisode.maxMuxQueueKb,
                wgcStarvedEpisode.peakOverloadFlags, wgcStarvedEpisode.maxCopyUs, copySlow ? "slow" : "ok",
                wgcStarvedEpisode.maxFenceUs, fenceSlow ? "slow" : "ok", poolSaturatedDrops, poolOverwritePrevented,
                ingressDecimated);
        }
        wgcStarvedEpisode.Reset();
    };
    auto ClearBufferedInjectFrames = [&]() {
        while (!bufferedInjectFrames.empty()) {
            QueuedFrame queuedFrame = std::move(bufferedInjectFrames.front());
            bufferedInjectFrames.pop_front();
            DiscardQueuedFrame(queuedFrame);
        }
    };
    auto ClearBufferedWgcFrames = [&]() {
        while (!bufferedWgcFrames.empty()) {
            QueuedFrame queuedFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            ReleaseQueuedFrameTexture(queuedFrame);
        }
    };
    auto ResetWarmupWgcFreshness = [&](bool resetStartupDiagnostics = true) {
        lastWarmupWgcSourceQpc = 0;
        wgcStartupBarrierQpc = 0;
        wgcStartupBarrierDroppedFrames = 0;
        wgcStartupPreLiveDelayComplete = false;
        wgcStartupPreLiveDelayDroppedFrames = 0;
        wgcStartupReserveWaitStartQpc = 0;
        wgcStartupReserveWaitCount = 0;
        wgcStartupHistoryProtectionLogged = false;
        wgcStartupReserveWaitInitialSpanUs = 0;
        wgcStartupReserveWaitFreshenedMax = 0;
        wgcFreshWarmupFrameCount = 0;
        if (resetStartupDiagnostics) {
            wgcAvSyncScheduleOffsetQpc = 0;
            wgcAvSyncStartupAudioAnchorQpc = 0;
            wgcAvSyncStartupVideoQpc = 0;
            wgcAvSyncStartupEffectiveDelayQpc = 0;
            wgcSmoothnessActiveDelayQpc = 0;
            pendingWgcStartContract = {};
            pendingWgcStartContractGeneration = 0;
            committedWgcStartContractGeneration = 0;
            wgcEncoderPrewarmAttempted = false;
            wgcEncoderPrewarmSucceeded = false;
            wgcEncoderPrewarmElapsedUs = 0;
            wgcSmoothnessFloorDelayQpc = 0;
            wgcSmoothnessFloorRequestedQpc = 0;
            wgcSmoothnessFloorSource = "off";
            wgcSmoothnessFloorClampedBy = "none";
            wgcSmoothnessFloorLogged = false;
            wgcSmoothnessFloorJitter = ce::capture_policy::WgcSmoothnessFloorJitter{};
            wgcSmoothnessDesiredFrames = 0;
            wgcSmoothnessRetainedFrames = 0;
            wgcSmoothnessActualFrames = 0;
            wgcSmoothnessPoolSlots = 0;
            wgcSmoothnessRetainedFrameCap = 0;
            wgcSmoothnessReservedFreeSlots = 0;
            wgcSmoothnessEstimatedVramBytes = 0;
            wgcSmoothnessCapLimited = false;
            wgcSmoothnessBufferReason = "not-run";
        }
    };
    auto TrackWarmupWgcFreshFrame = [&](const QueuedFrame& queuedFrame) {
        if (queuedFrame.isInjectMode || queuedFrame.timestamp <= 0) {
            return;
        }
        if (queuedFrame.timestamp > lastWarmupWgcSourceQpc) {
            lastWarmupWgcSourceQpc = queuedFrame.timestamp;
            ++wgcFreshWarmupFrameCount;
        }
    };
    auto updateLiveCfrShortfall = [&](int64_t nowQpc) {
        if (config.video.useVFR || !recordingOutputLive || liveStartQpc.QuadPart <= 0 || targetIntervalTicks <= 0 ||
            nowQpc <= liveStartQpc.QuadPart) {
            liveTicksScheduled = 0;
            return 0u;
        }
        int64_t scheduledUntilQpc = nowQpc;
        if (!media_main_g_Recording.load(std::memory_order_acquire)) {
            const int64_t drainStopQpc = media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire);
            if (drainStopQpc > liveStartQpc.QuadPart) {
                scheduledUntilQpc = drainStopQpc;
            }
        }
        const uint64_t elapsedTicks = static_cast<uint64_t>(scheduledUntilQpc - liveStartQpc.QuadPart) /
                                      static_cast<uint64_t>(targetIntervalTicks);
        liveTicksScheduled = ce::capture_policy::GetCfrScheduledTicksForEndpoint(
            elapsedTicks, liveTicksDiscardedByTimerRebase, wgcVisualDebtMaxExcessTicks);
        return ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);
    };

    // A/V content delay: align video content with inherently-late loopback audio by biasing
    // WGC source-frame selection back by the loopback capture latency (= the slowest audio
    // source's latency, so faster sources can be equalized up to it). Audio samples and the
    // CFR PTS grid are untouched, so track length/start/end and zero-drift are preserved.
    // Video buffer self-builds/holds via the bounded "too new for slot" path. QPC ticks.
    float maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    const int64_t avContentDelayQpc =
        (maxAudioCaptureLatencyMs > 0.0f && qpcFreq.QuadPart > 0)
            ? static_cast<int64_t>(std::llround(static_cast<double>(maxAudioCaptureLatencyMs) / 1000.0 *
                                                static_cast<double>(qpcFreq.QuadPart)))
            : 0;
    const bool avContentDelayActive = avContentDelayQpc > 0;
    // Smoothness FLOOR (WGC only): engage the active-delay jitter-absorbing playout even when there
    // is no audio-latency content delay. Configured = auto or explicit > 0; only meaningful for the
    // Screen-grab smoothness can arm without an A/V delay; the floor magnitude is resolved once
    // at the startup barrier from measured delivery jitter.
    const bool wgcSmoothnessFloorConfigured = IsActiveScreenGrab() && config.wgcSmoothnessBufferEnabled &&
                                              !config.video.useVFR &&
                                              (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    const bool wgcSmoothnessDelayDesired =
        ce::capture_policy::WgcSmoothnessDelayDesired(avContentDelayActive, wgcSmoothnessFloorConfigured);
    // Inject delays content by retaining whole extra frames above its oldest-first reserve.
    const size_t injectContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0)
            ? static_cast<size_t>(std::ceil(static_cast<double>(maxAudioCaptureLatencyMs) / frameIntervalMs))
            : 0;
    const double injectResidualEstimateMs = (injectContentDelayFrames > 0 && frameIntervalMs > 0.0)
                                                ? static_cast<double>(injectContentDelayFrames) * frameIntervalMs -
                                                      static_cast<double>(maxAudioCaptureLatencyMs)
                                                : 0.0;
    const double avContentDelayFrames =
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

    const auto qpcToUs = [&](int64_t qpcDelta) -> int64_t {
        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
    };
    const auto observeCaptureSyncPhaseSource = [&](const char* path,
                                                    ce::capture_policy::CfrCadencePhaseLockState& state,
                                                    int64_t sourceTimestampQpc) {
        if (!captureSyncPhaseLockEnabled) {
            return;
        }
        const uint64_t releasesBefore = state.releases;
        ce::capture_policy::ObserveCfrCaptureSyncSourceTimestamp(state, sourceTimestampQpc,
                                                                 captureSyncSourceIntervalTicks);
        if (state.releases != releasesBefore) {
            LogInfo(
                "[CFR PhaseLock] path=%s state=released reason=variable_source stable=%u unstable=%u "
                "multiplier=%u releases=%llu",
                path, state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.releases));
        }
    };
    const auto applyCaptureSyncPhaseTarget = [&](const char* path,
                                                 ce::capture_policy::CfrCadencePhaseLockState& state,
                                                 int64_t baseTargetQpc, int64_t sourceReferenceQpc) -> int64_t {
        const uint64_t acquisitionsBefore = state.acquisitions;
        const uint64_t releasesBefore = state.releases;
        const uint64_t rephasesBefore = state.rephases;
        const int64_t adjustedTargetQpc = ce::capture_policy::ApplyCfrCaptureSyncPhaseLock(
            state, baseTargetQpc, sourceReferenceQpc, captureSyncSourceIntervalTicks,
            captureSyncPhaseLockEnabled);
        if (state.acquisitions != acquisitionsBefore || state.releases != releasesBefore ||
            state.rephases != rephasesBefore) {
            const char* transition = state.acquisitions != acquisitionsBefore
                                         ? "acquired"
                                         : (state.rephases != rephasesBefore ? "rephased" : "released");
            LogInfo(
                "[CFR PhaseLock] path=%s state=%s offset=%lldus stable=%u unstable=%u multiplier=%u "
                "transitions=%llu/%llu/%llu",
                path, transition, static_cast<long long>(qpcToUs(state.lockedPhaseQpc)),
                state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
                static_cast<unsigned long long>(state.acquisitions),
                static_cast<unsigned long long>(state.rephases), static_cast<unsigned long long>(state.releases));
        }
        return adjustedTargetQpc;
    };
    const auto getWgcRawSelectionTimestamp = [](const QueuedFrame& frame) -> int64_t {
        return frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;
    };
    const auto getWgcEffectiveContentDelayQpc = [&]() -> int64_t {
        return avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
    };
    const auto isWgcEffectiveContentDelayActive = [&]() -> bool { return getWgcEffectiveContentDelayQpc() > 0; };
    const auto getWgcSmoothnessOutputFps = [&]() -> uint32_t {
        return config.video.fps > 0 ? static_cast<uint32_t>(config.video.fps) : 0u;
    };
    const auto shouldUseWgcSmoothnessBaseConfig = [&]() -> bool {
        // Pass wgcSmoothnessDelayDesired (audio-latency delay OR configured floor) so the buffer
        // arms for video-only / low-confidence captures too, not only when audio latency is present.
        return ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                                wgcSmoothnessDelayDesired, targetIntervalTicks);
    };
    const auto getWgcSmoothnessDesiredFramesForConfig = [&]() -> uint32_t {
        if (!shouldUseWgcSmoothnessBaseConfig()) {
            return 0u;
        }
        return ce::capture_policy::GetWgcSmoothnessDesiredFrames(getWgcSmoothnessOutputFps(),
                                                                 config.wgcSmoothnessBufferMaxMs);
    };
    const auto getWgcSmoothnessRetainedFramesBudget = [&]() -> uint32_t {
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        return (media_main_g_WgcCap && desiredFrames > 0) ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
    };
    const auto isWgcSmoothnessSourceRateEligibleNow = [&]() -> bool {
        if (!ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                              wgcSmoothnessDelayDesired, targetIntervalTicks)) {
            return false;
        }
        const uint32_t inputMin250Fps = media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin250Fps() : wgcRecentInputMin250Fps;
        const uint32_t inputMin500Fps = media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin500Fps() : wgcRecentInputMin500Fps;
        return ce::capture_policy::ShouldArmWgcSmoothnessBufferForSourceRate(getWgcSmoothnessOutputFps(),
                                                                             inputMin250Fps, inputMin500Fps);
    };
    const auto shouldAttemptWgcStartupSmoothnessBufferNow = [&]() -> bool {
        return ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
            config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired, targetIntervalTicks,
            getWgcSmoothnessRetainedFramesBudget());
    };
    const auto getWgcStartupSmoothnessTargetDelayQpc = [&](bool attempted) -> int64_t {
        return attempted
                   ? ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                         getWgcSmoothnessRetainedFramesBudget(), targetIntervalTicks, getWgcSmoothnessOutputFps(),
                         config.wgcSmoothnessBufferMaxMs)
                   : 0;
    };
    const auto getWgcSmoothnessBufferReason = [&]() -> const char* {
        if (!config.wgcSmoothnessBufferEnabled) {
            return "disabled";
        }
        if (config.video.useVFR) {
            return "vfr";
        }
        if (!wgcSmoothnessDelayDesired) {
            return "sync_delay_inactive";
        }
        if (targetIntervalTicks <= 0) {
            return "invalid_target";
        }
        const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
        if (desiredFrames == 0) {
            return "target_zero";
        }
        const uint32_t retainedFrames = getWgcSmoothnessRetainedFramesBudget();
        if (retainedFrames == 0) {
            return "vram_budget_exhausted";
        }
        if (!isWgcSmoothnessSourceRateEligibleNow()) {
            return "startup_attempt_source_rate_low";
        }
        return "startup_attempt";
    };
    const auto getWgcDelayReservoirLowWaterFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirLowWaterFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirTargetFramesForDelay = [&](int64_t delayQpc) -> uint32_t {
        return ce::capture_policy::GetWgcDelayReservoirTargetFrames(delayQpc, targetIntervalTicks);
    };
    const auto getWgcDelayReservoirLowWaterFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirLowWaterFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcDelayReservoirTargetFrames = [&]() -> uint32_t {
        return getWgcDelayReservoirTargetFramesForDelay(getWgcEffectiveContentDelayQpc());
    };
    const auto getWgcRetainedFrameCap = [&]() -> uint32_t {
        if (!media_main_g_WgcCap) {
            return 0u;
        }
        return media_main_g_WgcCap->GetSmoothnessRetainedFrameCap();
    };
    const auto updateWgcIngressPressure = [&](const char*) {
        if (!media_main_g_WgcCap) {
            return;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t retainedFrames = SaturatingToUint32(
            static_cast<uint64_t>(bufferedWgcFrames.size()) +
            static_cast<uint64_t>(std::min<size_t>(media_main_g_FrameQueue.Size(), static_cast<size_t>(UINT32_MAX))));
        const uint32_t lowWaterFrames = getWgcDelayReservoirLowWaterFrames();
        const bool delayReservoirActive = lowWaterFrames > 0;
        const bool recovering = delayReservoirActive && (wgcLowSourceModeActive || wgcLiveRecoveryModeActive ||
                                                         (wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64()) ||
                                                         retainedFrames <= lowWaterFrames);
        const bool uniformPlayoutOwnsSurplus =
            isWgcEffectiveContentDelayActive() && config.wgcActiveDelayUniformCadence;
        media_main_g_WgcCap->SetRetainedFramePressure(retainedFrames, retainedCap, lowWaterFrames, recovering,
                                           uniformPlayoutOwnsSurplus);
    };
    const auto trimBufferedWgcToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 3 || nowTick - wgcRetainedCapTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir capped: trimmedNewest=%u reason=%s retained=%zu cap=%u "
                    "reservedFree=%u poolSlots=%u (pool safety protected; surplus source frames become planned CFR "
                    "decimation/repeats, audio/PTS unchanged)",
                    trimmed, reason ? reason : "unknown", bufferedWgcFrames.size(), retainedCap,
                    media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                    media_main_g_WgcCap ? media_main_g_WgcCap->GetTexturePoolSlotCount() : 0u);
                wgcRetainedCapTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcStartupWaitToRetainedCap = [&](const char* reason) {
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        if (retainedCap == 0) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > retainedCap) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            LogInfo(
                "[WGC CFR] startup wait retained reservoir capped: trimmedOldest=%u reason=%s retained=%zu cap=%u "
                "reservedFree=%u poolSlots=%u",
                trimmed, reason ? reason : "startup-wait", bufferedWgcFrames.size(), retainedCap,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
                media_main_g_WgcCap ? media_main_g_WgcCap->GetTexturePoolSlotCount() : 0u);
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto trimBufferedWgcForPoolPressure = [&](const char* reason) {
        if (!media_main_g_WgcCap) {
            updateWgcIngressPressure(reason);
            return 0u;
        }
        const uint32_t retainedCap = getWgcRetainedFrameCap();
        const uint32_t reservedFreeSlots = media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount();
        const uint32_t currentFreeSlots = media_main_g_WgcCap->GetPoolSlotFreeCurrentCount();
        const uint32_t trimTarget = ce::capture_policy::GetWgcPoolPressureRetainedTrimTarget(
            currentFreeSlots, reservedFreeSlots, getWgcDelayReservoirTargetFrames(), retainedCap);
        if (trimTarget == 0 || trimTarget >= retainedCap || bufferedWgcFrames.size() <= trimTarget) {
            updateWgcIngressPressure(reason);
            return 0u;
        }

        uint32_t trimmed = 0;
        while (bufferedWgcFrames.size() > trimTarget) {
            QueuedFrame surplus = std::move(bufferedWgcFrames.back());
            bufferedWgcFrames.pop_back();
            ReleaseQueuedFrameTexture(surplus);
            ++trimmed;
        }
        if (trimmed > 0) {
            wgcRetainedCapTrimTotal += trimmed;
            wgcRetainedCapTrimWindow += trimmed;
            wgcPoolPressureTrimTotal += trimmed;
            wgcPoolPressureTrimWindow += trimmed;
            const DWORD nowTick = GetTickCount();
            if (trimmed >= 2 || nowTick - wgcPoolPressureTrimLastLogTick >= 1000) {
                LogInfo(
                    "[WGC CFR] retained reservoir pressure trim: trimmedNewest=%u reason=%s retained=%zu "
                    "target=%u cap=%u free=%u reservedFree=%u poolSlots=%u delayTarget=%u "
                    "(preserved active-delay target; released surplus copy-pool leases, audio/PTS unchanged)",
                    trimmed, reason ? reason : "pool-pressure", bufferedWgcFrames.size(), trimTarget, retainedCap,
                    currentFreeSlots, reservedFreeSlots, media_main_g_WgcCap->GetTexturePoolSlotCount(),
                    getWgcDelayReservoirTargetFrames());
                wgcPoolPressureTrimLastLogTick = nowTick;
            }
        }
        updateWgcIngressPressure(reason);
        return trimmed;
    };
    const auto recordWgcDelayResidualSample =
        [&](int64_t signedResidualUs, uint64_t& samples, uint64_t& absAccumUs, int64_t& signedAccumUs,
            uint32_t& absMaxUs, uint32_t& lateMaxUs, uint32_t& earlyMaxUs, std::array<uint32_t, 256>& histogram,
            uint64_t& windowSamples, uint64_t& windowAbsAccumUs, int64_t& windowSignedAccumUs, uint32_t& windowAbsMaxUs,
            uint32_t& windowLateMaxUs, std::array<uint32_t, 256>& windowHistogram) {
            const uint32_t absResidualUs =
                SaturatingToUint32(static_cast<uint64_t>(signedResidualUs >= 0 ? signedResidualUs : -signedResidualUs));
            ++samples;
            absAccumUs += absResidualUs;
            signedAccumUs += signedResidualUs;
            absMaxUs = std::max(absMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                lateMaxUs = std::max(lateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            } else {
                earlyMaxUs = std::max(earlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedResidualUs)));
            }
            const size_t histogramBin = std::min<size_t>(histogram.size() - 1, absResidualUs / 1000u);
            ++histogram[histogramBin];
            ++windowSamples;
            windowAbsAccumUs += absResidualUs;
            windowSignedAccumUs += signedResidualUs;
            windowAbsMaxUs = std::max(windowAbsMaxUs, absResidualUs);
            if (signedResidualUs >= 0) {
                windowLateMaxUs =
                    std::max(windowLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
            }
            ++windowHistogram[histogramBin];
        };
    const auto recordWgcDelayRealization = [&](int64_t predictedSignedResidualUs, int64_t rawSignedResidualUs) -> bool {
        if (!isWgcEffectiveContentDelayActive() || qpcFreq.QuadPart <= 0) {
            return false;
        }
        const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
        const int64_t realizedDelaySignedUs = requestedDelayUs - predictedSignedResidualUs;
        const uint32_t realizedDelayUs =
            SaturatingToUint32(static_cast<uint64_t>(realizedDelaySignedUs > 0 ? realizedDelaySignedUs : 0));

        wgcDelayRealizedAccumUs += realizedDelayUs;
        wgcDelayRealizedMinUs = std::min(wgcDelayRealizedMinUs, realizedDelayUs);
        wgcDelayRealizedMaxUs = std::max(wgcDelayRealizedMaxUs, realizedDelayUs);
        recordWgcDelayResidualSample(predictedSignedResidualUs, wgcDelayResidualSamples, wgcDelayResidualAbsAccumUs,
                                     wgcDelayResidualSignedAccumUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualLateMaxUs,
                                     wgcDelayResidualEarlyMaxUs, wgcDelayResidualAbsHistogram,
                                     wgcDelayResidualWindowSamples, wgcDelayResidualWindowAbsAccumUs,
                                     wgcDelayResidualWindowSignedAccumUs, wgcDelayResidualWindowAbsMaxUs,
                                     wgcDelayResidualWindowLateMaxUs, wgcDelayResidualWindowAbsHistogram);
        recordWgcDelayResidualSample(rawSignedResidualUs, wgcDelayRawResidualSamples, wgcDelayRawResidualAbsAccumUs,
                                     wgcDelayRawResidualSignedAccumUs, wgcDelayRawResidualAbsMaxUs,
                                     wgcDelayRawResidualLateMaxUs, wgcDelayRawResidualEarlyMaxUs,
                                     wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualWindowSamples,
                                     wgcDelayRawResidualWindowAbsAccumUs, wgcDelayRawResidualWindowSignedAccumUs,
                                     wgcDelayRawResidualWindowAbsMaxUs, wgcDelayRawResidualWindowLateMaxUs,
                                     wgcDelayRawResidualWindowAbsHistogram);
        const int64_t rawMinusPredictedUs = rawSignedResidualUs - predictedSignedResidualUs;
        const uint32_t rawMinusPredictedAbsUs = SaturatingToUint32(
            static_cast<uint64_t>(rawMinusPredictedUs >= 0 ? rawMinusPredictedUs : -rawMinusPredictedUs));
        ++wgcDelayRawMinusPredictedSamples;
        wgcDelayRawMinusPredictedSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedAbsMaxUs = std::max(wgcDelayRawMinusPredictedAbsMaxUs, rawMinusPredictedAbsUs);
        ++wgcDelayRawMinusPredictedWindowSamples;
        wgcDelayRawMinusPredictedWindowSignedAccumUs += rawMinusPredictedUs;
        wgcDelayRawMinusPredictedWindowAbsMaxUs =
            std::max(wgcDelayRawMinusPredictedWindowAbsMaxUs, rawMinusPredictedAbsUs);
        return true;
    };
    const auto wgcDelayResidualHistogramP95Us = [](const std::array<uint32_t, 256>& histogram,
                                                   uint64_t samples) -> uint32_t {
        if (samples == 0) {
            return 0;
        }
        const uint64_t targetRank = (samples * 95ull + 99ull) / 100ull;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < histogram.size(); ++i) {
            cumulative += histogram[i];
            if (cumulative >= targetRank) {
                return SaturatingToUint32(static_cast<uint64_t>(i) * 1000ull);
            }
        }
        return SaturatingToUint32(static_cast<uint64_t>(histogram.size() - 1) * 1000ull);
    };
    const auto wgcDelayResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualAbsHistogram, wgcDelayResidualSamples);
    };
    const auto wgcDelayResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayResidualWindowAbsHistogram, wgcDelayResidualWindowSamples);
    };
    const auto wgcDelayRawResidualP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualSamples);
    };
    const auto wgcDelayRawResidualWindowP95Us = [&]() -> uint32_t {
        return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualWindowAbsHistogram, wgcDelayRawResidualWindowSamples);
    };

    while (media_main_g_EncoderRunning || media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) || media_main_g_FrameQueue.Size() > 0 ||
           !bufferedWgcFrames.empty() || !bufferedInjectFrames.empty()) {
        LARGE_INTEGER cycleStartQpc;
        const uint64_t cycleLiveTicksOutputStart = liveTicksOutput;
        // NOTE: cycleStartQpc is set after timer sleep below to measure
        // encode processing time, not the full loop including sleep.
        if (media_main_g_pSharedMem) {
            if (!media_main_g_Recording.load(std::memory_order_acquire)) {
                const auto phase = static_cast<CapturePipelinePhase>(
                    media_main_g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire));
                if (phase != CapturePipelinePhase::kCancelling) {
                    media_main_g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kDrain),
                                                                  std::memory_order_release);
                }
            } else if (recordingOutputLive) {
                media_main_g_pSharedMem->runtimeState.capturePhase.store(static_cast<uint32_t>(CapturePipelinePhase::kLive),
                                                              std::memory_order_release);
            }
        }
        static DWORD lastThreadLog = 0;
        if (GetTickCount() - lastThreadLog > 1000) {
            LogInfo(
                "[EncoderThread] Alive. Q=%u Bot=%d Rate=%.3f Credit=%.2f IBuf=%zu WBuf=%zu Grid=%lld Live=%d "
                "EMA=%u Fence=%.2fms Encode=%.2fms",
                (unsigned int)media_main_g_FrameQueue.Size(), (int)media_main_g_IsEncoderBottlenecked, smoothedInputPerTick,
                frameCreditAccumulator, bufferedInjectFrames.size(), bufferedWgcFrames.size(),
                static_cast<long long>(encoderGridTickCount), (int)recordingOutputLive, pacingEmaUpdates,
                smoothedInjectFenceMs, smoothedEncodeMs);
            lastThreadLog = GetTickCount();
        }

        if (media_main_g_pSharedMem) {
            UpdateAtomicPeak(media_main_g_pSharedMem->runtimeState.bufferedInjectDepthPeak,
                             static_cast<uint32_t>(bufferedInjectFrames.size()));
        }

        if (media_main_g_pSharedMem) {
            uint32_t queueDepth = (uint32_t)media_main_g_FrameQueue.Size();
            queueDepth += static_cast<uint32_t>(bufferedInjectFrames.size());
            queueDepth += static_cast<uint32_t>(bufferedWgcFrames.size());
            double fenceWaitMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
            const uint32_t queuePressureThreshold =
                std::max<uint32_t>(8u, static_cast<uint32_t>(media_main_g_FrameQueue.Capacity() / 2));
            bool shouldThrottle = queueDepth >= queuePressureThreshold || fenceWaitMs > 16.0;

            media_main_g_pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
            media_main_g_pSharedMem->throttleCapture.store(shouldThrottle, std::memory_order_release);
            media_main_g_pSharedMem->runtimeState.hostDroppedFrames.store(static_cast<uint32_t>(media_main_g_FrameQueue.GetDroppedCount()));
            UpdateAtomicPeak(media_main_g_pSharedMem->runtimeState.encoderQueuePeakDepth, queueDepth);

            int64_t oldestBufferedTimestamp = 0;
            if (!bufferedInjectFrames.empty()) {
                oldestBufferedTimestamp = bufferedInjectFrames.front().timestamp;
            } else if (!bufferedWgcFrames.empty()) {
                oldestBufferedTimestamp = bufferedWgcFrames.front().timestamp;
            }
            if (oldestBufferedTimestamp > 0) {
                LARGE_INTEGER nowQpc;
                QueryPerformanceCounter(&nowQpc);
                uint64_t oldestAgeUs = 0;
                if (nowQpc.QuadPart > oldestBufferedTimestamp) {
                    oldestAgeUs =
                        static_cast<uint64_t>((nowQpc.QuadPart - oldestBufferedTimestamp) * 1000000 / qpcFreq.QuadPart);
                }
                wgcOldestBufferedFrameAgeUs = SaturatingToUint32(oldestAgeUs);
                media_main_g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(wgcOldestBufferedFrameAgeUs,
                                                                          std::memory_order_relaxed);
            } else {
                wgcOldestBufferedFrameAgeUs = 0;
                media_main_g_pSharedMem->runtimeState.oldestBufferedFrameAgeUs.store(0, std::memory_order_relaxed);
            }
        }

        uint32_t outputShortfallTicks = 0;
        const bool activeScreenGrab = IsActiveScreenGrab();
        const bool useScreenGrab = activeScreenGrab;
        const uint64_t currentWgcSourceEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
        if (activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            size_t bufferedDiscarded = 0;
            for (auto it = bufferedWgcFrames.begin(); it != bufferedWgcFrames.end();) {
                if (it->wgcSourceEpoch != currentWgcSourceEpoch) {
                    ReleaseQueuedFrameTexture(*it);
                    it = bufferedWgcFrames.erase(it);
                    ++bufferedDiscarded;
                } else {
                    ++it;
                }
            }
            const size_t queuedDiscarded = media_main_g_FrameQueue.DiscardWgcEpochNotEqual(currentWgcSourceEpoch);
            if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode && media_main_g_LastFrame.wgcSourceEpoch != currentWgcSourceEpoch) {
                ResetLastQueuedFrameCache();
            }
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            lastEmittedWgcSourceQpc = 0;
            lastEmittedWgcSelectionQpc = 0;
            lastWarmupWgcSourceQpc = 0;
            wgcInputPredictor.Reset();
            wgcCfrPhaseLock.Reset();
            wgcRecentDeliveredFps = 0;
            wgcRecentDeliveredMin250Fps = 0;
            wgcRecentDeliveredMin500Fps = 0;
            wgcRecentInputMin250Fps = 0;
            wgcRecentInputMin500Fps = 0;
            wgcLowSourceModeActive = false;
            wgcLiveRecoveryModeActive = false;
            wgcSourceStarvedCurrent = false;
            smoothedWgcFreshServiceMs = 0.0;
            smoothedWgcRepeatServiceMs = 0.0;
            wgcFreshServiceSamples = wgcRepeatServiceSamples = 0;
            wgcOverloadRepeatPacer.ResetActivePacing();
            lastSuccessfulWgcCursorEmbedded = false;
            hasSuccessfulWgcCursorMetadata = false;
            privacyRuntime.ResetSource();
            ResetDuplicationCursorSuppression("WGC source epoch change");
            LogInfo(
                "[EncoderThread] WGC source epoch changed: epoch=%llu bufferedDiscarded=%zu queuedDiscarded=%zu; "
                "selection/cursor lineage rebased without changing the audio or CFR timeline",
                static_cast<unsigned long long>(currentWgcSourceEpoch), bufferedDiscarded, queuedDiscarded);
        } else if (!activeScreenGrab && currentWgcSourceEpoch != observedWgcSourceEpoch) {
            // Standby WGC retargets are unrelated to the authoritative inject
            // pixels. Observe their publication epoch now so activating the
            // already-proven standby source does not later invalidate the
            // inject repeat fallback at the handoff boundary.
            observedWgcSourceEpoch = currentWgcSourceEpoch;
            LogInfo("[EncoderThread] Observed standby WGC source epoch %llu while inject remained active",
                    static_cast<unsigned long long>(currentWgcSourceEpoch));
        }
        const uint32_t outputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
        auto loadEncoderOverloadFlags = [&]() -> uint32_t {
            return media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;
        };
        auto isWgcCapacityPressureActive = [&]() -> bool {
            const uint32_t overloadFlags = loadEncoderOverloadFlags();
            return media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) || encoderTooSlowForTargetCurrent ||
                   (overloadFlags & (ce::capture_policy::kEncoderOverloadFlagEncoder |
                                     ce::capture_policy::kEncoderOverloadFlagMux)) != 0;
        };
        auto isWgcTrueSourceStarvedForCapacityPolicy = [&]() -> bool {
            return ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
                outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                isWgcCapacityPressureActive());
        };
        auto isWgcEncoderLimitedSmoothnessMode = [&]() -> bool {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive) {
                return false;
            }
            if (wgcSourceStarvedCurrent || isWgcTrueSourceStarvedForCapacityPolicy()) {
                return false;
            }
            const uint32_t bufferedWgcFrameCount =
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
            if (!ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                    bufferedWgcFrameCount)) {
                return false;
            }
            return ce::capture_policy::IsWgcEncoderLimitedSmoothnessMode(
                media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), encoderTooSlowForTargetCurrent,
                loadEncoderOverloadFlags());
        };
        if (!media_main_g_EncoderRunning && !media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) && activeScreenGrab) {
            const size_t bufferedDiscarded = bufferedWgcFrames.size();
            const size_t bufferedInjectDiscarded = bufferedInjectFrames.size();
            ClearBufferedWgcFrames();
            ClearBufferedInjectFrames();
            size_t queuedDiscarded = 0;
            QueuedFrame queuedFrame;
            while (media_main_g_FrameQueue.Pop(queuedFrame, 0)) {
                DiscardQueuedFrame(queuedFrame);
                ++queuedDiscarded;
            }
            if (bufferedDiscarded > 0 || bufferedInjectDiscarded > 0 || queuedDiscarded > 0) {
                LogInfo(
                    "[EncoderThread] WGC CFR exact-stop discarded pending frames: queued=%zu bufferedWgc=%zu "
                    "bufferedInject=%zu. "
                    "No post-stop CFR drain will be encoded.",
                    queuedDiscarded, bufferedDiscarded, bufferedInjectDiscarded);
            }
            break;
        }
        auto dropWgcVisualTimelineDebtToLiveWindow = [&](const char* reason) -> uint32_t {
            if (!activeScreenGrab || config.video.useVFR || !recordingOutputLive || targetIntervalTicks <= 0 ||
                qpcFreq.QuadPart <= 0) {
                return 0;
            }

            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const uint32_t maxDebtTicks = ce::capture_policy::GetWgcLiveVisualDebtLimitTicksForMode(
                targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (maxDebtTicks == 0 || outputShortfallTicks <= maxDebtTicks) {
                return 0;
            }

            const uint32_t excessTicks = outputShortfallTicks - maxDebtTicks;
            wgcVisualDebtMaxExcessTicks = std::max<uint64_t>(wgcVisualDebtMaxExcessTicks, excessTicks);

            static uint64_t s_lastWgcTimelineDebtDropLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (nowTick - s_lastWgcTimelineDebtDropLogTick >= 1000) {
                LogWarn(
                    "[EncoderThread] WGC CFR visual timeline debt drop: reason=%s mode=%s excessTicks=%u "
                    "maxDebtTicks=%u maxExcessTicks=%llu shortfall=%u",
                    reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                    excessTicks, maxDebtTicks, static_cast<unsigned long long>(wgcVisualDebtMaxExcessTicks),
                    outputShortfallTicks);
                s_lastWgcTimelineDebtDropLogTick = nowTick;
            }
            return excessTicks;
        };
        auto pruneStaleWgcVisualDebt = [&](int64_t liveNowQpc, const char* reason, bool allowDropAll,
                                           int64_t immutableSelectionTargetQpc) -> size_t {
            if (wgcWarmupUntilQpc > 0 && liveNowQpc < wgcWarmupUntilQpc) {
                return 0;
            }
            if (outputShortfallTicks > 0 && immutableSelectionTargetQpc <= 0) {
                return 0;
            }
            const bool encoderLimitedSmoothnessMode = isWgcEncoderLimitedSmoothnessMode();
            const bool startupSmoothnessAttempted = shouldAttemptWgcStartupSmoothnessBufferNow();
            const int64_t startupSmoothnessTargetQpc = getWgcStartupSmoothnessTargetDelayQpc(startupSmoothnessAttempted);
            const int64_t liveVisualDebtLimitQpc = ce::capture_policy::GetWgcLiveVisualDebtLimitQpcForMode(
                targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode);
            if (ce::capture_policy::ShouldProtectWgcStartupSmoothnessHistory(
                    recordingOutputLive, startupSmoothnessAttempted, startupSmoothnessTargetQpc,
                    liveVisualDebtLimitQpc)) {
                if (!wgcStartupHistoryProtectionLogged) {
                    wgcStartupHistoryProtectionLogged = true;
                    LogInfo("[EncoderThread] WGC startup history protected from the shallower live-debt window");
                }
                return 0;
            }
            const int64_t intentionalContentDelayQpc = getWgcEffectiveContentDelayQpc();
            const int64_t visualDebtFloorQpc = ce::capture_policy::GetWgcLiveVisualDebtFloorQpcForMode(
                liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, encoderLimitedSmoothnessMode,
                intentionalContentDelayQpc);
            if (visualDebtFloorQpc <= 0) {
                return 0;
            }

            size_t dropped = 0;
            uint64_t maxDebtUs = 0;
            while (!bufferedWgcFrames.empty()) {
                const int64_t selectionTimestampQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                const int64_t nextSelectionTimestampQpc =
                    bufferedWgcFrames.size() > 1 ? GetFrameSelectionTimestamp(bufferedWgcFrames[1]) : 0;
                if (!ce::capture_policy::ShouldPruneWgcVisualDebtFrameForGrid(
                        selectionTimestampQpc, nextSelectionTimestampQpc, visualDebtFloorQpc,
                        immutableSelectionTargetQpc)) {
                    break;
                }
                if (bufferedWgcFrames.size() == 1 && !allowDropAll) {
                    break;
                }

                if (qpcFreq.QuadPart > 0) {
                    maxDebtUs = std::max<uint64_t>(
                        maxDebtUs, static_cast<uint64_t>((visualDebtFloorQpc - selectionTimestampQpc) * 1000000 /
                                                         qpcFreq.QuadPart));
                }
                QueuedFrame stale = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(stale);
                ++dropped;
                ++wgcDropStaleDebtCount;
                ++wgcDropStaleDebtTotal;
            }

            if (dropped > 0) {
                wgcDropStaleDebtMaxUs = std::max(wgcDropStaleDebtMaxUs, SaturatingToUint32(maxDebtUs));
                static uint64_t s_lastStaleWgcDebtLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastStaleWgcDebtLogTick >= 1000 || dropped >= 8) {
                    LogWarn(
                        "[EncoderThread] WGC CFR stale visual debt drop: reason=%s mode=%s dropped=%zu floorQpc=%lld "
                        "gridTargetQpc=%lld liveNowQpc=%lld contentDelay=%lldus maxDebt=%lluus remaining=%zu shortfall=%u",
                        reason ? reason : "unknown", encoderLimitedSmoothnessMode ? "encoder_limited" : "bounded_live",
                        dropped, static_cast<long long>(visualDebtFloorQpc),
                        static_cast<long long>(immutableSelectionTargetQpc), static_cast<long long>(liveNowQpc),
                        static_cast<long long>(qpcToUs(intentionalContentDelayQpc)),
                        static_cast<unsigned long long>(maxDebtUs), bufferedWgcFrames.size(), outputShortfallTicks);
                    s_lastStaleWgcDebtLogTick = nowTick;
                }
            }
            return dropped;
        };
        auto noteActivePathMismatchDiscard = [&](bool frameIsInjectMode, const char* source) {
            ++activePathMismatchDiscardThisWindow;
            ++activePathMismatchDiscardTotal;
            const uint64_t discarded = media_main_g_ActivePathMismatchFramesDiscarded.fetch_add(1, std::memory_order_relaxed) + 1;
            if (activePathMismatchDiscardThisWindow <= 3 || (discarded % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] Discarded %s frame on active %s path from %s (window=%u total=%llu). Preventing "
                    "mid-recording encoder mode switch.",
                    frameIsInjectMode ? "inject" : "WGC/D3D11", useScreenGrab ? "WGC" : "inject", source,
                    activePathMismatchDiscardThisWindow, static_cast<unsigned long long>(discarded));
            }
        };
        auto discardActivePathMismatchFrame = [&](QueuedFrame& mismatchedFrame, const char* source, bool queuedFrame) {

            noteActivePathMismatchDiscard(mismatchedFrame.isInjectMode, source);
            if (queuedFrame) {
                DiscardQueuedFrame(mismatchedFrame);
            } else {
                if (!mismatchedFrame.isInjectMode) {
                    ReleaseQueuedFrameTexture(mismatchedFrame);
                }
                mismatchedFrame = QueuedFrame{};
            }
        };
        if (!config.video.useVFR && recordingOutputLive) {
            LARGE_INTEGER shortfallNow;
            QueryPerformanceCounter(&shortfallNow);
            outputShortfallTicks = updateLiveCfrShortfall(shortfallNow.QuadPart);
            dropWgcVisualTimelineDebtToLiveWindow(media_main_g_Recording.load(std::memory_order_acquire) ? "live" : "drain");
        }
        const bool recordingActive = media_main_g_Recording.load(std::memory_order_acquire);
        const bool drainOutstandingCfrTicks = media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire);
        if (ce::capture_policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(recordingActive, recordingOutputLive,
                                                                          drainOutstandingCfrTicks)) {
            LogWarn(
                "[EncoderThread] CFR stop drain skipped before first live video frame; no output timeline or "
                "captured frame exists to drain");
            media_main_g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
        }
        if (!recordingActive && recordingOutputLive && drainOutstandingCfrTicks) {
            const bool mediaEngineCanRepeatLastFrame =
                MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (activeScreenGrab && !bufferedWgcFrames.empty()) {
                int64_t drainPolicyQpc = media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire);
                if (drainPolicyQpc <= 0) {
                    LARGE_INTEGER drainNowQpc;
                    QueryPerformanceCounter(&drainNowQpc);
                    drainPolicyQpc = drainNowQpc.QuadPart;
                }
                pruneStaleWgcVisualDebt(drainPolicyQpc, "stop-drain",
                                        media_main_g_HasLastFrame && mediaEngineCanRepeatLastFrame, 0);
            }
            const bool bufferedFrameAvailable =
                activeScreenGrab ? !bufferedWgcFrames.empty() : !bufferedInjectFrames.empty();
            const size_t bufferedFrameCount = activeScreenGrab ? bufferedWgcFrames.size() : bufferedInjectFrames.size();
            const bool canDrainOutstandingTicks = ce::capture_policy::CanDrainOutstandingCfrTicks(
                activeScreenGrab, media_main_g_FrameQueue.Size() > 0, bufferedFrameAvailable, media_main_g_HasLastFrame,
                mediaEngineCanRepeatLastFrame);
            static uint64_t s_lastStopDrainProgressLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (outputShortfallTicks > 0 && nowTick - s_lastStopDrainProgressLogTick >= 5000) {
                LogInfo(
                    "[EncoderThread] CFR stop drain progress: shortfall=%u/%.1fms queue=%u buffered=%zu hostLast=%d "
                    "cachedRepeat=%d",
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    static_cast<unsigned>(media_main_g_FrameQueue.Size()), bufferedFrameCount, media_main_g_HasLastFrame ? 1 : 0,
                    mediaEngineCanRepeatLastFrame ? 1 : 0);
                s_lastStopDrainProgressLogTick = nowTick;
            }
            if (activeScreenGrab && outputShortfallTicks > 0 && !bufferedFrameAvailable && media_main_g_FrameQueue.Size() == 0 &&
                media_main_g_HasLastFrame && mediaEngineCanRepeatLastFrame && !wgcStopDrainHeldFrameLogged) {
                LogWarn(
                    "[EncoderThread] WGC CFR stop drain using held pre-stop frame: holdTicks=%u/%.1fms "
                    "queued=0 buffered=0. Audio endpoint is preserved; this is visual hold debt, not audio recovery.",
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs));
                wgcStopDrainHeldFrameLogged = true;
            }
            if (outputShortfallTicks == 0 || !canDrainOutstandingTicks) {
                if (outputShortfallTicks == 0) {
                    LogInfo("[EncoderThread] CFR stop drain complete: scheduled=%llu output=%llu",
                            static_cast<unsigned long long>(liveTicksScheduled),
                            static_cast<unsigned long long>(liveTicksOutput));
                } else {
                    LogWarn(
                        "[EncoderThread] CFR stop drain aborted: no captured frame/repeat available for outstanding "
                        "shortfall=%u/%.1fms "
                        "(queue=%u buffered=%zu hostLast=%d cachedRepeat=%d; cached repeats close only accrued "
                        "CFR debt)",
                        outputShortfallTicks,
                        ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                        static_cast<unsigned>(media_main_g_FrameQueue.Size()), bufferedFrameCount, media_main_g_HasLastFrame ? 1 : 0,
                        mediaEngineCanRepeatLastFrame ? 1 : 0);
                }
                s_lastStopDrainProgressLogTick = 0;
                media_main_g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
            }
        }

        const bool frameAvailableForCatchup =
            activeScreenGrab ? (!bufferedWgcFrames.empty()) : (!bufferedInjectFrames.empty());
        bool shouldCatchUpToWallClock = false;
        uint32_t catchupTicksThisLoop = 0;
        const auto loadWgcAudioLeadExcessMs = [&]() -> double {
            if (!media_main_g_pSharedMem) {
                return 0.0;
            }
            const uint32_t audioLeadExcessSamples =
                media_main_g_pSharedMem->runtimeState.wgcAudioLeadExcessSamples.load(std::memory_order_relaxed);
            return static_cast<double>(audioLeadExcessSamples) * 1000.0 / 48000.0;
        };
        const auto computeWgcCoverageRepeatActive = [&](double audioLeadExcessMs) {
            if (!activeScreenGrab || !recordingOutputLive) {
                return false;
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            const double oldestBufferedFrameAgeMs = static_cast<double>(wgcOldestBufferedFrameAgeUs) / 1000.0;
            uint32_t effectiveDeliveredFps = wgcRecentDeliveredFps;
            if (wgcRecentDeliveredMin250Fps > 0) {
                effectiveDeliveredFps = std::min(effectiveDeliveredFps, wgcRecentDeliveredMin250Fps);
            }
            if (wgcRecentDeliveredMin500Fps > 0) {
                effectiveDeliveredFps = std::min(effectiveDeliveredFps, wgcRecentDeliveredMin500Fps);
            }
            if (ce::capture_policy::ShouldSuppressWgcCoverageLossForEncoderBottleneck(
                    media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), effectiveDeliveredFps,
                    std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps > 0 ? config.video.fps : 1)))) {
                return false;
            }
            return ce::capture_policy::HasWgcUnrecoverableCoverageLoss(shortfallDurationMs, oldestBufferedFrameAgeMs,
                                                                       audioLeadExcessMs);
        };
        auto recomputeCatchupPolicy = [&]() {
            const uint32_t targetOutputFpsForPolicy = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
            encoderTooSlowForTargetCurrent = ce::capture_policy::IsEncoderTooSlowForTargetFps(
                smoothedEncodeMs, frameIntervalMs, targetOutputFpsForPolicy);
            injectEncoderServiceTooSlowCurrent = ce::capture_policy::IsEncoderTooSlowForTargetFps(
                std::max(smoothedEncodeMs, smoothedInjectServiceMs), frameIntervalMs, targetOutputFpsForPolicy);
            const bool encoderCatchupBottleneckedCurrent =
                encoderTooSlowForTargetCurrent || media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
            const bool nextInjectCfrRecoveryActive = ce::capture_policy::GetInjectCfrRecoveryActive(
                injectCfrRecoveryActive, recordingOutputLive && !activeScreenGrab, config.video.useVFR,
                outputShortfallTicks);
            if (nextInjectCfrRecoveryActive != injectCfrRecoveryActive) {
                const uint64_t transitionTick = GetTickCount64();
                const bool recoveryEntering = nextInjectCfrRecoveryActive;
                const uint64_t recoveryDurationMs =
                    !recoveryEntering && injectCfrRecoveryStartTick > 0 ? transitionTick - injectCfrRecoveryStartTick
                                                                        : 0;
                const uint64_t recoveryFreshCatchup =
                    !recoveryEntering ? injectFreshCatchupTotal - injectCfrRecoveryStartFreshCatchup : 0;
                const uint64_t recoveryRepeatCatchup =
                    !recoveryEntering ? injectRepeatCatchupTotal - injectCfrRecoveryStartRepeatCatchup : 0;
                injectCfrRecoveryActive = nextInjectCfrRecoveryActive;
                if (injectCfrRecoveryActive) {
                    ++injectCfrRecoveryEpisodesThisWindow;
                    ++injectCfrRecoveryEpisodesTotal;
                    injectCfrRecoveryStartTick = transitionTick;
                    injectCfrRecoveryStartDebt = outputShortfallTicks;
                    injectCfrRecoveryBestDebt = outputShortfallTicks;
                    injectCfrRecoveryStartFreshCatchup = injectFreshCatchupTotal;
                    injectCfrRecoveryStartRepeatCatchup = injectRepeatCatchupTotal;
                    injectCfrRecoveryLastProgressLogTick = transitionTick;
                }
                LogInfo(
                    "[Inject CFR] Recovery %s: shortfall=%u/%.1fms startDebt=%u bestDebt=%u duration=%llums "
                    "fresh=%llu repeat=%llu enc=%.2fms service=%.2fms cycle=%.2fms bottleneck=%d. "
                    "exitDebt=%u tick(s)",
                    injectCfrRecoveryActive
                        ? "entered"
                        : (outputShortfallTicks <= ce::capture_policy::kInjectCfrRecoveryExitShortfallTicks
                               ? "completed"
                               : "disarmed"),
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    recoveryEntering ? outputShortfallTicks : injectCfrRecoveryStartDebt,
                    recoveryEntering ? outputShortfallTicks : injectCfrRecoveryBestDebt,
                    static_cast<unsigned long long>(recoveryDurationMs),
                    static_cast<unsigned long long>(recoveryFreshCatchup),
                    static_cast<unsigned long long>(recoveryRepeatCatchup), smoothedEncodeMs,
                    smoothedInjectServiceMs, smoothedEncCycleMs,
                    media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                    ce::capture_policy::kInjectCfrRecoveryExitShortfallTicks);
            }
            if (injectCfrRecoveryActive) {
                injectCfrRecoveryBestDebt = std::min(injectCfrRecoveryBestDebt, outputShortfallTicks);
                const uint64_t recoveryNowTick = GetTickCount64();
                if (injectCfrRecoveryStartTick > 0 && recoveryNowTick - injectCfrRecoveryStartTick >= 5000 &&
                    recoveryNowTick - injectCfrRecoveryLastProgressLogTick >= 5000) {
                    LogWarn(
                        "[Inject CFR] Recovery still active: duration=%llums debt=%u start=%u best=%u "
                        "fresh=%llu repeat=%llu enc=%.2fms service=%.2fms cycle=%.2fms buffered=%zu credit=%.2f "
                        "bottleneck=%d serviceSlow=%d",
                        static_cast<unsigned long long>(recoveryNowTick - injectCfrRecoveryStartTick),
                        outputShortfallTicks, injectCfrRecoveryStartDebt, injectCfrRecoveryBestDebt,
                        static_cast<unsigned long long>(injectFreshCatchupTotal - injectCfrRecoveryStartFreshCatchup),
                        static_cast<unsigned long long>(injectRepeatCatchupTotal - injectCfrRecoveryStartRepeatCatchup),
                        smoothedEncodeMs, smoothedInjectServiceMs, smoothedEncCycleMs, bufferedInjectFrames.size(),
                        frameCreditAccumulator,
                        media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                        injectEncoderServiceTooSlowCurrent ? 1 : 0);
                    injectCfrRecoveryLastProgressLogTick = recoveryNowTick;
                }
            }
            const double shortfallDurationMs =
                ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs);
            wgcAudioLeadExcessMsCurrent = loadWgcAudioLeadExcessMs();
            wgcCoverageRepeatActiveCurrent = computeWgcCoverageRepeatActive(wgcAudioLeadExcessMsCurrent);
            shouldCatchUpToWallClock =
                !config.video.useVFR && recordingOutputLive &&
                ce::capture_policy::ShouldCfrCatchUpToWallClock(outputShortfallTicks, activeScreenGrab,
                                                                frameAvailableForCatchup, media_main_g_HasLastFrame);
            if (!shouldCatchUpToWallClock) {
                catchupTicksThisLoop = 0u;
            } else if (activeScreenGrab) {
                catchupTicksThisLoop = ce::capture_policy::GetWgcCatchupTicksThisLoop(
                    encoderCatchupBottleneckedCurrent, encoderTooSlowForTargetCurrent, bufferedWgcFrames.size(),
                    frameCreditAccumulator, outputShortfallTicks, targetOutputFpsForPolicy, wgcRecentDeliveredMin250Fps,
                    wgcRecentInputMin250Fps, wgcNoFreshTickPermille, wgcLowSourceModeActive,
                    wgcAudioLeadExcessMsCurrent);
            } else {
                catchupTicksThisLoop = ce::capture_policy::GetInjectCfrCatchupTicksThisLoop(
                    outputShortfallTicks, injectCfrRecoveryActive,
                    encoderCatchupBottleneckedCurrent || injectEncoderServiceTooSlowCurrent);
            }
            if (activeScreenGrab &&
                ce::capture_policy::ShouldClampWgcCoverageCatchupToSingleTick(
                    wgcCoverageRepeatActiveCurrent, encoderCatchupBottleneckedCurrent, shortfallDurationMs)) {
                catchupTicksThisLoop = std::min<uint32_t>(catchupTicksThisLoop, 1u);
            }
            if (activeScreenGrab && catchupTicksThisLoop > 1u) {
                static uint64_t s_lastWgcRepeatCatchupLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastWgcRepeatCatchupLogTick >= 5000) {
                    LogInfo(
                        "[EncoderThread] WGC CFR repeat catch-up armed: shortfall=%u/%.1fms ticksThisLoop=%u "
                        "audioLeadExcess=%.1fms encoderBottleneck=%d encoderTooSlow=%d buffered=%zu",
                        outputShortfallTicks, shortfallDurationMs, catchupTicksThisLoop, wgcAudioLeadExcessMsCurrent,
                        encoderCatchupBottleneckedCurrent ? 1 : 0, encoderTooSlowForTargetCurrent ? 1 : 0,
                        bufferedWgcFrames.size());
                    s_lastWgcRepeatCatchupLogTick = nowTick;
                }
            }
        };
        recomputeCatchupPolicy();

        const int64_t selectionGridTick =
            (!config.video.useVFR && recordingOutputLive) ? (encoderGridTickCount + 1) : encoderGridTickCount;
        int64_t scheduledSampleQpc = 0;
        int64_t encoderLateQpc = 0;
        uint32_t encoderLateTickCount = 0;
        bool drainingOutstandingLiveTicks = !media_main_g_EncoderRunning &&
                                            media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                            recordingOutputLive && !config.video.useVFR;
        if (media_main_g_EncoderRunning || drainingOutstandingLiveTicks) {
            scheduledSampleQpc = nextSampleTime.QuadPart;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (media_main_g_EncoderRunning) {
                int64_t waitTicks = nextSampleTime.QuadPart - now.QuadPart;
                if (waitTicks > 0) {
                    WaitUntilQpcTarget(hTimer, scheduledSampleQpc, qpcFreq.QuadPart);
                }

                QueryPerformanceCounter(&now);
                cycleStartQpc = now;  // Start measuring encode processing after timer sleep
                if (!config.video.useVFR && targetIntervalTicks > 0 && now.QuadPart > scheduledSampleQpc) {
                    encoderLateQpc = now.QuadPart - scheduledSampleQpc;
                    const uint32_t wakeLateUs = SaturatingToUint32(static_cast<uint64_t>(encoderLateQpc) * 1000000ull /
                                                                   static_cast<uint64_t>(qpcFreq.QuadPart));
                    encoderWakeLateAccumUs += wakeLateUs;
                    ++encoderWakeLateSamples;
                    encoderWakeLateMaxUs = std::max(encoderWakeLateMaxUs, wakeLateUs);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.encoderTimerWakeLateAvgUs.store(
                            SaturatingToUint32(encoderWakeLateAccumUs / encoderWakeLateSamples),
                            std::memory_order_relaxed);
                        media_main_g_pSharedMem->runtimeState.encoderTimerWakeLateMaxUs.store(encoderWakeLateMaxUs,
                                                                                   std::memory_order_relaxed);
                    }
                    const uint64_t lateTicks =
                        static_cast<uint64_t>(encoderLateQpc) / static_cast<uint64_t>(targetIntervalTicks);
                    encoderLateTickCount = SaturatingToUint32(lateTicks);
                }

                nextSampleTime.QuadPart += targetIntervalTicks;
            } else {
                nextSampleTime.QuadPart += targetIntervalTicks;
                cycleStartQpc = now;
            }

            if (media_main_g_EncoderRunning) {
                // Periodically resync the encoder grid to wall clock time to
                // prevent systematic drift when encoder ticks are consistently
                // longer than the target interval.  Without this, the selection
                // target grows increasingly out of sync with actual frame times.
                if (recordingOutputLive && encoderGridStartQpc > 0 && targetIntervalTicks > 0 && liveTicksOutput > 0 &&
                    (liveTicksOutput % 60 == 0) && outputShortfallTicks < 2) {
                    LARGE_INTEGER resyncNow;
                    QueryPerformanceCounter(&resyncNow);
                    const int64_t idealGridStart =
                        resyncNow.QuadPart - static_cast<int64_t>(liveTicksOutput) * targetIntervalTicks;
                    const int64_t driftTicks = (idealGridStart - encoderGridStartQpc) / targetIntervalTicks;
                    if (driftTicks >= 2 || driftTicks <= -2) {
                        encoderGridStartQpc = idealGridStart;
                    }
                }

                // Hidden warmup can rebase freely because those frames are discarded.
                // Once recording is live, skip ahead when significantly late to prevent
                // linear accumulation of encoder timer drift.  The buffered WGC frames
                // provide continuity — the output PTS gap is filled from the frame pool.
                const uint32_t timerRebaseThreshold = ce::capture_policy::GetCfrTimerRebaseThresholdTicks(
                    activeScreenGrab, config.video.useVFR, recordingOutputLive);
                if (!recordingOutputLive && now.QuadPart > nextSampleTime.QuadPart + targetIntervalTicks * 2) {
                    nextSampleTime = now;
                } else if (recordingOutputLive && encoderLateTickCount >= timerRebaseThreshold) {
                    static uint32_t s_lateTickLogCount = 0;
                    s_lateTickLogCount++;
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.timerRebases.fetch_add(1, std::memory_order_relaxed);
                    }
                    int64_t overshootTicks = (encoderLateQpc + targetIntervalTicks - 1) / targetIntervalTicks;
                    const int64_t overshootUs = (encoderLateQpc * 1000000) / qpcFreq.QuadPart;
                    uint32_t droppedShortfallTicks = 0;
                    const bool discardTimerDebt = ce::capture_policy::ShouldDiscardCfrTimerRebaseDebt(activeScreenGrab);
                    if (discardTimerDebt && liveStartQpc.QuadPart > 0 && now.QuadPart > liveStartQpc.QuadPart) {
                        const uint64_t elapsedTicks = static_cast<uint64_t>(now.QuadPart - liveStartQpc.QuadPart) /
                                                      static_cast<uint64_t>(targetIntervalTicks);
                        droppedShortfallTicks = ce::capture_policy::GetCfrTimerRebaseDiscardTicks(
                            elapsedTicks, liveTicksDiscardedByTimerRebase, liveTicksOutput);
                    }
                    liveTicksDiscardedByTimerRebase += droppedShortfallTicks;
                    outputShortfallTicks = updateLiveCfrShortfall(now.QuadPart);
                    recomputeCatchupPolicy();
                    if (s_lateTickLogCount <= 10 || s_lateTickLogCount % 60 == 0) {
                        LogInfo(
                            "[EncoderThread] Timer skip-ahead: late by %lld ticks (%lld us), rebasing "
                            "(count=%u, dropShortfall=%u, discardTotal=%llu, preserveShortfall=%u)",
                            (long long)overshootTicks, (long long)overshootUs, s_lateTickLogCount,
                            droppedShortfallTicks, static_cast<unsigned long long>(liveTicksDiscardedByTimerRebase),
                            discardTimerDebt ? 0u : 1u);
                    }
                    // Reset nextSampleTime to current time + 1 tick interval
                    // so the timer wakes on time from now on.
                    nextSampleTime.QuadPart = now.QuadPart + targetIntervalTicks;
                }
            } else {
                cycleStartQpc = now;
                nextSampleTime.QuadPart += targetIntervalTicks;
            }
        }

        int64_t scheduledOutputQpc = scheduledSampleQpc;
        if (!config.video.useVFR && recordingOutputLive && activeScreenGrab) {
            // Wake deadlines may rebase after expensive work, but source selection,
            // cursor sampling, and submission stay on the immutable CFR grid. Extra
            // held slots repay debt without duplicate QPC or postponing the next wake.
            scheduledOutputQpc = ce::capture_policy::GetNextCfrOutputQpc(
                liveStartQpc.QuadPart, liveTicksOutput, targetIntervalTicks, scheduledSampleQpc);
        }
        const auto computeWgcSelectionTargetForTick = [&](int64_t scheduledQpcForTick, int64_t selectionGridTickForTick,
                                                          bool applyLiveDelay) {
            const int64_t fallbackTargetQpc =
                ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTickForTick, targetIntervalTicks);
            // Uniform playout keeps its fixed delay through recovery; the legacy
            // reservoir may yield it. Keep target and application on one helper.
            const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
            const bool uniformCadenceActiveDelay = effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
            return ce::capture_policy::GetWgcActiveDelaySelectionTargetQpc(
                scheduledQpcForTick, fallbackTargetQpc, targetIntervalTicks, recordingOutputLive, applyLiveDelay,
                wgcLiveRecoveryModeActive, uniformCadenceActiveDelay, effectiveContentDelayQpc);
        };
        const auto computeWgcSelectionTargetQpc = [&](bool applyLiveDelay) {
            return computeWgcSelectionTargetForTick(scheduledOutputQpc, selectionGridTick, applyLiveDelay);
        };
        const auto computeLiveWgcSelectionTargetQpc = [&]() { return computeWgcSelectionTargetQpc(false); };
        const auto computeDelayedWgcSelectionTargetQpc = [&]() { return computeWgcSelectionTargetQpc(true); };
        const auto clampWgcSelectionTargetQpc = [&](int64_t selectionTargetQpc, int64_t liveNowQpc) {
            const bool encoderBottlenecked = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
            const int64_t clampedSelectionTargetQpc = ce::capture_policy::ClampWgcSelectionTargetToLiveQpc(
                selectionTargetQpc, liveNowQpc, targetIntervalTicks, qpcFreq.QuadPart, wgcLowSourceModeActive,
                wgcLiveRecoveryModeActive, outputShortfallTicks, encoderBottlenecked,
                ce::capture_policy::kCfrShortfallCatchupThresholdTicks, isWgcEncoderLimitedSmoothnessMode(),
                getWgcEffectiveContentDelayQpc());
            if (clampedSelectionTargetQpc > selectionTargetQpc) {
                const uint64_t clampDeltaUs = static_cast<uint64_t>(clampedSelectionTargetQpc - selectionTargetQpc) *
                                              1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                ++wgcSelectionTargetClampCount;
                wgcSelectionTargetClampMaxUs = std::max(wgcSelectionTargetClampMaxUs, SaturatingToUint32(clampDeltaUs));
            }
            return clampedSelectionTargetQpc;
        };
        const auto computeLiveTimelineElapsedUs = [&](int64_t scheduledQpcForTick) -> int64_t {
            if (liveStartQpc.QuadPart <= 0 || qpcFreq.QuadPart <= 0) {
                return -1;
            }
            const int64_t deltaQpc = scheduledQpcForTick - liveStartQpc.QuadPart;
            if (deltaQpc < 0) {
                return -1;
            }
            return (deltaQpc * 1000000) / qpcFreq.QuadPart;
        };

        QueuedFrame frame;
        bool popped = false;
        bool wgcTelemetryTickArmed = false;
        uint32_t wgcBufferedAtTickStart = 0;
        bool wgcFreshAvailableAtTickStart = false;
        bool wgcReserveAvailableAtTickStart = false;
        bool wgcSelectionDelayAppliedThisTick = false;
        bool wgcProactiveOverloadRepeatThisTick = false;
        bool wgcDelayRealizationRecordedThisTick = false;
        auto tryPopBufferedWgcFrameForTarget = [&](int64_t selectionTargetQpc, int64_t liveSelectionTargetQpc,
                                                   int64_t liveNowQpc, bool selectionDelayApplied,
                                                   QueuedFrame* selectedFrame,
                                                   bool* repeatedBecauseNoFrameCoverage = nullptr) {
            if (repeatedBecauseNoFrameCoverage) {
                *repeatedBecauseNoFrameCoverage = false;
            }
            if (!selectedFrame || bufferedWgcFrames.empty()) {
                return false;
            }

            pruneStaleWgcVisualDebt(liveNowQpc, "selection", media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode,
                                    selectionTargetQpc);

            const bool lowSourceMode = wgcLowSourceModeActive;
            const bool deepUnderfeed = ce::capture_policy::IsWgcDeepUnderfeed(
                outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille);
            // When an A/V content delay is active, a GPU-bound source that under-delivers cannot
            // sustain the delay reservoir. Defending it per-tick by selecting older-than-target
            // frames (reserve-preservation index-0 bias + soft-late older search) perturbs the
            // otherwise-uniform CFR cadence into abnormal judder. In uniform-cadence mode we take
            // the closest-to-target frame (monotonic + hard-cap guards stay intact) and let the
            // realized content delay float gracefully; sync-safe relaxed rescue paths are kept.
            const bool preferUniformActiveDelayCadence = ce::capture_policy::IsWgcActiveDelayUniformCadenceMode(
                selectionDelayApplied, config.wgcActiveDelayUniformCadence);
            ce::capture_policy::WgcAdaptiveTelemetry activeDelayTelemetry{};
            activeDelayTelemetry.outputFps = outputFps;
            activeDelayTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
            activeDelayTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
            activeDelayTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
            activeDelayTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
            activeDelayTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
            const uint32_t wgcSourceJitterAvgUs = media_main_g_WgcCap ? SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs()) : 0u;
            const uint32_t wgcPredictorJitterUs =
                wgcInputPredictor.IsCalibrated()
                    ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                    : 0u;
            activeDelayTelemetry.averageJitterUs = std::max(wgcSourceJitterAvgUs, wgcPredictorJitterUs);
            activeDelayTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
            activeDelayTelemetry.bufferedWgcFrames =
                static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), static_cast<size_t>(UINT32_MAX)));
            const int64_t effectiveSelectionTargetQpc =
                selectionTargetQpc > 0 ? selectionTargetQpc : liveSelectionTargetQpc;
            const uint32_t activeDelaySoftLateTargetUs =
                ce::capture_policy::GetWgcActiveDelaySoftLateTargetUs(targetIntervalTicks, qpcFreq.QuadPart);
            const int64_t minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
                lastEmittedWgcSourceQpc, liveSelectionTargetQpc, targetIntervalTicks, lowSourceMode);
            const int64_t baseStaleFallbackMinTimestampQpc =
                ce::capture_policy::GetWgcStaleUniqueFallbackMinTimestampQpc(
                    lastEmittedWgcSourceQpc, effectiveSelectionTargetQpc, targetIntervalTicks, lowSourceMode,
                    deepUnderfeed);
            int64_t staleFallbackMinTimestampQpc = baseStaleFallbackMinTimestampQpc;
            if (selectionDelayApplied && staleFallbackMinTimestampQpc > 0 && activeDelayTelemetry.averageJitterUs > 0 &&
                targetIntervalTicks > 0 && qpcFreq.QuadPart > 0) {
                const uint32_t reserveCapFrames = getWgcDelayReservoirTargetFrames() + 2u;
                if (bufferedWgcFrames.size() <= reserveCapFrames) {
                    const int64_t jitterMarginQpc = std::min<int64_t>(
                        targetIntervalTicks * 2,
                        (static_cast<int64_t>(activeDelayTelemetry.averageJitterUs) * qpcFreq.QuadPart) / 1000000);
                    staleFallbackMinTimestampQpc = std::max<int64_t>(0, staleFallbackMinTimestampQpc - jitterMarginQpc);
                }
            }

            while (bufferedWgcFrames.size() > 1) {
                const QueuedFrame& current = bufferedWgcFrames[0];
                const QueuedFrame& next = bufferedWgcFrames[1];
                const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
                const bool sameSelectionTimestamp =
                    current.selectionTimestamp > 0 && current.selectionTimestamp == next.selectionTimestamp;
                const bool duplicateSelectionCandidate = sameTimestamp || sameSelectionTimestamp;
                const bool currentTooOld = staleFallbackMinTimestampQpc > 0 && current.timestamp > 0 &&
                                           current.timestamp < staleFallbackMinTimestampQpc &&
                                           next.timestamp > current.timestamp;
                if (!duplicateSelectionCandidate && !currentTooOld) {
                    break;
                }

                QueuedFrame obsolete = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(obsolete);
                ++wgcDropObsoleteCount;
            }

            if (bufferedWgcFrames.empty()) {
                ++wgcFreshSelectionMissCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }

            bool skippedTooNewForSlot = false;
            bool olderFrameAvoidedRepeatThisTick = false;
            const auto activeDelayRepeatClusterTicks = [&]() -> uint32_t {
                return std::max<uint32_t>(
                    cadenceCounters.consecutiveDuplicateFrames,
                    cadenceCounters.holdTicksRunning > 1 ? (cadenceCounters.holdTicksRunning - 1) : 0);
            };
            const auto currentDelayResidualAvgAbsUs = [&]() -> uint32_t {
                if (wgcDelayResidualWindowSamples > 0) {
                    return SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs / wgcDelayResidualWindowSamples);
                }
                return wgcDelayResidualSamples > 0
                           ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                           : 0u;
            };
            const auto currentDelayResidualP95Us = [&]() -> uint32_t {
                const uint32_t windowP95 = wgcDelayResidualWindowP95Us();
                return windowP95 > 0 ? windowP95 : wgcDelayResidualP95Us();
            };
            const auto currentDelayResidualLateMaxUs = [&]() -> uint32_t {
                return wgcDelayResidualWindowLateMaxUs > 0 ? wgcDelayResidualWindowLateMaxUs
                                                           : wgcDelayResidualLateMaxUs;
            };
            const auto currentRawDelayResidualAvgAbsUs = [&]() -> uint32_t {
                if (wgcDelayRawResidualWindowSamples > 0) {
                    return SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs / wgcDelayRawResidualWindowSamples);
                }
                return wgcDelayRawResidualSamples > 0
                           ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                           : 0u;
            };
            const auto currentRawDelayResidualP95Us = [&]() -> uint32_t {
                const uint32_t windowP95 = wgcDelayRawResidualWindowP95Us();
                return windowP95 > 0 ? windowP95 : wgcDelayRawResidualP95Us();
            };
            const auto currentRawDelayResidualLateMaxUs = [&]() -> uint32_t {
                return wgcDelayRawResidualWindowLateMaxUs > 0 ? wgcDelayRawResidualWindowLateMaxUs
                                                              : wgcDelayRawResidualLateMaxUs;
            };
            const auto currentCombinedDelayResidualAvgAbsUs = [&]() -> uint32_t {
                return std::max(currentDelayResidualAvgAbsUs(), currentRawDelayResidualAvgAbsUs());
            };
            const auto currentCombinedDelayResidualP95Us = [&]() -> uint32_t {
                return std::max(currentDelayResidualP95Us(), currentRawDelayResidualP95Us());
            };
            const auto currentCombinedDelayResidualLateMaxUs = [&]() -> uint32_t {
                return std::max(currentDelayResidualLateMaxUs(), currentRawDelayResidualLateMaxUs());
            };
            const auto activeDelayWindowClassFor = [&](bool hardSafeCandidateAvailable) {
                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                return ce::capture_policy::ClassifyWgcActiveDelayWindow(
                    activeDelayTelemetry, lowSourceMode, wgcLiveRecoveryModeActive, wgcSourceStarvedCurrent,
                    deepUnderfeed, activeDelaySourceRecovery, hardSafeCandidateAvailable);
            };
            const auto rawActiveDelayCandidateSafe = [&](int64_t rawSelectionTimestamp) -> bool {
                if (rawSelectionTimestamp <= 0) {
                    return true;
                }
                if (ce::capture_policy::IsWgcFrameTooNewForActiveDelayHardLimit(
                        rawSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart)) {
                    return false;
                }
                uint32_t rawLateResidualUs = 0;
                if (rawSelectionTimestamp > effectiveSelectionTargetQpc && qpcFreq.QuadPart > 0) {
                    rawLateResidualUs = SaturatingToUint32(static_cast<uint64_t>(
                        (rawSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000 / qpcFreq.QuadPart));
                }
                return ce::capture_policy::HasWgcActiveDelayResidualHeadroom(
                    rawLateResidualUs, currentRawDelayResidualAvgAbsUs(), currentRawDelayResidualP95Us(),
                    currentRawDelayResidualLateMaxUs(), activeDelayWindowClassFor(true), activeDelaySoftLateTargetUs);
            };
            const auto activeDelayCandidateLateResidualUs = [&](const QueuedFrame& candidate) -> uint32_t {
                return ce::capture_policy::GetWgcActiveDelayFinalSelectionLateResidualUs(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, qpcFreq.QuadPart);
            };
            const auto isActiveDelayCandidateHardSafe = [&](const QueuedFrame& candidate) -> bool {
                if (!selectionDelayApplied || effectiveSelectionTargetQpc <= 0 || candidate.timestamp <= 0) {
                    return false;
                }
                const bool sourceTimestampAdvanced = candidate.timestamp > lastEmittedWgcSourceQpc;
                const bool fallbackFreshEnough =
                    ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp, staleFallbackMinTimestampQpc);
                if (!sourceTimestampAdvanced || !fallbackFreshEnough) {
                    return false;
                }
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart);
            };
            const auto isActiveDelayCandidateSoftSafe = [&](const QueuedFrame& candidate) -> bool {
                if (!isActiveDelayCandidateHardSafe(candidate)) {
                    return false;
                }
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(
                    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart, activeDelaySoftLateTargetUs);
            };
            const auto hasActiveDelayHardSafeCandidate = [&]() -> bool {
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (isActiveDelayCandidateHardSafe(candidate)) {
                        return true;
                    }
                }
                return false;
            };
            const auto hasActiveDelaySoftSafeCandidate = [&]() -> bool {
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (isActiveDelayCandidateSoftSafe(candidate)) {
                        return true;
                    }
                }
                return false;
            };
            const auto currentOldestSoftSafeAgeUs = [&]() -> uint32_t {
                if (liveNowQpc <= 0 || qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                uint32_t oldestAgeUs = 0;
                for (const QueuedFrame& candidate : bufferedWgcFrames) {
                    if (!isActiveDelayCandidateSoftSafe(candidate)) {
                        continue;
                    }
                    const int64_t selectionTimestamp = GetFrameSelectionTimestamp(candidate);
                    if (selectionTimestamp <= 0 || liveNowQpc <= selectionTimestamp) {
                        continue;
                    }
                    const uint32_t ageUs = SaturatingToUint32(
                        static_cast<uint64_t>((liveNowQpc - selectionTimestamp) * 1000000 / qpcFreq.QuadPart));
                    oldestAgeUs = std::max(oldestAgeUs, ageUs);
                }
                return oldestAgeUs;
            };
            const auto currentRepeatReserveSpanUs = [&]() -> uint32_t {
                if (bufferedWgcFrames.size() < 2 || qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                const int64_t firstQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                const int64_t lastQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                if (firstQpc <= 0 || lastQpc <= firstQpc) {
                    return 0u;
                }
                return SaturatingToUint32(static_cast<uint64_t>((lastQpc - firstQpc) * 1000000 / qpcFreq.QuadPart));
            };
            const auto recordActiveDelayRepeatClass =
                [&](ce::capture_policy::WgcActiveDelayWindowClass repeatWindowClass, bool hardSafeCandidateAvailable,
                    bool softSafeCandidateAvailable) {
                    if (!selectionDelayApplied) {
                        return;
                    }
                    switch (repeatWindowClass) {
                        case ce::capture_policy::WgcActiveDelayWindowClass::kHealthy:
                            ++wgcDelayWindowHealthyRepeatWindow;
                            ++wgcDelayWindowHealthyRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kRecoverableUnderfill:
                            ++wgcDelayWindowRecoverableRepeatWindow;
                            ++wgcDelayWindowRecoverableRepeatTotal;

                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited:
                            ++wgcDelayWindowSourceLimitedRepeatWindow;
                            ++wgcDelayWindowSourceLimitedRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kHardSourceStall:
                            ++wgcDelayWindowSourceLimitedRepeatWindow;
                            ++wgcDelayWindowSourceLimitedRepeatTotal;
                            ++wgcDelayWindowHardStallRepeatWindow;
                            ++wgcDelayWindowHardStallRepeatTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery:
                            ++wgcDelayWindowRecoverableRepeatWindow;
                            ++wgcDelayWindowRecoverableRepeatTotal;
                            ++wgcDelayWindowPostStallRepeatWindow;
                            ++wgcDelayWindowPostStallRepeatTotal;
                            break;
                    }
                    if (hardSafeCandidateAvailable) {
                        ++wgcDelayRepeatWithSafeCandidateWindow;
                        ++wgcDelayRepeatWithSafeCandidateTotal;
                    } else {
                        ++wgcDelayRepeatWithoutSafeCandidateWindow;
                        ++wgcDelayRepeatWithoutSafeCandidateTotal;
                    }
                    if (softSafeCandidateAvailable) {
                        ++wgcDelayRepeatWithSoftSafeCandidateWindow;
                        ++wgcDelayRepeatWithSoftSafeCandidateTotal;
                        const uint32_t oldestSoftSafeAgeUs = currentOldestSoftSafeAgeUs();
                        wgcDelayOldestSoftSafeAgeWindowMaxUs =
                            std::max(wgcDelayOldestSoftSafeAgeWindowMaxUs, oldestSoftSafeAgeUs);
                        wgcDelayOldestSoftSafeAgeMaxUs = std::max(wgcDelayOldestSoftSafeAgeMaxUs, oldestSoftSafeAgeUs);
                    } else {
                        ++wgcDelayRepeatWithoutSoftSafeCandidateWindow;
                        ++wgcDelayRepeatWithoutSoftSafeCandidateTotal;
                        ++wgcDelaySyncProtectedRepeatWindow;
                        ++wgcDelaySyncProtectedRepeatTotal;
                        if (hardSafeCandidateAvailable) {
                            ++wgcDelayRepeatHardOnlyCandidateWindow;
                            ++wgcDelayRepeatHardOnlyCandidateTotal;
                        }
                    }
                    if (!wgcActiveDelayRepeatClassKnown || repeatWindowClass != wgcActiveDelayLastRepeatClass) {
                        const uint64_t nowTick = GetTickCount64();
                        if (!wgcActiveDelayRepeatClassKnown || nowTick - wgcActiveDelayLastRepeatClassLogTick >= 500) {
                            LogInfo(
                                "[WGC CFR] Active-delay repeat state=%s hardSafe=%d softSafe=%d srcStarved=%d "
                                "lowSource=%d deepUnderfeed=%d recoveryActive=%d buffered=%zu span=%uus "
                                "residualP95=%uus rawP95=%uus softTarget=%uus",
                                ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                                hardSafeCandidateAvailable ? 1 : 0, softSafeCandidateAvailable ? 1 : 0,
                                wgcSourceStarvedCurrent ? 1 : 0, lowSourceMode ? 1 : 0, deepUnderfeed ? 1 : 0,
                                wgcActiveDelaySourceRecoveryUntilTick > nowTick ? 1 : 0, bufferedWgcFrames.size(),
                                currentRepeatReserveSpanUs(), currentCombinedDelayResidualP95Us(),
                                currentRawDelayResidualP95Us(), activeDelaySoftLateTargetUs);
                            wgcActiveDelayLastRepeatClassLogTick = nowTick;
                        }
                        wgcActiveDelayRepeatClassKnown = true;
                        wgcActiveDelayLastRepeatClass = repeatWindowClass;
                    }
                    const uint32_t reserveDepth = SaturatingToUint32(bufferedWgcFrames.size());
                    wgcDelayRepeatReserveDepthWindowMax = std::max(wgcDelayRepeatReserveDepthWindowMax, reserveDepth);
                    wgcDelayRepeatReserveDepthMax = std::max(wgcDelayRepeatReserveDepthMax, reserveDepth);
                    const uint32_t reserveSpanUs = currentRepeatReserveSpanUs();
                    wgcDelayRepeatReserveSpanWindowMaxUs =
                        std::max(wgcDelayRepeatReserveSpanWindowMaxUs, reserveSpanUs);
                    wgcDelayRepeatReserveSpanMaxUs = std::max(wgcDelayRepeatReserveSpanMaxUs, reserveSpanUs);
                };
            const auto isCurrentSyncDelayHoldSourceLimited = [&](bool softSafeCandidateAvailable) -> bool {
                if (!selectionDelayApplied) {
                    return false;
                }
                if (!softSafeCandidateAvailable) {
                    return true;
                }
                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                const bool sourceRecoveryWithoutSafeFrame = activeDelaySourceRecovery && !softSafeCandidateAvailable;
                if (ce::capture_policy::IsWgcSyncDelayHoldSourceLimited(
                        outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                        wgcSourceStarvedCurrent, lowSourceMode, deepUnderfeed, sourceRecoveryWithoutSafeFrame)) {
                    return true;
                }
                return false;
            };
            const auto recordActiveDelayRepeatLowerBound = [&](bool softSafeCandidateAvailable,
                                                               bool syncDelayHoldSourceLimited) {
                if (!selectionDelayApplied) {
                    return;
                }
                if (syncDelayHoldSourceLimited || !softSafeCandidateAvailable) {
                    ++wgcSourceRepeatLowerBoundWindow;
                    ++wgcSourceRepeatLowerBoundTotal;
                    ++wgcDelaySourceLimitedRepeatWindow;
                    ++wgcDelaySourceLimitedRepeatTotal;
                    return;
                }

                ++wgcExcessRepeatWindow;
                ++wgcExcessRepeatTotal;
                ++wgcPolicyAddedRepeatWindow;
                ++wgcPolicyAddedRepeatTotal;
                const uint32_t repeatClusterTicks = activeDelayRepeatClusterTicks();
                if (repeatClusterTicks > 0) {
                    ++wgcExcessRepeatClusterWindow;
                    ++wgcExcessRepeatClusterTotal;
                    wgcExcessRepeatClusterWindowMaxTicks =
                        std::max(wgcExcessRepeatClusterWindowMaxTicks, repeatClusterTicks);
                    wgcExcessRepeatClusterMaxTicks = std::max(wgcExcessRepeatClusterMaxTicks, repeatClusterTicks);
                }
            };
            const auto recordSyncDelayRepeatHold = [&](bool countRepeatClusterPressure, bool hardSafeCandidateAvailable,
                                                       bool softSafeCandidateAvailable) {
                if (selectionDelayApplied && countRepeatClusterPressure) {
                    const uint32_t repeatClusterTicks = activeDelayRepeatClusterTicks();
                    if (repeatClusterTicks > 0) {
                        ++wgcDelayRepeatClusterPressureWindow;
                        ++wgcDelayRepeatClusterPressureTotal;
                        wgcDelayRepeatClusterPressureWindowMaxTicks =
                            std::max(wgcDelayRepeatClusterPressureWindowMaxTicks, repeatClusterTicks);
                        wgcDelayRepeatClusterPressureMaxTicks =
                            std::max(wgcDelayRepeatClusterPressureMaxTicks, repeatClusterTicks);
                    }
                }
                ++wgcRepeatPolicyHoldCount;
                ++wgcRepeatPolicyHoldTotal;
                if (selectionDelayApplied) {
                    ++wgcSyncDelayHoldCount;
                    ++wgcSyncDelayHoldTotal;
                    const uint64_t selectionNowTick = GetTickCount64();
                    const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > selectionNowTick;
                    const auto repeatWindowClass = activeDelayWindowClassFor(hardSafeCandidateAvailable);
                    recordActiveDelayRepeatClass(repeatWindowClass, hardSafeCandidateAvailable,
                                                 softSafeCandidateAvailable);
                    const bool syncDelayHoldSourceLimited =
                        isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                    if (syncDelayHoldSourceLimited) {
                        ++wgcSyncDelaySourceLimitedHoldCount;
                        ++wgcSyncDelaySourceLimitedHoldTotal;
                        if (activeDelaySourceRecovery && !wgcSourceStarvedCurrent && !lowSourceMode && !deepUnderfeed) {
                            ++wgcSyncDelaySourceRecoveryHoldCount;
                            ++wgcSyncDelaySourceRecoveryHoldTotal;
                        }
                    } else {
                        ++wgcSyncDelayPolicyHoldCount;
                        ++wgcSyncDelayPolicyHoldTotal;
                    }
                }
            };
            auto buildCandidateList = [&](std::vector<size_t>* outIndices, bool requireFresh,
                                          bool allowRelaxedActiveDelayResidual) {
                if (!outIndices) {
                    return;
                }
                outIndices->clear();
                for (size_t i = 0; i < bufferedWgcFrames.size(); ++i) {
                    const QueuedFrame& candidate = bufferedWgcFrames[i];
                    if (candidate.timestamp <= 0) {
                        continue;
                    }
                    const bool sourceTimestampAdvanced = candidate.timestamp > lastEmittedWgcSourceQpc;
                    const bool freshEnough =
                        ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp, minFreshTimestampQpc);
                    const bool fallbackFreshEnough = ce::capture_policy::IsWgcTimestampFreshEnough(
                        candidate.timestamp, staleFallbackMinTimestampQpc);
                    const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
                    const int64_t candidateRawSelectionTimestamp = getWgcRawSelectionTimestamp(candidate);
                    if (selectionDelayApplied &&
                        !ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                            candidateSelectionTimestamp, candidateRawSelectionTimestamp, effectiveSelectionTargetQpc,
                            targetIntervalTicks, qpcFreq.QuadPart)) {
                        skippedTooNewForSlot = true;
                        ++wgcDelayRelaxedRejectedSyncRiskWindow;
                        ++wgcDelayRelaxedRejectedSyncRiskTotal;
                        continue;
                    }
                    const bool tooNewForSlot =
                        media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode &&
                        (selectionDelayApplied
                             ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks)
                             : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks));
                    if (tooNewForSlot) {
                        bool useRelaxedActiveDelayCandidate = false;
                        if (selectionDelayApplied && allowRelaxedActiveDelayResidual && media_main_g_HasLastFrame &&
                            !media_main_g_LastFrame.isInjectMode) {
                            const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(media_main_g_LastFrame);
                            const auto delayWindowClass = activeDelayWindowClassFor(true);
                            const auto relaxedScore = ce::capture_policy::ScoreWgcActiveDelayRelaxedCandidate(
                                candidateSelectionTimestamp, repeatSelectionTimestamp, effectiveSelectionTargetQpc,
                                targetIntervalTicks, qpcFreq.QuadPart, activeDelayRepeatClusterTicks(),
                                currentCombinedDelayResidualAvgAbsUs(), currentCombinedDelayResidualP95Us(),
                                currentCombinedDelayResidualLateMaxUs(), delayWindowClass, activeDelaySoftLateTargetUs);
                            useRelaxedActiveDelayCandidate = relaxedScore.Accepted();
                            if (useRelaxedActiveDelayCandidate &&
                                !rawActiveDelayCandidateSafe(candidateRawSelectionTimestamp)) {
                                useRelaxedActiveDelayCandidate = false;
                                ++wgcDelayRelaxedRejectedSyncRiskWindow;
                                ++wgcDelayRelaxedRejectedSyncRiskTotal;
                            }
                            switch (relaxedScore.decision) {
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                                    ++wgcDelayRelaxedRejectedSyncRiskWindow;
                                    ++wgcDelayRelaxedRejectedSyncRiskTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                                    ++wgcDelayRelaxedRejectedResidualHeadroomWindow;
                                    ++wgcDelayRelaxedRejectedResidualHeadroomTotal;
                                    if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(delayWindowClass) &&
                                        relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                        ++wgcDelaySoftLateRejectedWindow;
                                        ++wgcDelaySoftLateRejectedTotal;
                                    }
                                    break;
                                case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                                    ++wgcDelayRelaxedRejectedRepeatCostWindow;
                                    ++wgcDelayRelaxedRejectedRepeatCostTotal;
                                    break;
                                default:
                                    break;
                            }
                            if (useRelaxedActiveDelayCandidate &&
                                relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelayNearCapAcceptedWindow;
                                ++wgcDelayNearCapAcceptedTotal;
                            }
                            if (useRelaxedActiveDelayCandidate &&
                                !ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(delayWindowClass) &&
                                relaxedScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelaySoftLateAcceptedWindow;
                                ++wgcDelaySoftLateAcceptedTotal;
                            }
                        }
                        if (!useRelaxedActiveDelayCandidate) {
                            skippedTooNewForSlot = true;
                            continue;
                        }
                    }
                    if (requireFresh) {
                        if (!(sourceTimestampAdvanced && freshEnough)) {
                            continue;
                        }
                    } else {
                        if (!(sourceTimestampAdvanced && fallbackFreshEnough)) {
                            continue;
                        }
                    }
                    outIndices->push_back(i);
                }
            };

            buildCandidateList(&wgcFreshCandidateIndices, true, false);
            if (wgcFreshCandidateIndices.empty()) {
                buildCandidateList(&wgcFallbackCandidateIndices, false, false);
            }

            std::vector<size_t>* candidateIndices =
                !wgcFreshCandidateIndices.empty() ? &wgcFreshCandidateIndices : &wgcFallbackCandidateIndices;
            bool usingFreshCandidateSet = !wgcFreshCandidateIndices.empty();
            const auto bestCandidateDistance = [&](const std::vector<size_t>& indices) -> int64_t {
                if (indices.empty() || effectiveSelectionTargetQpc <= 0) {
                    return INT64_MAX;
                }
                int64_t bestDistance = AbsoluteTimestampDistance(
                    GetFrameSelectionTimestamp(bufferedWgcFrames[indices[0]]), effectiveSelectionTargetQpc);
                for (size_t candidateOffset = 1; candidateOffset < indices.size(); ++candidateOffset) {
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[indices[candidateOffset]]),
                        effectiveSelectionTargetQpc);
                    bestDistance = std::min(bestDistance, candidateDistance);
                }
                return bestDistance;
            };
            if (selectionDelayApplied && skippedTooNewForSlot) {
                buildCandidateList(&wgcRelaxedFreshCandidateIndices, true, true);
                if (wgcRelaxedFreshCandidateIndices.empty()) {
                    buildCandidateList(&wgcRelaxedFallbackCandidateIndices, false, true);
                }
                std::vector<size_t>* relaxedCandidateIndices = !wgcRelaxedFreshCandidateIndices.empty()
                                                                   ? &wgcRelaxedFreshCandidateIndices
                                                                   : &wgcRelaxedFallbackCandidateIndices;
                if (!relaxedCandidateIndices->empty()) {
                    const int64_t strictDistance = bestCandidateDistance(*candidateIndices);
                    const int64_t relaxedDistance = bestCandidateDistance(*relaxedCandidateIndices);
                    if (candidateIndices->empty() || relaxedDistance < strictDistance) {
                        candidateIndices = relaxedCandidateIndices;
                        usingFreshCandidateSet = !wgcRelaxedFreshCandidateIndices.empty();
                    }
                }
            }

            if (selectionDelayApplied && skippedTooNewForSlot && candidateIndices->empty() && media_main_g_HasLastFrame &&
                !media_main_g_LastFrame.isInjectMode) {
                wgcRepeatRescueCandidateIndices.clear();
                ++wgcDelayRepeatRescueAttemptWindow;
                ++wgcDelayRepeatRescueAttemptTotal;
                ++wgcDelayRepeatPromotionAttemptWindow;
                ++wgcDelayRepeatPromotionAttemptTotal;
                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(media_main_g_LastFrame);
                const auto rescueWindowClass = activeDelayWindowClassFor(true);
                for (size_t i = 0; i < bufferedWgcFrames.size(); ++i) {
                    const QueuedFrame& candidate = bufferedWgcFrames[i];
                    if (candidate.timestamp <= 0 || candidate.timestamp <= lastEmittedWgcSourceQpc ||
                        !ce::capture_policy::IsWgcTimestampFreshEnough(candidate.timestamp,
                                                                       staleFallbackMinTimestampQpc)) {
                        continue;
                    }
                    if (isActiveDelayCandidateSoftSafe(candidate)) {
                        wgcRepeatRescueCandidateIndices.push_back(i);
                        continue;
                    }

                    const auto rescueScore = ce::capture_policy::ScoreWgcActiveDelayRepeatRescueCandidate(
                        GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                        repeatSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart,
                        activeDelayRepeatClusterTicks(), currentCombinedDelayResidualAvgAbsUs(),
                        currentCombinedDelayResidualP95Us(), currentCombinedDelayResidualLateMaxUs(), rescueWindowClass,
                        activeDelaySoftLateTargetUs);
                    if (rescueScore.Accepted()) {
                        wgcRepeatRescueCandidateIndices.push_back(i);
                        continue;
                    }

                    switch (rescueScore.decision) {
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                            ++wgcDelayRepeatRescueRejectedSyncWindow;
                            ++wgcDelayRepeatRescueRejectedSyncTotal;
                            break;
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                            ++wgcDelayRepeatRescueRejectedHeadroomWindow;
                            ++wgcDelayRepeatRescueRejectedHeadroomTotal;
                            if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(rescueWindowClass) &&
                                rescueScore.candidateLateResidualUs > activeDelaySoftLateTargetUs) {
                                ++wgcDelayRepeatPromotionRejectedSoftWindow;
                                ++wgcDelayRepeatPromotionRejectedSoftTotal;
                            }
                            break;
                        case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                            ++wgcDelayRepeatRescueRejectedCostWindow;
                            ++wgcDelayRepeatRescueRejectedCostTotal;
                            break;
                        default:
                            break;
                    }
                }
                if (!wgcRepeatRescueCandidateIndices.empty()) {
                    candidateIndices = &wgcRepeatRescueCandidateIndices;
                    usingFreshCandidateSet = false;
                    olderFrameAvoidedRepeatThisTick = true;
                    ++wgcDelayRepeatRescueSuccessWindow;
                    ++wgcDelayRepeatRescueSuccessTotal;
                    ++wgcDelayRepeatPromotedBeforeRepeatWindow;
                    ++wgcDelayRepeatPromotedBeforeRepeatTotal;
                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                }
            }

            if (candidateIndices->empty()) {
                ++wgcFreshSelectionMissCount;
                if (skippedTooNewForSlot) {
                    const bool hardSafeCandidateAvailable = hasActiveDelayHardSafeCandidate();
                    const bool softSafeCandidateAvailable = hasActiveDelaySoftSafeCandidate();
                    if (hardSafeCandidateAvailable) {
                        ++wgcDelayRepeatSafeAfterPromotionWindow;
                        ++wgcDelayRepeatSafeAfterPromotionTotal;
                    }
                    const bool syncDelayHoldSourceLimited =
                        isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                    const auto repeatWindowClass = activeDelayWindowClassFor(hardSafeCandidateAvailable);
                    recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable, syncDelayHoldSourceLimited);
                    recordSyncDelayRepeatHold(true, hardSafeCandidateAvailable, softSafeCandidateAvailable);
                    const int64_t firstSelectionTimestamp =
                        !bufferedWgcFrames.empty() ? GetFrameSelectionTimestamp(bufferedWgcFrames.front()) : 0;
                    int64_t leadUs = 0;
                    if (qpcFreq.QuadPart > 0 && firstSelectionTimestamp > effectiveSelectionTargetQpc) {
                        leadUs = ((firstSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart;
                        const uint32_t leadUsClamped = SaturatingToUint32(static_cast<uint64_t>(leadUs));
                        wgcTooNewLeadMaxUs = std::max(wgcTooNewLeadMaxUs, leadUsClamped);
                        wgcTooNewLeadSessionMaxUs = std::max(wgcTooNewLeadSessionMaxUs, leadUsClamped);
                    }
                    static uint64_t s_lastTooNewWgcSelectionLogTick = 0;
                    const uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastTooNewWgcSelectionLogTick >= 1000) {
                        const int64_t allowedLeadUs =
                            (qpcFreq.QuadPart > 0 && targetIntervalTicks > 0)
                                ? ((selectionDelayApplied
                                        ? ce::capture_policy::GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks)
                                        : (targetIntervalTicks *
                                           static_cast<int64_t>(ce::capture_policy::kWgcCfrSelectionMaxLeadTicks))) *
                                   1000000) /
                                      qpcFreq.QuadPart
                                : 0;
                        const int64_t avDelayUs = (qpcFreq.QuadPart > 0 && getWgcEffectiveContentDelayQpc() > 0)
                                                      ? (getWgcEffectiveContentDelayQpc() * 1000000) / qpcFreq.QuadPart
                                                      : 0;
                        LogInfo(
                            "[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot "
                            "(lead=%lldus allowedLead=%lldus avDelay=%lldus syncDelay=%d syncSourceLimited=%d "
                            "delayClass=%s softLateTarget=%uus hardSafeCandidate=%d softSafeCandidate=%d "
                            "lowSource=%d sourceStarved=%d encoderLimited=%d targetQpc=%lld firstQpc=%lld "
                            "buffered=%zu shortfall=%u minIn=%u minDel=%u noFresh=%upm)",
                            static_cast<long long>(leadUs), static_cast<long long>(allowedLeadUs),
                            static_cast<long long>(avDelayUs), selectionDelayApplied ? 1 : 0,
                            syncDelayHoldSourceLimited ? 1 : 0,
                            ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                            activeDelaySoftLateTargetUs, hardSafeCandidateAvailable ? 1 : 0,
                            softSafeCandidateAvailable ? 1 : 0, lowSourceMode ? 1 : 0, wgcSourceStarvedCurrent ? 1 : 0,
                            isWgcEncoderLimitedSmoothnessMode() ? 1 : 0,
                            static_cast<long long>(effectiveSelectionTargetQpc),
                            static_cast<long long>(firstSelectionTimestamp), bufferedWgcFrames.size(),
                            outputShortfallTicks, wgcRecentInputMin250Fps, wgcRecentDeliveredMin250Fps,
                            wgcNoFreshTickPermille);
                        s_lastTooNewWgcSelectionLogTick = nowTick;
                    }
                } else {
                    recordActiveDelayRepeatLowerBound(false, true);
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }

            size_t selectedIndex = candidateIndices->front();
            if (effectiveSelectionTargetQpc > 0) {
                size_t bestCandidateOffset = 0;
                int64_t bestDistance = AbsoluteTimestampDistance(
                    GetFrameSelectionTimestamp(bufferedWgcFrames[(*candidateIndices)[0]]), effectiveSelectionTargetQpc);
                for (size_t candidateOffset = 1; candidateOffset < candidateIndices->size(); ++candidateOffset) {
                    const size_t bufferedIndex = (*candidateIndices)[candidateOffset];
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    if (candidateDistance < bestDistance ||
                        (candidateDistance == bestDistance &&
                         GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) >
                             GetFrameSelectionTimestamp(bufferedWgcFrames[(*candidateIndices)[bestCandidateOffset]]))) {
                        bestDistance = candidateDistance;
                        bestCandidateOffset = candidateOffset;
                    }
                }
                selectedIndex = (*candidateIndices)[bestCandidateOffset];
            }

            selectedIndex = ce::capture_policy::ClampWgcSelectionIndexForLowSource(
                selectedIndex, bufferedWgcFrames.size(), bufferedWgcFrames.size(), wgcRecentDeliveredFps,
                wgcRecentInputMin250Fps, outputFps, wgcNoFreshTickPermille, wgcLiveRecoveryModeActive);

            if (usingFreshCandidateSet && selectedIndex > 0) {
                const QueuedFrame& earlierFresh = bufferedWgcFrames[0];
                const QueuedFrame& chosenFresh = bufferedWgcFrames[selectedIndex];
                if (earlierFresh.timestamp > lastEmittedWgcSourceQpc &&
                    chosenFresh.timestamp > earlierFresh.timestamp &&
                    ce::capture_policy::ShouldPreferEarlierFreshWgcFrameForReserveDefense(
                        earlierFresh.selectionTimestamp > 0 ? earlierFresh.selectionTimestamp : earlierFresh.timestamp,
                        chosenFresh.selectionTimestamp > 0 ? chosenFresh.selectionTimestamp : chosenFresh.timestamp,
                        effectiveSelectionTargetQpc, targetIntervalTicks, wgcReservePressureActive, lowSourceMode,
                        deepUnderfeed, wgcLiveRecoveryModeActive, preferUniformActiveDelayCadence)) {
                    selectedIndex = 0;
                } else {
                    ++wgcReserveSpendTickCount;
                }
            }

            const auto finalSelectionWithinActiveDelayHardLimit = [&](size_t candidateIndex) -> bool {
                if (!selectionDelayApplied || effectiveSelectionTargetQpc <= 0 ||
                    candidateIndex >= bufferedWgcFrames.size()) {
                    return true;
                }
                const QueuedFrame& finalCandidate = bufferedWgcFrames[candidateIndex];
                return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    GetFrameSelectionTimestamp(finalCandidate), getWgcRawSelectionTimestamp(finalCandidate),
                    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart);
            };
            const auto activeDelayLateResidualUsForIndex = [&](size_t candidateIndex) -> uint32_t {
                if (candidateIndex >= bufferedWgcFrames.size() || effectiveSelectionTargetQpc <= 0 ||
                    qpcFreq.QuadPart <= 0) {
                    return 0u;
                }
                return activeDelayCandidateLateResidualUs(bufferedWgcFrames[candidateIndex]);
            };
            if (!preferUniformActiveDelayCadence && selectionDelayApplied && candidateIndices &&
                !candidateIndices->empty() &&
                !ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(activeDelayWindowClassFor(true)) &&
                activeDelayLateResidualUsForIndex(selectedIndex) > activeDelaySoftLateTargetUs) {
                bool foundSoftCandidate = false;
                size_t softIndex = selectedIndex;
                int64_t bestSoftDistance = INT64_MAX;
                uint32_t bestSoftLateResidualUs = UINT32_MAX;
                for (size_t bufferedIndex = 0; bufferedIndex < bufferedWgcFrames.size(); ++bufferedIndex) {
                    const QueuedFrame& softCandidate = bufferedWgcFrames[bufferedIndex];
                    if (bufferedIndex >= bufferedWgcFrames.size() ||
                        softCandidate.timestamp <= lastEmittedWgcSourceQpc ||
                        !ce::capture_policy::IsWgcTimestampFreshEnough(softCandidate.timestamp,
                                                                       staleFallbackMinTimestampQpc) ||
                        !finalSelectionWithinActiveDelayHardLimit(bufferedIndex) ||
                        activeDelayLateResidualUsForIndex(bufferedIndex) > activeDelaySoftLateTargetUs) {
                        continue;
                    }
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    const uint32_t candidateLateResidualUs = activeDelayLateResidualUsForIndex(bufferedIndex);
                    if (!foundSoftCandidate || candidateDistance < bestSoftDistance ||
                        (candidateDistance == bestSoftDistance &&
                         (candidateLateResidualUs < bestSoftLateResidualUs ||
                          (candidateLateResidualUs == bestSoftLateResidualUs &&
                           GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) <
                               GetFrameSelectionTimestamp(bufferedWgcFrames[softIndex]))))) {
                        foundSoftCandidate = true;
                        softIndex = bufferedIndex;
                        bestSoftDistance = candidateDistance;
                        bestSoftLateResidualUs = candidateLateResidualUs;
                    }
                }
                if (foundSoftCandidate) {
                    selectedIndex = softIndex;
                    olderFrameAvoidedRepeatThisTick = true;
                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                }
            }
            if (!finalSelectionWithinActiveDelayHardLimit(selectedIndex) && candidateIndices &&
                !candidateIndices->empty()) {
                bool foundSafeCandidate = false;
                size_t rescueIndex = selectedIndex;
                int64_t bestDistance = INT64_MAX;
                for (const size_t bufferedIndex : *candidateIndices) {
                    if (bufferedIndex >= bufferedWgcFrames.size() ||
                        !finalSelectionWithinActiveDelayHardLimit(bufferedIndex)) {
                        continue;
                    }
                    const int64_t candidateDistance = AbsoluteTimestampDistance(
                        GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]), effectiveSelectionTargetQpc);
                    if (!foundSafeCandidate || candidateDistance < bestDistance ||
                        (candidateDistance == bestDistance &&
                         GetFrameSelectionTimestamp(bufferedWgcFrames[bufferedIndex]) >
                             GetFrameSelectionTimestamp(bufferedWgcFrames[rescueIndex]))) {
                        foundSafeCandidate = true;
                        rescueIndex = bufferedIndex;
                        bestDistance = candidateDistance;
                    }
                }
                if (foundSafeCandidate) {
                    selectedIndex = rescueIndex;
                    ++wgcDelayPostSelectionRescuedSyncRiskWindow;
                    ++wgcDelayPostSelectionRescuedSyncRiskTotal;
                }
            }

            const QueuedFrame& candidate = bufferedWgcFrames[selectedIndex];
            const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
            const int64_t candidateRawSelectionTimestamp = getWgcRawSelectionTimestamp(candidate);
            if (selectionDelayApplied && effectiveSelectionTargetQpc > 0 &&
                !ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                    candidateSelectionTimestamp, candidateRawSelectionTimestamp, effectiveSelectionTargetQpc,
                    targetIntervalTicks, qpcFreq.QuadPart)) {
                ++wgcDelayPostSelectionRejectedSyncRiskWindow;
                ++wgcDelayPostSelectionRejectedSyncRiskTotal;
                const bool hardSafeCandidateAvailable = hasActiveDelayHardSafeCandidate();
                const bool softSafeCandidateAvailable = hasActiveDelaySoftSafeCandidate();
                recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable,
                                                  isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable));
                recordSyncDelayRepeatHold(true, hardSafeCandidateAvailable, softSafeCandidateAvailable);
                static uint64_t s_lastWgcFinalSelectionRejectLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastWgcFinalSelectionRejectLogTick >= 1000 ||
                    wgcDelayPostSelectionRejectedSyncRiskWindow <= 3) {
                    const int64_t predictedLeadUs =
                        qpcFreq.QuadPart > 0
                            ? ((candidateSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    const int64_t rawLeadUs =
                        qpcFreq.QuadPart > 0 && candidateRawSelectionTimestamp > 0
                            ? ((candidateRawSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000) /
                                  qpcFreq.QuadPart
                            : 0;
                    LogWarn(
                        "[EncoderThread] WGC active-delay final selection rejected: predictedLead=%lldus "
                        "rawLead=%lldus selectedIndex=%zu buffered=%zu targetQpc=%lld predictedQpc=%lld "
                        "rawQpc=%lld",
                        static_cast<long long>(predictedLeadUs), static_cast<long long>(rawLeadUs), selectedIndex,
                        bufferedWgcFrames.size(), static_cast<long long>(effectiveSelectionTargetQpc),
                        static_cast<long long>(candidateSelectionTimestamp),
                        static_cast<long long>(candidateRawSelectionTimestamp));
                    s_lastWgcFinalSelectionRejectLogTick = nowTick;
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            const bool encoderLimitedSmoothnessForBacktrack = isWgcEncoderLimitedSmoothnessMode();
            if (candidate.timestamp > 0 && lastEmittedWgcSourceQpc > 0 &&
                (candidate.timestamp < lastEmittedWgcSourceQpc ||
                 (encoderLimitedSmoothnessForBacktrack && candidate.timestamp == lastEmittedWgcSourceQpc))) {
                ++wgcSelectedSourceBacktrackThisWindow;
                ++wgcSelectedSourceBacktrackTotal;
                static uint64_t s_lastWgcBacktrackLogTick = 0;
                const uint64_t nowTick = GetTickCount64();
                if (nowTick - s_lastWgcBacktrackLogTick >= 1000) {
                    LogWarn(
                        "[EncoderThread] WGC CFR selected source backtrack blocked: candidateQpc=%lld "
                        "lastEmittedQpc=%lld selectedIndex=%zu buffered=%zu mode=%s shortfall=%u",
                        static_cast<long long>(candidate.timestamp), static_cast<long long>(lastEmittedWgcSourceQpc),
                        selectedIndex, bufferedWgcFrames.size(),
                        encoderLimitedSmoothnessForBacktrack ? "encoder_limited" : "normal", outputShortfallTicks);
                    s_lastWgcBacktrackLogTick = nowTick;
                }
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            const bool selectedActiveDelayCandidateRelaxed =
                selectionDelayApplied && effectiveSelectionTargetQpc > 0 &&
                ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                    candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks);
            if (selectionDelayApplied && skippedTooNewForSlot && !selectedActiveDelayCandidateRelaxed &&
                !olderFrameAvoidedRepeatThisTick) {
                ++wgcDelayOlderFrameAvoidedRepeatWindow;
                ++wgcDelayOlderFrameAvoidedRepeatTotal;
            }
            if (selectedActiveDelayCandidateRelaxed && qpcFreq.QuadPart > 0) {
                const uint32_t residualUs = SaturatingToUint32(static_cast<uint64_t>(
                    (candidateSelectionTimestamp - effectiveSelectionTargetQpc) * 1000000 / qpcFreq.QuadPart));
                ++wgcDelayRelaxedSelectionCount;
                ++wgcDelayRelaxedSelectionWindowCount;
                wgcDelayRelaxedSelectionMaxUs = std::max(wgcDelayRelaxedSelectionMaxUs, residualUs);
                const int64_t repeatSelectionTimestamp = media_main_g_HasLastFrame ? GetFrameSelectionTimestamp(media_main_g_LastFrame) : 0;
                const auto relaxedScore = ce::capture_policy::ScoreWgcActiveDelayRelaxedCandidate(
                    candidateSelectionTimestamp, repeatSelectionTimestamp, effectiveSelectionTargetQpc,
                    targetIntervalTicks, qpcFreq.QuadPart, activeDelayRepeatClusterTicks(),
                    currentDelayResidualAvgAbsUs(), currentDelayResidualP95Us(), currentDelayResidualLateMaxUs(),
                    activeDelayWindowClassFor(true), activeDelaySoftLateTargetUs);
                if (relaxedScore.decision == ce::capture_policy::WgcActiveDelayRelaxedDecision::kAcceptBetterTarget) {
                    ++wgcDelayRelaxedBetterTargetWindow;
                    ++wgcDelayRelaxedBetterTargetTotal;
                } else if (relaxedScore.decision ==
                           ce::capture_policy::WgcActiveDelayRelaxedDecision::kAcceptRepeatCluster) {
                    ++wgcDelayRelaxedRepeatClusterWindow;
                    ++wgcDelayRelaxedRepeatClusterTotal;
                }
            }
            const auto selectedActiveDelayWindowClass =
                selectionDelayApplied && isActiveDelayCandidateHardSafe(candidate) ? activeDelayWindowClassFor(true)
                                                                                   : activeDelayWindowClassFor(false);
            const bool selectedPostStallSafeFrame =
                selectionDelayApplied &&
                selectedActiveDelayWindowClass == ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery;
            const bool canHoldFreshFrame =

                selectionDelayApplied && !selectedPostStallSafeFrame && selectedIndex == 0 &&
                bufferedWgcFrames.size() == 1 &&
                ce::capture_policy::ShouldHoldSingleFreshWgcFrame(
                    wgcReservePressureActive, lowSourceMode, wgcRecentInputMin250Fps, outputFps, smoothedInputPerTick,
                    outputShortfallTicks, media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), false,
                    deepUnderfeed);
            if (canHoldFreshFrame && effectiveSelectionTargetQpc > 0 &&
                ShouldHoldFrameForNextTick(candidateSelectionTimestamp, effectiveSelectionTargetQpc,
                                           targetIntervalTicks, targetIntervalTicks / 10)) {
                const bool softSafeCandidateAvailable = isActiveDelayCandidateSoftSafe(candidate);
                recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable,
                                                  isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable));
                ++wgcHoldForNextTickCount;
                ++wgcHeldFreshFrameTickCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            if (selectedPostStallSafeFrame) {
                ++wgcDelayPostStallSafeFrameWindow;
                ++wgcDelayPostStallSafeFrameTotal;
            }

            /*
            if (liveSelectionTargetQpc > 0 && candidateSelectionTimestamp > liveSelectionTargetQpc && g_HasLastFrame &&
                !g_LastFrame.isInjectMode) {
                ++wgcFreshSelectionMissCount;
                if (repeatedBecauseNoFrameCoverage) {
                    *repeatedBecauseNoFrameCoverage = true;
                }
                return false;
            }
            */

            if (!usingFreshCandidateSet) {
                ++wgcStaleUniqueFallbackCount;
                if (effectiveSelectionTargetQpc > 0 &&
                    candidateSelectionTimestamp + targetIntervalTicks < effectiveSelectionTargetQpc) {
                    ++wgcAncientSelectionCount;
                }
            }

            // Frame age limit: when the encoder is severely behind, reject frames that are
            // too old.  This prevents encoding ancient content (e.g., 17 seconds old) that
            // doesn't match the intended output position. Instead, we emit a duplicate frame
            // which "stutters honestly" while the encoder recovers.
            constexpr int64_t kMaxFrameAgeMs = 1000;  // 1 second maximum frame age
            if (encoderTooSlowForTargetCurrent && effectiveSelectionTargetQpc > 0 && qpcFreq.QuadPart > 0) {
                const int64_t frameAgeTicks = effectiveSelectionTargetQpc - candidateSelectionTimestamp;
                const int64_t frameAgeMs = (frameAgeTicks * 1000) / qpcFreq.QuadPart;
                if (frameAgeMs > kMaxFrameAgeMs) {
                    ++wgcFreshSelectionMissCount;
                    if (repeatedBecauseNoFrameCoverage) {
                        *repeatedBecauseNoFrameCoverage = true;
                    }
                    return false;
                }
            }

            for (size_t i = 0; i < selectedIndex; ++i) {
                QueuedFrame obsolete = std::move(bufferedWgcFrames.front());
                bufferedWgcFrames.pop_front();
                ReleaseQueuedFrameTexture(obsolete);
                ++wgcDropObsoleteCount;
            }

            if (preferUniformActiveDelayCadence) {
                ++wgcDelayUniformCadenceWindow;
                ++wgcDelayUniformCadenceTotal;
            }

            *selectedFrame = std::move(bufferedWgcFrames.front());
            bufferedWgcFrames.pop_front();
            if (selectedFrame->duplicateSourceTimestamp) {
                ++wgcSelectDuplicateSourceCount;
            } else {
                ++wgcSelectFreshCount;
            }

            return true;
        };

        auto inspectBufferedWgcCoverageForTarget = [&](int64_t selectionTargetQpc, bool activeDelaySelection,
                                                       uint32_t requiredReserveFrames, bool* hasFrameForTick,
                                                       bool* hasReserveFrame) {
            if (hasFrameForTick) {
                *hasFrameForTick = false;
            }
            if (hasReserveFrame) {
                *hasReserveFrame = false;
            }
            if (bufferedWgcFrames.empty()) {
                return;
            }

            size_t idx = 0;
            if (selectionTargetQpc > 0) {
                while ((idx + 1) < bufferedWgcFrames.size()) {
                    const QueuedFrame& current = bufferedWgcFrames[idx];
                    const QueuedFrame& next = bufferedWgcFrames[idx + 1];
                    const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
                    const bool nextAlreadyCoversTarget = next.timestamp > 0 && next.timestamp <= selectionTargetQpc;
                    if (!sameTimestamp && !nextAlreadyCoversTarget) {
                        break;
                    }
                    ++idx;
                }
            }

            if (idx >= bufferedWgcFrames.size()) {
                return;
            }

            const QueuedFrame& candidate = bufferedWgcFrames[idx];
            const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
            const bool canUseCandidateNow =
                selectionTargetQpc <= 0 || candidateSelectionTimestamp <= 0 ||
                !(activeDelaySelection ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                             candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)
                                       : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                             candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)) ||
                !media_main_g_HasLastFrame || media_main_g_LastFrame.isInjectMode;
            if (hasFrameForTick) {
                *hasFrameForTick = canUseCandidateNow;
            }
            if (hasReserveFrame) {
                const uint32_t reserveFrames = static_cast<uint32_t>(
                    std::min<size_t>(bufferedWgcFrames.size() - idx, static_cast<size_t>(UINT32_MAX)));
                *hasReserveFrame = canUseCandidateNow && reserveFrames >= std::max<uint32_t>(1u, requiredReserveFrames);
            }
        };

        if (useScreenGrab) {
            if (!bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }
            smoothedInjectFenceMs = 0.0;
            if (!config.video.useVFR) {
                // CFR WGC: drain all pending captured frames and let the CFR slot
                // scheduler be the only authority for selection/repeat/drop.
                drainedScreenGrabFrames.clear();
                drainedWgcCapturedFrames.clear();
                if (auto capture = media_main_g_WgcCap.Read()) {
                    const uint64_t drainSourceEpoch = media_main_g_WgcSourceEpoch.load(std::memory_order_acquire);
                    capture->DrainPendingFrames(drainedWgcCapturedFrames, 0);
                    for (auto& capturedFrame : drainedWgcCapturedFrames) {
                        if (!capturedFrame.texture) {
                            continue;
                        }
                        if (capturedFrame.sourceEpoch != drainSourceEpoch) {
                            static uint64_t s_retiredPullFrameDrops = 0;
                            ++s_retiredPullFrameDrops;
                            if (s_retiredPullFrameDrops <= 3 || (s_retiredPullFrameDrops % 120ull) == 0ull) {
                                LogInfo(
                                    "[WGC] Dropping retired-source pull frame: frameEpoch=%llu activeEpoch=%llu "
                                    "discarded=%llu",
                                    static_cast<unsigned long long>(capturedFrame.sourceEpoch),
                                    static_cast<unsigned long long>(drainSourceEpoch),
                                    static_cast<unsigned long long>(s_retiredPullFrameDrops));
                            }
                            ReleaseWgcCapturedFrame(capturedFrame);
                            continue;
                        }
                        drainedScreenGrabFrames.push_back(MakeQueuedWgcFrame(std::move(capturedFrame)));
                    }
                }

                QueuedFrame temp;
                while (media_main_g_FrameQueue.Pop(temp, 0)) {
                    DiscardQueuedFrame(temp);
                }

                const bool sampleWgcCadenceTick = !(recordingOutputLive && encoderLateTickCount > 0);

                // Phase 1: keep only frames that belong to the recording interval,
                // then feed raw timestamps to predictor BEFORE moving frames to the
                // buffer (std::move invalidates source object). Always feed the
                // predictor (even when encoder is late) so it can calibrate the
                // source FPS for Bresenham pacing and logging.
                if (!drainedScreenGrabFrames.empty()) {
                    const int64_t stopBoundaryQpc = !media_main_g_Recording.load(std::memory_order_acquire)
                                                        ? media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire)
                                                        : 0;
                    size_t postStopDropped = 0;
                    uint32_t postStopMaxLeadUs = 0;
                    std::vector<QueuedFrame> keptFrames;
                    keptFrames.reserve(drainedScreenGrabFrames.size());
                    for (auto& drainedFrame : drainedScreenGrabFrames) {
                        const int64_t sourceFrameQpc =
                            drainedFrame.rawTimestamp > 0 ? drainedFrame.rawTimestamp : drainedFrame.timestamp;
                        if (!ce::capture_policy::ShouldKeepWgcFrameForStopDrain(sourceFrameQpc, stopBoundaryQpc)) {
                            if (qpcFreq.QuadPart > 0 && sourceFrameQpc > stopBoundaryQpc) {
                                const uint64_t leadUs = static_cast<uint64_t>(sourceFrameQpc - stopBoundaryQpc) *
                                                        1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                postStopMaxLeadUs = std::max(postStopMaxLeadUs, SaturatingToUint32(leadUs));
                            }
                            ReleaseQueuedFrameTexture(drainedFrame);
                            ++postStopDropped;
                            ++wgcPostStopFrameDropTotal;
                            continue;
                        }

                        if (!drainedFrame.isInjectMode && drainedFrame.timestamp > 0) {
                            wgcInputPredictor.Update(drainedFrame.timestamp, qpcFreq.QuadPart);
                            // Monotonic bounded-deviation smoothing of the raw compositor timestamp.
                            // WGC/DXGI timestamps are DWM composition times and arrive quantized
                            // under VRR/composed presentation even when the game presents perfectly
                            // smoothly; a CFR playout slaved to the raw stamps converts a surplus
                            // source into constant single-tick repeats (fortistutter root cause).
                            // The raw timestamp stays untouched for sync validation/diagnostics.
                            drainedFrame.selectionTimestamp =
                                wgcInputPredictor.SmoothMonotonicTimestamp(drainedFrame.timestamp, targetIntervalTicks);
                            observeCaptureSyncPhaseSource(
                                "screen_grab", wgcCfrPhaseLock, GetFrameSelectionTimestamp(drainedFrame));
                            if (drainedFrame.selectionTimestamp > 0 && qpcFreq.QuadPart > 0) {
                                const int64_t devQpc =
                                    AbsoluteTimestampDistance(drainedFrame.selectionTimestamp, drainedFrame.timestamp);
                                const uint32_t devUs = SaturatingToUint32(static_cast<uint64_t>(devQpc) * 1000000ull /
                                                                          static_cast<uint64_t>(qpcFreq.QuadPart));
                                ++wgcTsSmoothSamplesWindow;
                                wgcTsSmoothDevAccumUsWindow += devUs;
                                wgcTsSmoothDevMaxUsWindow = std::max(wgcTsSmoothDevMaxUsWindow, devUs);
                                wgcTsSmoothDevMaxUsTotal = std::max(wgcTsSmoothDevMaxUsTotal, devUs);
                                const uint64_t snapTotal = wgcInputPredictor.SmoothingSnapCount();
                                if (snapTotal > wgcTsSmoothSnapCountTotal) {
                                    wgcTsSmoothSnapCountWindow +=
                                        static_cast<uint32_t>(snapTotal - wgcTsSmoothSnapCountTotal);
                                    wgcTsSmoothSnapCountTotal = snapTotal;
                                }
                            }
                            static int64_t s_lastWgcSrcQpc = 0;
                            if (drainedFrame.timestamp == s_lastWgcSrcQpc) {
                                ++dupTimestampCount;
                            }
                            s_lastWgcSrcQpc = drainedFrame.timestamp;
                        }
                        keptFrames.push_back(std::move(drainedFrame));
                    }
                    if (postStopDropped > 0) {
                        wgcPostStopFrameDropMaxUs = std::max(wgcPostStopFrameDropMaxUs, postStopMaxLeadUs);
                        static uint64_t s_lastPostStopWgcDropLogTick = 0;
                        const uint64_t nowTick = GetTickCount64();
                        if (nowTick - s_lastPostStopWgcDropLogTick >= 1000 || postStopDropped >= 4) {
                            LogWarn(
                                "[EncoderThread] WGC CFR post-stop frame drop: dropped=%zu stopQpc=%lld maxLead=%uus "
                                "total=%llu",
                                postStopDropped, static_cast<long long>(stopBoundaryQpc), postStopMaxLeadUs,
                                static_cast<unsigned long long>(wgcPostStopFrameDropTotal));
                            s_lastPostStopWgcDropLogTick = nowTick;
                        }
                    }
                    drainedScreenGrabFrames.swap(keptFrames);
                }

                // Phase 2: append newly drained WGC frames and keep the host-side
                // reserve shallow. This restores timestamp-aware selection for
                // >target source cadence without letting callback bursts create a
                // deep unstable queue.
                for (auto& drainedFrame : drainedScreenGrabFrames) {
                    bufferedWgcFrames.push_back(std::move(drainedFrame));
                }

                if (recordingOutputLive && !bufferedWgcFrames.empty()) {
                    LARGE_INTEGER visualDebtNowQpc;
                    QueryPerformanceCounter(&visualDebtNowQpc);
                    int64_t visualDebtPolicyQpc = visualDebtNowQpc.QuadPart;
                    if (!media_main_g_Recording.load(std::memory_order_acquire)) {
                        const int64_t drainStopQpc = media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire);
                        if (drainStopQpc > 0) {
                            visualDebtPolicyQpc = drainStopQpc;
                        }
                    }
                    pruneStaleWgcVisualDebt(visualDebtPolicyQpc, "live-buffer",
                                            media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode, 0);
                }
                if (recordingOutputLive && !bufferedWgcFrames.empty()) {
                    trimBufferedWgcForPoolPressure("live-pool-pressure");
                }
                trimBufferedWgcToRetainedCap(recordingOutputLive ? "live-buffer" : "warmup-buffer");

                // Track frame arrival rate for pacing telemetry only.
                if (sampleWgcCadenceTick) {
                    pacingInputThisWindow += static_cast<uint32_t>(drainedScreenGrabFrames.size());
                    pacingTicksThisWindow++;
                    const uint32_t wgcPacingWindowSize = (pacingEmaUpdates < 6)
                                                             ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                             : (uint32_t)config.video.fps / 2;
                    if (pacingTicksThisWindow >= wgcPacingWindowSize) {
                        double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                        // Adaptive alpha: fast convergence during startup, burst detection, steady-state
                        double alpha = 0.5;
                        if (pacingEmaUpdates < 6) {
                            alpha = 0.7;
                        } else if (smoothedInputPerTick > 0.01) {
                            double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
                            if (deviation > 0.20) {
                                alpha = 0.8;
                            }
                        }
                        smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
                        pacingInputThisWindow = 0;
                        pacingTicksThisWindow = 0;
                        ++pacingEmaUpdates;
                    }
                }

                if (media_main_g_WgcCap) {
                    wgcRecentDeliveredFps = media_main_g_WgcCap->GetDeliveredRatePerSec();
                    wgcRecentDeliveredMin250Fps = media_main_g_WgcCap->GetDeliveredMin250Fps();
                    wgcRecentDeliveredMin500Fps = media_main_g_WgcCap->GetDeliveredMin500Fps();
                    wgcRecentInputMin250Fps = media_main_g_WgcCap->GetInputMin250Fps();
                    wgcRecentInputMin500Fps = media_main_g_WgcCap->GetInputMin500Fps();
                }
                const uint32_t wgcPolicySourceJitterUs =
                    media_main_g_WgcCap ? SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs()) : 0u;
                const uint32_t wgcPolicyPredictorJitterUs =
                    wgcInputPredictor.IsCalibrated()
                        ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                        : 0u;
                const uint32_t wgcPolicyAverageJitterUs = std::max(wgcPolicySourceJitterUs, wgcPolicyPredictorJitterUs);

                wgcCoverageDelayTicksCurrent = 0;

                const uint64_t wgcPolicyNowTick = GetTickCount64();
                const ce::capture_policy::WgcAdaptiveTelemetry wgcAdaptiveTelemetry = {
                    outputFps,
                    wgcRecentDeliveredFps,
                    wgcRecentDeliveredMin250Fps,
                    wgcRecentDeliveredMin500Fps,
                    wgcRecentInputMin250Fps,
                    wgcRecentInputMin500Fps,
                    wgcPolicyAverageJitterUs,
                    wgcNoFreshTickPermille,
                    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
                    0u,
                    0.0,
                };
                const bool allowWgcLiveRecoveryMode =
                    recordingOutputLive && media_main_g_Recording.load(std::memory_order_acquire);
                static uint64_t s_lastWgcWarmupLogTick = 0;
                const bool inWgcWarmup =
                    wgcWarmupUntilQpc > 0 && liveTicksOutput < static_cast<uint64_t>(std::max(24u, outputFps / 6u));
                if (inWgcWarmup && s_lastWgcWarmupLogTick == 0) {
                    s_lastWgcWarmupLogTick = GetTickCount64();
                    LogInfo("[WGC CFR] Warmup active: %llu ticks to stabilize capture pipeline",
                            static_cast<unsigned long long>(std::max(24u, outputFps / 6u)));
                } else if (!inWgcWarmup && s_lastWgcWarmupLogTick > 0 &&
                           (wgcPolicyNowTick - s_lastWgcWarmupLogTick) >= 1000) {
                    LogInfo("[WGC CFR] Warmup ended: liveTicksOutput=%llu buffered=%zu",
                            static_cast<unsigned long long>(liveTicksOutput), bufferedWgcFrames.size());
                    s_lastWgcWarmupLogTick = 0;
                }
                const bool wgcSourceHealthTelemetryReady =
                    !inWgcWarmup && allowWgcLiveRecoveryMode &&
                    liveTicksOutput >= std::max<uint64_t>(8ull, outputFps / 8u) && wgcRecentDeliveredMin250Fps > 0 &&
                    wgcRecentDeliveredMin500Fps > 0 && wgcRecentInputMin250Fps > 0 && wgcRecentInputMin500Fps > 0;
                const bool wgcCapacityPressureForRecovery = isWgcCapacityPressureActive();
                const ce::capture_policy::WgcLiveRecoveryState wgcLiveRecoveryStateCurrent =
                    wgcSourceHealthTelemetryReady
                        ? ce::capture_policy::ClassifyWgcLiveRecoveryState(wgcAdaptiveTelemetry, outputShortfallTicks,
                                                                           wgcCapacityPressureForRecovery)
                        : ce::capture_policy::WgcLiveRecoveryState::kHealthy;
                wgcSourceStarvedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSourceStarved;
                wgcSchedulerLimitedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kSchedulerLimited;
                wgcEncoderRecoveryLimitedCurrent =
                    allowWgcLiveRecoveryMode &&
                    wgcLiveRecoveryStateCurrent == ce::capture_policy::WgcLiveRecoveryState::kEncoderLimited;
                wgcReservePressureActive = ce::capture_policy::IsWgcReservePressureActive(
                    wgcNoReserveTickCount, wgcQueueTickSampleCount, outputFps);
                const ce::capture_policy::WgcLowSourceState wgcLowSourceStateCurrent =
                    wgcSourceHealthTelemetryReady ? ce::capture_policy::ClassifyWgcLowSourceState(wgcAdaptiveTelemetry)
                                                  : ce::capture_policy::WgcLowSourceState::kHealthy;
                const bool shouldEnterWgcLowSourceMode =
                    wgcLowSourceStateCurrent != ce::capture_policy::WgcLowSourceState::kHealthy;
                const bool bufferedReserveRecovered =
                    isWgcEffectiveContentDelayActive()
                        ? ce::capture_policy::IsWgcDelayReservoirRecovered(
                              bufferedWgcFrames.size(), getWgcEffectiveContentDelayQpc(), targetIntervalTicks)
                        : bufferedWgcFrames.size() >= 3;
                const uint64_t wgcLowSourceDurationMs =
                    wgcLowSourceModeActive && wgcPolicyNowTick >= wgcLowSourceStateChangeTick
                        ? (wgcPolicyNowTick - wgcLowSourceStateChangeTick)
                        : 0;
                const bool stableWgcUnderfeed =
                    wgcSourceHealthTelemetryReady &&
                    wgcLowSourceDurationMs >= ce::capture_policy::kWgcStableUnderfeedClassificationMs &&
                    wgcRecentDeliveredMin250Fps > 0 && wgcRecentInputMin250Fps > 0;
                const bool shouldExitWgcLowSourceMode =
                    ce::capture_policy::ShouldExitWgcLowSourceMode(wgcAdaptiveTelemetry, encoderTooSlowForTargetCurrent,
                                                                   bufferedReserveRecovered, wgcLowSourceDurationMs);
                const auto wgcLowSourceModeUpdate = ce::capture_policy::UpdateHeldMode(
                    wgcLowSourceModeActive, wgcLowSourceStateChangeTick, wgcPolicyNowTick, shouldEnterWgcLowSourceMode,
                    shouldExitWgcLowSourceMode, !encoderTooSlowForTargetCurrent && bufferedReserveRecovered,
                    ce::capture_policy::kWgcLowSourceEnterHoldMs, ce::capture_policy::kWgcLowSourceExitHoldMs);
                wgcLowSourceModeActive = wgcLowSourceModeUpdate.active;
                wgcLowSourceStateChangeTick = wgcLowSourceModeUpdate.stateChangeTick;
                if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
                    LogInfo(
                        "[WGC CFR] Low-source mode entered: state=%s src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                        ce::capture_policy::WgcLowSourceStateToString(wgcLowSourceStateCurrent), wgcRecentDeliveredFps,
                        wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
                        wgcRecentInputMin500Fps, wgcNoFreshTickPermille, bufferedWgcFrames.size());
                } else if (wgcLowSourceModeUpdate.transition == ce::capture_policy::HeldModeTransition::kExited) {
                    if (wgcLowSourceModeUpdate.immediate) {
                        ++captureSessionSummary.lowSourceImmediateExits;
                    } else {
                        LogInfo("[WGC CFR] Low-source mode exited: src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                                wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                                wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                                bufferedWgcFrames.size());
                    }
                }

                if (wgcSourceHealthTelemetryReady) {
                    const bool shouldEnterWgcLiveRecoveryMode = ce::capture_policy::ShouldEnterWgcLiveRecoveryMode(
                        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery);
                    const bool shouldExitWgcLiveRecoveryMode = ce::capture_policy::ShouldExitWgcLiveRecoveryMode(
                        wgcAdaptiveTelemetry, outputShortfallTicks, wgcCapacityPressureForRecovery, stableWgcUnderfeed);
                    const auto wgcLiveRecoveryModeUpdate = ce::capture_policy::UpdateHeldMode(
                        wgcLiveRecoveryModeActive, wgcLiveRecoveryStateChangeTick, wgcPolicyNowTick,
                        shouldEnterWgcLiveRecoveryMode, shouldExitWgcLiveRecoveryMode, false,
                        ce::capture_policy::kWgcRecoveryEnterHoldMs, ce::capture_policy::kWgcRecoveryExitHoldMs);
                    wgcLiveRecoveryModeActive = wgcLiveRecoveryModeUpdate.active;
                    wgcLiveRecoveryStateChangeTick = wgcLiveRecoveryModeUpdate.stateChangeTick;
                    if (wgcLiveRecoveryModeUpdate.transition == ce::capture_policy::HeldModeTransition::kEntered) {
                        LogInfo(
                            "[WGC CFR] Live-recovery entered: state=%s srcStarved=%d schedLimited=%d encLimited=%d "
                            "shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm buffered=%zu",
                            ce::capture_policy::WgcLiveRecoveryStateToString(wgcLiveRecoveryStateCurrent),
                            wgcSourceStarvedCurrent ? 1 : 0, wgcSchedulerLimitedCurrent ? 1 : 0,
                            wgcEncoderRecoveryLimitedCurrent ? 1 : 0, outputShortfallTicks,
                            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            bufferedWgcFrames.size());
                    } else if (wgcLiveRecoveryModeUpdate.transition ==
                               ce::capture_policy::HeldModeTransition::kExited) {
                        LogInfo(
                            "[WGC CFR] Live-recovery exited: shortfall=%u/%.1fms src=%u/%u/%u input=%u/%u empty=%upm "
                            "buffered=%zu",
                            outputShortfallTicks,
                            ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                            wgcRecentDeliveredFps, wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            bufferedWgcFrames.size());
                    }
                } else {
                    wgcLiveRecoveryModeActive = false;
                    wgcLiveRecoveryStateChangeTick = 0;
                }

                const bool wgcCapacityPressureActiveCurrent = isWgcCapacityPressureActive();
                const uint32_t wgcBufferedFramesForPolicy =
                    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull));
                const bool wgcTrueSourceStarvedForCapacityCurrent =
                    ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
                        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy, wgcCapacityPressureActiveCurrent);
                const bool wgcSourceHealthyForEncoderLimitedCurrent =
                    ce::capture_policy::IsWgcSourceHealthyEnoughForEncoderLimitedSmoothness(
                        outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy);
                const bool wgcEncoderLimitedSmoothnessActiveCurrent = isWgcEncoderLimitedSmoothnessMode();
                if (wgcLowSourceModeActive && wgcCapacityPressureActiveCurrent &&
                    wgcSourceHealthyForEncoderLimitedCurrent && wgcEncoderLimitedSmoothnessActiveCurrent) {
                    ++wgcEncoderLimitedSuppressedByLowSourceThisWindow;
                    ++wgcEncoderLimitedSuppressedByLowSourceTotal;
                }
                if (wgcEncoderRecoveryLimitedCurrent && !wgcEncoderLimitedSmoothnessActiveCurrent &&
                    !wgcTrueSourceStarvedForCapacityCurrent) {
                    ++wgcCapacityPressureModeMismatchThisWindow;
                    ++wgcCapacityPressureModeMismatchTotal;
                    static uint64_t s_lastWgcModeMismatchLogTick = 0;
                    const uint64_t mismatchNowTick = GetTickCount64();
                    if (mismatchNowTick - s_lastWgcModeMismatchLogTick >= 1000) {
                        LogWarn(
                            "[WGC CFR] encoder-limited mode mismatch: recovery=encoder_limited smoothness=0 "
                            "lowSource=%d sourceHealthy=%d trueSourceStarved=%d shortfall=%u input=%u/%u "
                            "freshMiss=%upm buffered=%u overload=0x%X",
                            wgcLowSourceModeActive ? 1 : 0, wgcSourceHealthyForEncoderLimitedCurrent ? 1 : 0,
                            wgcTrueSourceStarvedForCapacityCurrent ? 1 : 0, outputShortfallTicks,
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
                            wgcBufferedFramesForPolicy, loadEncoderOverloadFlags());
                        s_lastWgcModeMismatchLogTick = mismatchNowTick;
                    }
                }

                const bool starvedEpisodeShouldBeActive =
                    wgcSourceHealthTelemetryReady &&
                    (ce::capture_policy::IsWgcDeepUnderfeed(outputFps, wgcRecentDeliveredMin250Fps,
                                                            wgcRecentInputMin250Fps, wgcNoFreshTickPermille) ||
                     (wgcLiveRecoveryModeActive && wgcSourceStarvedCurrent));
                if (starvedEpisodeShouldBeActive) {
                    if (!wgcStarvedEpisode.active) {
                        wgcStarvedEpisode.Reset();
                        wgcStarvedEpisode.active = true;
                        wgcStarvedEpisode.startTickMs = GetTickCount64();
                        LARGE_INTEGER episodeStartQpc = {};
                        QueryPerformanceCounter(&episodeStartQpc);
                        wgcStarvedEpisode.startQpc = episodeStartQpc.QuadPart;
                        wgcStarvedEpisode.startLiveTicks = liveTicksOutput;
                        wgcStarvedEpisode.startDuplicateTicks = captureSessionSummary.duplicateTicks;
                        if (media_main_g_WgcCap) {
                            wgcStarvedEpisode.startPoolSaturatedDrops = media_main_g_WgcCap->GetPoolSaturatedDropCount();
                            wgcStarvedEpisode.startPoolOverwritePrevented =
                                media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
                            wgcStarvedEpisode.startIngressDecimated = media_main_g_WgcCap->GetIngressDecimatedCount();
                        }
                    }
                    wgcStarvedEpisode.minInputFps = std::min(wgcStarvedEpisode.minInputFps, wgcRecentInputMin250Fps);
                    wgcStarvedEpisode.minDeliveredFps =
                        std::min(wgcStarvedEpisode.minDeliveredFps, wgcRecentDeliveredMin250Fps);
                    wgcStarvedEpisode.peakFreshMissPermille =
                        std::max(wgcStarvedEpisode.peakFreshMissPermille, wgcNoFreshTickPermille);
                    wgcStarvedEpisode.minBufferedFrames = std::min<uint32_t>(
                        wgcStarvedEpisode.minBufferedFrames,
                        static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)));
                } else if (wgcStarvedEpisode.active) {
                    const uint64_t nowTickMs = GetTickCount64();
                    const uint64_t durationMs = nowTickMs - wgcStarvedEpisode.startTickMs;
                    const uint64_t outputTicks = liveTicksOutput - wgcStarvedEpisode.startLiveTicks;
                    const uint64_t duplicateTicks =
                        captureSessionSummary.duplicateTicks - wgcStarvedEpisode.startDuplicateTicks;
                    finishWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks);
                }

                const bool activeDelaySevereSourceStall =
                    isWgcEffectiveContentDelayActive() && wgcSourceHealthTelemetryReady &&
                    ce::capture_policy::IsWgcSevereSourceStallForActiveDelay(
                        outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                        wgcBufferedFramesForPolicy);
                if (activeDelaySevereSourceStall) {
                    const uint64_t recoveryUntil =
                        wgcPolicyNowTick + ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs;
                    const bool enteringRecovery = wgcActiveDelaySourceRecoveryUntilTick <= wgcPolicyNowTick;
                    wgcActiveDelaySourceRecoveryUntilTick =
                        std::max<uint64_t>(wgcActiveDelaySourceRecoveryUntilTick, recoveryUntil);
                    if (enteringRecovery) {
                        LogInfo(
                            "[WGC CFR] Active-delay source recovery entered: src=%u/%u input=%u/%u empty=%upm "
                            "buffered=%u holdMs=%u",
                            wgcRecentDeliveredMin250Fps, wgcRecentDeliveredMin500Fps, wgcRecentInputMin250Fps,
                            wgcRecentInputMin500Fps, wgcNoFreshTickPermille, wgcBufferedFramesForPolicy,
                            ce::capture_policy::kWgcActiveDelaySourceRecoveryHoldMs);
                    }
                }
                if (isWgcEffectiveContentDelayActive() && wgcActiveDelaySourceRecoveryUntilTick > wgcPolicyNowTick) {
                    ++wgcActiveDelaySourceRecoveryTicks;
                }

                if (media_main_g_WgcCap && recordingOutputLive && media_main_g_Recording) {
                    const uint32_t currentTargetFps = media_main_g_WgcCap->GetProducerTargetFps();
                    if (currentTargetFps != 0) {
                        LogError(
                            "[WGC CFR] ERROR: producer contract violation: backend=%s outputFps=%u "
                            "producerTargetFps=%u; forcing MinUpdateInterval=0 because finite producer intervals "
                            "alias variable-rate input",
                            media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc", outputFps, currentTargetFps);
                        media_main_g_WgcCap->SetProducerTargetFps(0);
                        media_main_g_WgcProducerTargetFps.store(0, std::memory_order_relaxed);
                        ++wgcProducerRateRetuneCount;
                        ++wgcProducerRateRetuneTotal;
                    }
                }

                const bool scheduledWgcTelemetryTick =
                    !config.video.useVFR && media_main_g_EncoderRunning && media_main_g_Recording && recordingOutputLive;
                if (scheduledWgcTelemetryTick) {
                    LARGE_INTEGER selectionNowQpc;
                    QueryPerformanceCounter(&selectionNowQpc);
                    wgcTelemetryTickArmed = true;
                    wgcBufferedAtTickStart = static_cast<uint32_t>(bufferedWgcFrames.size());
                    wgcReserveAvailableAtTickStart = false;
                    // Uniform active-delay playout keeps its delay through below-target VRR cadence;
                    // otherwise recovery collapses realized delay toward zero and can latch there.
                    // The legacy reservoir path still yields its delay to live recovery.
                    const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
                    const bool uniformCadenceActiveDelay =
                        effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
                    const bool liveRecoverySuppressesDelay =
                        ce::capture_policy::ShouldLiveRecoverySuppressWgcSelectionDelay(wgcLiveRecoveryModeActive,
                                                                                        uniformCadenceActiveDelay);
                    const bool activeDelayInspection =
                        effectiveContentDelayQpc > 0 && recordingOutputLive && !liveRecoverySuppressesDelay;
                    const uint32_t requiredReservoirFrames =
                        activeDelayInspection ? std::max<uint32_t>(1u, getWgcDelayReservoirLowWaterFrames()) : 1u;
                    const int64_t reservoirInspectionTargetQpc =
                        activeDelayInspection
                            ? clampWgcSelectionTargetQpc(computeDelayedWgcSelectionTargetQpc(),
                                                         selectionNowQpc.QuadPart)
                            : clampWgcSelectionTargetQpc(computeWgcSelectionTargetQpc(false), selectionNowQpc.QuadPart);
                    inspectBufferedWgcCoverageForTarget(reservoirInspectionTargetQpc, activeDelayInspection,
                                                        requiredReservoirFrames, &wgcFreshAvailableAtTickStart,
                                                        &wgcReserveAvailableAtTickStart);
                    wgcSelectionDelayAppliedThisTick =
                        !liveRecoverySuppressesDelay &&
                        ce::capture_policy::ShouldApplyWgcSelectionDelay(
                            recordingOutputLive, outputShortfallTicks,
                            media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), wgcReserveAvailableAtTickStart,
                            effectiveContentDelayQpc > 0);
                    if (wgcSelectionDelayAppliedThisTick) {
                        ++wgcSelectionDelayTickCount;
                    }

                    const bool wgcSourceAtOrAboveTarget =
                        wgcSourceHealthTelemetryReady &&
                        ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                            outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps);
                    const bool wgcSourceHealthyForPacing =
                        !wgcTrueSourceStarvedForCapacityCurrent &&
                        (wgcSourceAtOrAboveTarget || wgcSourceHealthyForEncoderLimitedCurrent);
                    const bool wgcRepeatAvailableForPacer =
                        media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode &&
                        ((MediaEngine_RepeatLastFrameWithTimeline != nullptr) ||
                         (MediaEngine_RepeatLastFrame != nullptr)) &&
                        MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
                    const bool wgcPacingCapacityPressure =
                        wgcCapacityPressureActiveCurrent || outputShortfallTicks > 0;
                    const auto overloadPacerDecision = ce::capture_policy::UpdateWgcOverloadRepeatPacer(
                        wgcOverloadRepeatPacer, true, wgcSourceHealthyForPacing, wgcPacingCapacityPressure,
                        wgcFreshAvailableAtTickStart, wgcRepeatAvailableForPacer, smoothedWgcFreshServiceMs,
                        smoothedWgcRepeatServiceMs, frameIntervalMs, wgcFreshServiceSamples, wgcRepeatServiceSamples);
                    wgcProactiveOverloadRepeatThisTick = overloadPacerDecision.repeat;

                    static uint64_t s_lastWgcOverloadPacerLogTick = 0;
                    const uint64_t overloadPacerNowTick = GetTickCount64();
                    if (overloadPacerDecision.entered &&
                        overloadPacerNowTick - s_lastWgcOverloadPacerLogTick >= 1000) {
                        LogWarn(
                            "[WGC CFR] Overload repeat pacer entered: reason=%s backend=%s fresh=%.2fms/%u repeat=%.2fms/%u "
                            "budget=%.2fms freshFraction=%.3f shortfall=%u buffered=%zu source=%u/%u "
                            "repeats=%llu maxRun=%u (CFR PTS and audio timeline unchanged)",
                            overloadPacerDecision.reason,
                            media_main_g_WgcCap && media_main_g_WgcCap->IsUsingDesktopDuplication() ? "dxgi_dup" : "wgc",
                            smoothedWgcFreshServiceMs, wgcFreshServiceSamples, smoothedWgcRepeatServiceMs,
                            wgcRepeatServiceSamples, overloadPacerDecision.serviceBudgetMs,
                            overloadPacerDecision.freshFraction, outputShortfallTicks, bufferedWgcFrames.size(),
                            wgcRecentInputMin250Fps, wgcRecentInputMin500Fps,
                            static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats),
                            wgcOverloadRepeatPacer.maxConsecutiveProactiveRepeats);
                        s_lastWgcOverloadPacerLogTick = overloadPacerNowTick;
                    } else if (overloadPacerDecision.exited &&
                               overloadPacerNowTick - s_lastWgcOverloadPacerLogTick >= 1000) {
                        LogInfo(
                            "[WGC CFR] Overload repeat pacer exited: reason=%s fresh=%.2fms repeat=%.2fms "

                            "freshFraction=%.3f shortfall=%u repeats=%llu",
                            overloadPacerDecision.reason, smoothedWgcFreshServiceMs, smoothedWgcRepeatServiceMs,
                            overloadPacerDecision.freshFraction, outputShortfallTicks,
                            static_cast<unsigned long long>(wgcOverloadRepeatPacer.proactiveRepeats));
                        s_lastWgcOverloadPacerLogTick = overloadPacerNowTick;
                    }
                }

                if (wgcProactiveOverloadRepeatThisTick) {
                    // Spend this exact CFR slot on the cache without consuming
                    // the covered candidate or relabeling video/audio content.
                } else if (!media_main_g_EncoderRunning && !bufferedWgcFrames.empty()) {
                    frame = std::move(bufferedWgcFrames.front());
                    bufferedWgcFrames.pop_front();
                    popped = true;
                } else if (!bufferedWgcFrames.empty()) {
                    LARGE_INTEGER selectionNowQpc;
                    QueryPerformanceCounter(&selectionNowQpc);
                    const int64_t liveSelectionTargetQpc =
                        (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                            ? clampWgcSelectionTargetQpc(computeLiveWgcSelectionTargetQpc(), selectionNowQpc.QuadPart)
                            : 0;
                    const int64_t delayedSelectionTargetQpc =
                        (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                            ? clampWgcSelectionTargetQpc(computeDelayedWgcSelectionTargetQpc(),
                                                         selectionNowQpc.QuadPart)
                            : 0;
                    int64_t effectiveSelectionTargetQpc =
                        wgcSelectionDelayAppliedThisTick ? delayedSelectionTargetQpc : liveSelectionTargetQpc;
                    if (wgcLowSourceModeActive && !inWgcWarmup && targetIntervalTicks > 0 &&
                        !bufferedWgcFrames.empty()) {
                        const int64_t newestQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        if (newestQpc > 0 && effectiveSelectionTargetQpc > newestQpc + targetIntervalTicks) {
                            const int64_t drift = effectiveSelectionTargetQpc - (newestQpc + targetIntervalTicks);
                            wgcBiasAccumQpc += drift;
                            effectiveSelectionTargetQpc = newestQpc + targetIntervalTicks;
                            ++wgcBiasClampCount;
                        } else if (wgcBiasAccumQpc > 0) {
                            wgcBiasAccumQpc = std::max<int64_t>(0, wgcBiasAccumQpc - targetIntervalTicks * 2);
                        }
                    }
                    const bool useInjectParityDelayPacing = wgcSelectionDelayAppliedThisTick &&
                                                            isWgcEffectiveContentDelayActive() &&
                                                            config.wgcActiveDelayUniformCadence;
                    if (!useInjectParityDelayPacing) {
                        const int64_t phaseReferenceQpc = bufferedWgcFrames.empty()
                                                                  ? 0
                                                                  : GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        effectiveSelectionTargetQpc = applyCaptureSyncPhaseTarget(
                            "screen_grab", wgcCfrPhaseLock, effectiveSelectionTargetQpc, phaseReferenceQpc);
                    }
                    if (useInjectParityDelayPacing) {
                        // Fixed-latency jitter-buffer playout selects nearest to gridTick-contentDelay.
                        // Audio-passed surplus drops, too-new frames remain reserve, and true gaps hold while
                        // preserving CFR PTS/content A/V delay; raw timestamps remain for validation.
                        while (!bufferedWgcFrames.empty() && lastEmittedWgcSelectionQpc > 0 &&
                               GetFrameSelectionTimestamp(bufferedWgcFrames.front()) > 0 &&
                               GetFrameSelectionTimestamp(bufferedWgcFrames.front()) <= lastEmittedWgcSelectionQpc) {
                            QueuedFrame stale = std::move(bufferedWgcFrames.front());
                            bufferedWgcFrames.pop_front();
                            ReleaseQueuedFrameTexture(stale);
                            ++wgcDropObsoleteCount;
                        }
                        // Grid-anchored content-delay target (UNCLAMPED toward live: the uniform-cadence
                        // path maintains the delay through low-source/recovery rather than clamping
                        // toward live, which would re-collapse the realized delay -- the other half of
                        // the rubber-band).
                        int64_t playoutTargetQpc = (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                                                       ? computeDelayedWgcSelectionTargetQpc()
                                                       : 0;
                        const int64_t phaseReferenceQpc = bufferedWgcFrames.empty()
                                                                  ? 0
                                                                  : GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                        playoutTargetQpc = applyCaptureSyncPhaseTarget(
                            "screen_grab", wgcCfrPhaseLock, playoutTargetQpc, phaseReferenceQpc);
                        const int64_t playoutLeadToleranceQpc =
                            ce::capture_policy::GetWgcActiveDelayResidualToleranceQpc(targetIntervalTicks);
                        // Never raise this immutable audio-aligned target to reach retained source history.
                        // The former anti-freeze floor judged safety from wall-clock frame age, which can
                        // still put seconds-newer visual content into an old CFR/audio slot after encoder
                        // debt. Existing held-repeat catch-up advances the grid itself and resumes fresh
                        // selection once the target reaches retained history, without changing PTS or audio.
                        if (!bufferedWgcFrames.empty()) {
                            const int64_t oldestBufferedSlotQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                            if (ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                    oldestBufferedSlotQpc, playoutTargetQpc, targetIntervalTicks)) {
                                ++wgcUniformGridDebtHoldTotal;
                                const uint64_t oldestLeadUs = static_cast<uint64_t>(
                                    qpcToUs(oldestBufferedSlotQpc - playoutTargetQpc));
                                wgcUniformGridDebtLeadMaxUs = std::max(wgcUniformGridDebtLeadMaxUs, oldestLeadUs);
                                static uint64_t s_lastGridDebtHoldLogTick = 0;
                                const uint64_t nowGridDebtHoldTick = GetTickCount64();
                                if (nowGridDebtHoldTick - s_lastGridDebtHoldLogTick >= 1000) {
                                    s_lastGridDebtHoldLogTick = nowGridDebtHoldTick;
                                    LogWarn(
                                        "[WGC CFR] Uniform playout grid-debt sync hold: oldestLead=%lluus "
                                        "shortfall=%u buffered=%zu total=%llu (held-repeat catch-up advances the "
                                        "immutable CFR grid; visual/audio content targets and PTS unchanged)",
                                        static_cast<unsigned long long>(oldestLeadUs), outputShortfallTicks,
                                        bufferedWgcFrames.size(),
                                        static_cast<unsigned long long>(wgcUniformGridDebtHoldTotal));
                                }
                            }
                        }
                        const uint32_t uniformActiveDelaySoftLateTargetUs =
                            ce::capture_policy::GetWgcActiveDelaySoftLateTargetUs(targetIntervalTicks,
                                                                                  qpcFreq.QuadPart);
                        const bool uniformDeepUnderfeed = ce::capture_policy::IsWgcDeepUnderfeed(
                            outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille);
                        ce::capture_policy::WgcAdaptiveTelemetry uniformActiveDelayTelemetry{};
                        uniformActiveDelayTelemetry.outputFps = outputFps;
                        uniformActiveDelayTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
                        uniformActiveDelayTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
                        uniformActiveDelayTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
                        uniformActiveDelayTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
                        uniformActiveDelayTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
                        const uint32_t uniformWgcSourceJitterAvgUs =
                            media_main_g_WgcCap ? SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs()) : 0u;
                        const uint32_t uniformWgcPredictorJitterUs =
                            wgcInputPredictor.IsCalibrated() ? SaturatingToUint32(static_cast<uint64_t>(
                                                                   wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
                                                             : 0u;
                        uniformActiveDelayTelemetry.averageJitterUs =
                            std::max(uniformWgcSourceJitterAvgUs, uniformWgcPredictorJitterUs);
                        uniformActiveDelayTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
                        uniformActiveDelayTelemetry.bufferedWgcFrames = static_cast<uint32_t>(
                            std::min<size_t>(bufferedWgcFrames.size(), static_cast<size_t>(UINT32_MAX)));
                        const auto uniformRepeatClusterTicks = [&]() -> uint32_t {
                            return std::max<uint32_t>(
                                cadenceCounters.consecutiveDuplicateFrames,
                                cadenceCounters.holdTicksRunning > 1 ? (cadenceCounters.holdTicksRunning - 1) : 0);
                        };
                        const auto uniformDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            if (wgcDelayResidualWindowSamples > 0) {
                                return SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs /
                                                          wgcDelayResidualWindowSamples);
                            }
                            return wgcDelayResidualSamples > 0
                                       ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
                                       : 0u;
                        };
                        const auto uniformRawDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            if (wgcDelayRawResidualWindowSamples > 0) {
                                return SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs /
                                                          wgcDelayRawResidualWindowSamples);
                            }
                            return wgcDelayRawResidualSamples > 0
                                       ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
                                       : 0u;
                        };
                        const auto uniformDelayResidualP95Us = [&]() -> uint32_t {
                            const uint32_t windowP95 = wgcDelayResidualWindowP95Us();
                            return windowP95 > 0 ? windowP95 : wgcDelayResidualP95Us();
                        };
                        const auto uniformRawDelayResidualP95Us = [&]() -> uint32_t {
                            const uint32_t windowP95 = wgcDelayRawResidualWindowP95Us();
                            return windowP95 > 0 ? windowP95 : wgcDelayRawResidualP95Us();
                        };
                        const auto uniformDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return wgcDelayResidualWindowLateMaxUs > 0 ? wgcDelayResidualWindowLateMaxUs
                                                                       : wgcDelayResidualLateMaxUs;
                        };
                        const auto uniformRawDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return wgcDelayRawResidualWindowLateMaxUs > 0 ? wgcDelayRawResidualWindowLateMaxUs
                                                                          : wgcDelayRawResidualLateMaxUs;
                        };
                        const auto uniformCombinedDelayResidualAvgAbsUs = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualAvgAbsUs(), uniformRawDelayResidualAvgAbsUs());
                        };
                        const auto uniformCombinedDelayResidualP95Us = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualP95Us(), uniformRawDelayResidualP95Us());
                        };
                        const auto uniformCombinedDelayResidualLateMaxUs = [&]() -> uint32_t {
                            return std::max(uniformDelayResidualLateMaxUs(), uniformRawDelayResidualLateMaxUs());
                        };
                        const auto uniformCandidateHardSafeForTarget = [&](const QueuedFrame& candidate,
                                                                           int64_t targetQpc) -> bool {
                            if (targetQpc <= 0 || targetIntervalTicks <= 0 || candidate.timestamp <= 0 ||
                                GetFrameSelectionTimestamp(candidate) <= lastEmittedWgcSelectionQpc) {
                                return false;
                            }
                            return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
                                GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                                targetQpc, targetIntervalTicks, qpcFreq.QuadPart);
                        };
                        const auto uniformCandidateSoftSafeForTarget = [&](const QueuedFrame& candidate,
                                                                           int64_t targetQpc) -> bool {
                            if (!uniformCandidateHardSafeForTarget(candidate, targetQpc)) {
                                return false;
                            }
                            return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(
                                GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
                                targetQpc, targetIntervalTicks, qpcFreq.QuadPart, uniformActiveDelaySoftLateTargetUs);
                        };
                        const auto uniformHasHardSafeCandidateForTarget = [&](int64_t targetQpc) -> bool {
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (uniformCandidateHardSafeForTarget(candidate, targetQpc)) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        const auto uniformHasSoftSafeCandidateForTarget = [&](int64_t targetQpc) -> bool {
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (uniformCandidateSoftSafeForTarget(candidate, targetQpc)) {
                                    return true;
                                }
                            }
                            return false;
                        };
                        const auto uniformRepeatReserveSpanUs = [&]() -> uint32_t {
                            if (bufferedWgcFrames.size() < 2 || qpcFreq.QuadPart <= 0) {
                                return 0u;
                            }
                            const int64_t firstQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
                            const int64_t lastQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
                            if (firstQpc <= 0 || lastQpc <= firstQpc) {
                                return 0u;
                            }
                            return SaturatingToUint32(
                                static_cast<uint64_t>((lastQpc - firstQpc) * 1000000 / qpcFreq.QuadPart));
                        };
                        const auto uniformOldestSoftSafeAgeUs = [&](int64_t targetQpc) -> uint32_t {
                            if (selectionNowQpc.QuadPart <= 0 || qpcFreq.QuadPart <= 0) {
                                return 0u;
                            }
                            uint32_t oldestAgeUs = 0;
                            for (const QueuedFrame& candidate : bufferedWgcFrames) {
                                if (!uniformCandidateSoftSafeForTarget(candidate, targetQpc)) {
                                    continue;
                                }
                                const int64_t selectionTimestamp = GetFrameSelectionTimestamp(candidate);
                                if (selectionTimestamp <= 0 || selectionNowQpc.QuadPart <= selectionTimestamp) {
                                    continue;
                                }
                                oldestAgeUs = std::max(
                                    oldestAgeUs,
                                    SaturatingToUint32(static_cast<uint64_t>(
                                        (selectionNowQpc.QuadPart - selectionTimestamp) * 1000000 / qpcFreq.QuadPart)));
                            }
                            return oldestAgeUs;
                        };
                        const auto uniformActiveDelayWindowClassFor = [&](bool hardSafeCandidateAvailable) {
                            const bool activeDelaySourceRecovery =
                                wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                            return ce::capture_policy::ClassifyWgcActiveDelayWindow(
                                uniformActiveDelayTelemetry, wgcLowSourceModeActive, wgcLiveRecoveryModeActive,
                                wgcSourceStarvedCurrent, uniformDeepUnderfeed, activeDelaySourceRecovery,
                                hardSafeCandidateAvailable);
                        };
                        const auto uniformSyncDelayHoldSourceLimited = [&](bool softSafeCandidateAvailable) -> bool {
                            if (!softSafeCandidateAvailable) {
                                return true;
                            }
                            const bool activeDelaySourceRecovery =
                                wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
                            const bool sourceRecoveryWithoutSafeFrame =
                                activeDelaySourceRecovery && !softSafeCandidateAvailable;
                            return ce::capture_policy::IsWgcSyncDelayHoldSourceLimited(
                                outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille,
                                wgcSourceStarvedCurrent, wgcLowSourceModeActive, uniformDeepUnderfeed,
                                sourceRecoveryWithoutSafeFrame);
                        };
                        const auto recordUniformRepeatDiagnostics = [&](bool hardSafeCandidateAvailable,
                                                                        bool softSafeCandidateAvailable) {
                            ++wgcDelayUniformHoldWindow;
                            ++wgcDelayUniformHoldTotal;
                            const uint32_t repeatClusterTicks = uniformRepeatClusterTicks();
                            if (repeatClusterTicks > 0) {
                                ++wgcDelayRepeatClusterPressureWindow;
                                ++wgcDelayRepeatClusterPressureTotal;
                                wgcDelayRepeatClusterPressureWindowMaxTicks =
                                    std::max(wgcDelayRepeatClusterPressureWindowMaxTicks, repeatClusterTicks);
                                wgcDelayRepeatClusterPressureMaxTicks =
                                    std::max(wgcDelayRepeatClusterPressureMaxTicks, repeatClusterTicks);
                            }
                            ++wgcRepeatPolicyHoldCount;
                            ++wgcRepeatPolicyHoldTotal;
                            ++wgcSyncDelayHoldCount;
                            ++wgcSyncDelayHoldTotal;

                            const auto repeatWindowClass = uniformActiveDelayWindowClassFor(hardSafeCandidateAvailable);
                            switch (repeatWindowClass) {
                                case ce::capture_policy::WgcActiveDelayWindowClass::kHealthy:
                                    ++wgcDelayWindowHealthyRepeatWindow;
                                    ++wgcDelayWindowHealthyRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kRecoverableUnderfill:
                                    ++wgcDelayWindowRecoverableRepeatWindow;
                                    ++wgcDelayWindowRecoverableRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited:
                                    ++wgcDelayWindowSourceLimitedRepeatWindow;
                                    ++wgcDelayWindowSourceLimitedRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kHardSourceStall:
                                    ++wgcDelayWindowSourceLimitedRepeatWindow;
                                    ++wgcDelayWindowSourceLimitedRepeatTotal;
                                    ++wgcDelayWindowHardStallRepeatWindow;
                                    ++wgcDelayWindowHardStallRepeatTotal;
                                    break;
                                case ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery:
                                    ++wgcDelayWindowRecoverableRepeatWindow;
                                    ++wgcDelayWindowRecoverableRepeatTotal;
                                    ++wgcDelayWindowPostStallRepeatWindow;
                                    ++wgcDelayWindowPostStallRepeatTotal;
                                    break;
                            }
                            if (hardSafeCandidateAvailable) {
                                ++wgcDelayRepeatWithSafeCandidateWindow;
                                ++wgcDelayRepeatWithSafeCandidateTotal;
                            } else {
                                ++wgcDelayRepeatWithoutSafeCandidateWindow;
                                ++wgcDelayRepeatWithoutSafeCandidateTotal;
                            }
                            if (softSafeCandidateAvailable) {
                                ++wgcDelayRepeatWithSoftSafeCandidateWindow;
                                ++wgcDelayRepeatWithSoftSafeCandidateTotal;
                                const uint32_t oldestSoftSafeAgeUs = uniformOldestSoftSafeAgeUs(playoutTargetQpc);
                                wgcDelayOldestSoftSafeAgeWindowMaxUs =
                                    std::max(wgcDelayOldestSoftSafeAgeWindowMaxUs, oldestSoftSafeAgeUs);
                                wgcDelayOldestSoftSafeAgeMaxUs =
                                    std::max(wgcDelayOldestSoftSafeAgeMaxUs, oldestSoftSafeAgeUs);
                            } else {
                                ++wgcDelayRepeatWithoutSoftSafeCandidateWindow;
                                ++wgcDelayRepeatWithoutSoftSafeCandidateTotal;
                                ++wgcDelaySyncProtectedRepeatWindow;
                                ++wgcDelaySyncProtectedRepeatTotal;
                                if (hardSafeCandidateAvailable) {
                                    ++wgcDelayRepeatHardOnlyCandidateWindow;
                                    ++wgcDelayRepeatHardOnlyCandidateTotal;
                                }
                            }

                            const uint64_t nowTick = GetTickCount64();
                            if (!wgcActiveDelayRepeatClassKnown || repeatWindowClass != wgcActiveDelayLastRepeatClass) {
                                if (!wgcActiveDelayRepeatClassKnown ||
                                    nowTick - wgcActiveDelayLastRepeatClassLogTick >= 500) {
                                    LogInfo(
                                        "[WGC CFR] Uniform active-delay repeat state=%s hardSafe=%d softSafe=%d "
                                        "srcStarved=%d lowSource=%d deepUnderfeed=%d recoveryActive=%d buffered=%zu "
                                        "span=%uus residualP95=%uus rawP95=%uus softTarget=%uus",
                                        ce::capture_policy::WgcActiveDelayWindowClassToString(repeatWindowClass),
                                        hardSafeCandidateAvailable ? 1 : 0, softSafeCandidateAvailable ? 1 : 0,
                                        wgcSourceStarvedCurrent ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
                                        uniformDeepUnderfeed ? 1 : 0,
                                        wgcActiveDelaySourceRecoveryUntilTick > nowTick ? 1 : 0,
                                        bufferedWgcFrames.size(), uniformRepeatReserveSpanUs(),
                                        uniformCombinedDelayResidualP95Us(), uniformRawDelayResidualP95Us(),
                                        uniformActiveDelaySoftLateTargetUs);
                                    wgcActiveDelayLastRepeatClassLogTick = nowTick;
                                }
                                wgcActiveDelayRepeatClassKnown = true;
                                wgcActiveDelayLastRepeatClass = repeatWindowClass;
                            }

                            const bool syncDelayHoldSourceLimited =
                                uniformSyncDelayHoldSourceLimited(softSafeCandidateAvailable);
                            if (syncDelayHoldSourceLimited) {
                                ++wgcSyncDelaySourceLimitedHoldCount;
                                ++wgcSyncDelaySourceLimitedHoldTotal;
                                ++wgcSourceRepeatLowerBoundWindow;
                                ++wgcSourceRepeatLowerBoundTotal;
                                ++wgcDelaySourceLimitedRepeatWindow;
                                ++wgcDelaySourceLimitedRepeatTotal;
                                const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > nowTick;
                                if (activeDelaySourceRecovery && !wgcSourceStarvedCurrent && !wgcLowSourceModeActive &&
                                    !uniformDeepUnderfeed) {
                                    ++wgcSyncDelaySourceRecoveryHoldCount;
                                    ++wgcSyncDelaySourceRecoveryHoldTotal;
                                }
                            } else {
                                ++wgcSyncDelayPolicyHoldCount;
                                ++wgcSyncDelayPolicyHoldTotal;
                                ++wgcExcessRepeatWindow;
                                ++wgcExcessRepeatTotal;
                                ++wgcPolicyAddedRepeatWindow;
                                ++wgcPolicyAddedRepeatTotal;
                                if (repeatClusterTicks > 0) {
                                    ++wgcExcessRepeatClusterWindow;
                                    ++wgcExcessRepeatClusterTotal;
                                    wgcExcessRepeatClusterWindowMaxTicks =
                                        std::max(wgcExcessRepeatClusterWindowMaxTicks, repeatClusterTicks);
                                    wgcExcessRepeatClusterMaxTicks =
                                        std::max(wgcExcessRepeatClusterMaxTicks, repeatClusterTicks);
                                }
                            }

                            const uint32_t reserveDepth = SaturatingToUint32(bufferedWgcFrames.size());
                            wgcDelayRepeatReserveDepthWindowMax =
                                std::max(wgcDelayRepeatReserveDepthWindowMax, reserveDepth);
                            wgcDelayRepeatReserveDepthMax = std::max(wgcDelayRepeatReserveDepthMax, reserveDepth);
                            const uint32_t reserveSpanUs = uniformRepeatReserveSpanUs();
                            wgcDelayRepeatReserveSpanWindowMaxUs =
                                std::max(wgcDelayRepeatReserveSpanWindowMaxUs, reserveSpanUs);
                            wgcDelayRepeatReserveSpanMaxUs = std::max(wgcDelayRepeatReserveSpanMaxUs, reserveSpanUs);
                        };
                        const auto recordUniformWgcDelayRealizationForFrame = [&](const QueuedFrame& selectedFrame) {
                            const int64_t gridReferenceQpc =
                                scheduledOutputQpc > 0 ? scheduledOutputQpc : selectionNowQpc.QuadPart;
                            if (qpcFreq.QuadPart <= 0 || selectedFrame.timestamp <= 0 || gridReferenceQpc <= 0) {
                                return false;
                            }
                            const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
                            const int64_t predictedRealizedDelayUs =
                                ((gridReferenceQpc - GetFrameSelectionTimestamp(selectedFrame)) * 1000000) /
                                qpcFreq.QuadPart;
                            const int64_t predictedResidualUs = requestedDelayUs - predictedRealizedDelayUs;
                            const int64_t rawSelectionQpc = getWgcRawSelectionTimestamp(selectedFrame);
                            int64_t rawResidualUs = predictedResidualUs;
                            if (rawSelectionQpc > 0) {
                                const int64_t rawRealizedDelayUs =
                                    ((gridReferenceQpc - rawSelectionQpc) * 1000000) / qpcFreq.QuadPart;
                                rawResidualUs = requestedDelayUs - rawRealizedDelayUs;
                            }
                            return recordWgcDelayRealization(predictedResidualUs, rawResidualUs);
                        };
                        const auto recordUniformRepeatRescueRejection =
                            [&](const ce::capture_policy::WgcActiveDelayRelaxedCandidateScore& rescueScore,
                                ce::capture_policy::WgcActiveDelayWindowClass rescueWindowClass) {
                                switch (rescueScore.decision) {
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectSyncRisk:
                                        ++wgcDelayRepeatRescueRejectedSyncWindow;
                                        ++wgcDelayRepeatRescueRejectedSyncTotal;
                                        break;
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectResidualHeadroom:
                                        ++wgcDelayRepeatRescueRejectedHeadroomWindow;
                                        ++wgcDelayRepeatRescueRejectedHeadroomTotal;
                                        if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(
                                                rescueWindowClass) &&
                                            rescueScore.candidateLateResidualUs > uniformActiveDelaySoftLateTargetUs) {
                                            ++wgcDelayRepeatPromotionRejectedSoftWindow;
                                            ++wgcDelayRepeatPromotionRejectedSoftTotal;
                                        }
                                        break;
                                    case ce::capture_policy::WgcActiveDelayRelaxedDecision::kRejectRepeatCost:
                                        ++wgcDelayRepeatRescueRejectedCostWindow;
                                        ++wgcDelayRepeatRescueRejectedCostTotal;
                                        break;
                                    default:
                                        break;
                                }
                            };
                        if (!bufferedWgcFrames.empty()) {
                            uint32_t playoutStaleDrops = 0;
                            while (bufferedWgcFrames.size() > 1 && playoutTargetQpc > 0 &&
                                   ce::capture_policy::ShouldDropCfrFrontForNearerPlayout(
                                       GetFrameSelectionTimestamp(bufferedWgcFrames[0]),
                                       GetFrameSelectionTimestamp(bufferedWgcFrames[1]), playoutTargetQpc,
                                       playoutLeadToleranceQpc)) {
                                QueuedFrame staleFront = std::move(bufferedWgcFrames.front());
                                bufferedWgcFrames.pop_front();
                                ReleaseQueuedFrameTexture(staleFront);
                                ++wgcDropObsoleteCount;
                                ++playoutStaleDrops;
                            }
                            if (playoutStaleDrops > 0) {
                                // Age-based catch-up after a WGC delivery gap/burst: the audio-passed
                                // backlog is dropped (not replayed), so the realized delay stays pinned
                                // instead of rubber-banding. Expected/healthy under a bursty source;
                                // throttle the log so a busy window stays readable.
                                wgcDelayPaceCapTrimTotal += playoutStaleDrops;
                                wgcDelayPaceCapTrimWindow += playoutStaleDrops;
                                const DWORD capTrimNowTick = GetTickCount();
                                if (playoutStaleDrops >= 3 && capTrimNowTick - wgcDelayPaceCapTrimLastLogTick >= 1000) {
                                    LogWarn(
                                        "[WGC CFR] active-delay playout catch-up: dropped %u audio-passed frame(s) "
                                        "after a WGC delivery gap depthAfter=%zu target=%lldus (bursty source "
                                        "delivery, NOT a game render hitch; realized content delay pinned, A/V sync "
                                        "preserved)",
                                        playoutStaleDrops, bufferedWgcFrames.size(),
                                        static_cast<long long>(qpcToUs(getWgcEffectiveContentDelayQpc())));
                                    wgcDelayPaceCapTrimLastLogTick = capTrimNowTick;
                                }
                            }
                            const auto playout =
                                playoutTargetQpc > 0
                                    ? ce::capture_policy::DecideCfrNearestPlayout(
                                          GetFrameSelectionTimestamp(bufferedWgcFrames.front()), playoutTargetQpc,
                                          playoutLeadToleranceQpc, lastEmittedWgcSelectionQpc)
                                    : ce::capture_policy::WgcNearestPlayoutDecision{/*emit=*/true, /*hold=*/false};
                            bool uniformRepeatRescueAccepted = false;
                            ce::capture_policy::WgcActiveDelayRelaxedCandidateScore uniformRepeatRescueScore{};
                            ce::capture_policy::WgcActiveDelayWindowClass uniformRepeatRescueClass =
                                ce::capture_policy::WgcActiveDelayWindowClass::kSourceLimited;
                            if (playout.hold && playoutTargetQpc > 0 && !bufferedWgcFrames.empty() && media_main_g_HasLastFrame &&
                                !media_main_g_LastFrame.isInjectMode) {
                                ++wgcDelayRepeatRescueAttemptWindow;
                                ++wgcDelayRepeatRescueAttemptTotal;
                                ++wgcDelayRepeatPromotionAttemptWindow;
                                ++wgcDelayRepeatPromotionAttemptTotal;
                                const QueuedFrame& rescueCandidate = bufferedWgcFrames.front();
                                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(media_main_g_LastFrame);
                                uniformRepeatRescueClass = uniformActiveDelayWindowClassFor(true);
                                uniformRepeatRescueScore = ce::capture_policy::ScoreWgcActiveDelayRepeatRescueCandidate(
                                    GetFrameSelectionTimestamp(rescueCandidate),
                                    getWgcRawSelectionTimestamp(rescueCandidate), repeatSelectionTimestamp,
                                    playoutTargetQpc, targetIntervalTicks, qpcFreq.QuadPart,
                                    uniformRepeatClusterTicks(), uniformCombinedDelayResidualAvgAbsUs(),
                                    uniformCombinedDelayResidualP95Us(), uniformCombinedDelayResidualLateMaxUs(),
                                    uniformRepeatRescueClass, uniformActiveDelaySoftLateTargetUs);
                                uniformRepeatRescueAccepted = uniformRepeatRescueScore.Accepted();
                                if (uniformRepeatRescueAccepted) {
                                    ++wgcDelayRepeatRescueSuccessWindow;
                                    ++wgcDelayRepeatRescueSuccessTotal;
                                    ++wgcDelayRepeatPromotedBeforeRepeatWindow;
                                    ++wgcDelayRepeatPromotedBeforeRepeatTotal;
                                    ++wgcDelayRepeatSafeAfterPromotionWindow;
                                    ++wgcDelayRepeatSafeAfterPromotionTotal;
                                    ++wgcDelayOlderFrameAvoidedRepeatWindow;
                                    ++wgcDelayOlderFrameAvoidedRepeatTotal;
                                    if (uniformRepeatRescueScore.candidateLateResidualUs >
                                        uniformActiveDelaySoftLateTargetUs) {
                                        ++wgcDelayNearCapAcceptedWindow;
                                        ++wgcDelayNearCapAcceptedTotal;
                                    }
                                    if (!ce::capture_policy::IsWgcActiveDelaySourceLimitedClass(
                                            uniformRepeatRescueClass) &&
                                        uniformRepeatRescueScore.candidateLateResidualUs >
                                            uniformActiveDelaySoftLateTargetUs) {
                                        ++wgcDelaySoftLateAcceptedWindow;
                                        ++wgcDelaySoftLateAcceptedTotal;
                                    }
                                } else {
                                    recordUniformRepeatRescueRejection(uniformRepeatRescueScore,
                                                                       uniformRepeatRescueClass);
                                }
                            }
                            if ((playout.emit || uniformRepeatRescueAccepted) && !bufferedWgcFrames.empty()) {
                                frame = std::move(bufferedWgcFrames.front());
                                bufferedWgcFrames.pop_front();
                                popped = true;
                                if (frame.duplicateSourceTimestamp) {
                                    ++wgcSelectDuplicateSourceCount;
                                } else {
                                    ++wgcSelectFreshCount;
                                }
                                ++wgcDelayUniformCadenceWindow;
                                ++wgcDelayUniformCadenceTotal;
                                // Measure the realized content delay against the GRID playout reference
                                // (scheduledOutputQpc == the immutable output slot), NOT wall-clock
                                // `selectionNowQpc`. The emitted frame lands at a fixed PTS slot and the
                                // co-timed audio is anchored to the same grid, so the file's true A/V
                                // content offset is `gridSlotTime - frame.timestamp`, independent of how
                                // late the encoder thread happened to wake. Using `selectionNowQpc` here
                                // folded encoder-thread scheduling jitter (30-88 ms late wakes under
                                // 100% GPU / network-drive mux I/O in 20260626_050554) into the metric,
                                // inflating realizedDelay to ~108 ms and tripping
                                // wgc_active_delay_realized_delay_unstable even though the content placed
                                // in each slot was grid-correct. Thread-wake jitter is reported
                                // separately as SchedSel/SelMax; this counter must stay content-honest.
                                wgcDelayRealizationRecordedThisTick = recordUniformWgcDelayRealizationForFrame(frame);
                                if (uniformRepeatRescueAccepted) {
                                    static uint32_t s_uniformRepeatRescueLogCount = 0;
                                    if (s_uniformRepeatRescueLogCount < 5) {
                                        ++s_uniformRepeatRescueLogCount;
                                        const int64_t candidateLeadUs =
                                            qpcFreq.QuadPart > 0
                                                ? ((GetFrameSelectionTimestamp(frame) - playoutTargetQpc) * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        const int64_t candidateDamageUs =
                                            qpcFreq.QuadPart > 0
                                                ? (uniformRepeatRescueScore.candidateDamageQpc * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        const int64_t repeatDamageUs =
                                            qpcFreq.QuadPart > 0
                                                ? (uniformRepeatRescueScore.repeatDamageQpc * 1000000) /
                                                      qpcFreq.QuadPart
                                                : 0;
                                        LogInfo(
                                            "[WGC CFR] Uniform playout rescued repeat with sync-safe frame: "
                                            "decision=%s lead=%lldus lateResidual=%uus candidateDamage=%lldus "
                                            "repeatDamage=%lldus buffered=%zu class=%s softTarget=%uus",
                                            ce::capture_policy::WgcActiveDelayRelaxedDecisionToString(
                                                uniformRepeatRescueScore.decision),
                                            static_cast<long long>(candidateLeadUs),
                                            uniformRepeatRescueScore.candidateLateResidualUs,
                                            static_cast<long long>(candidateDamageUs),
                                            static_cast<long long>(repeatDamageUs), bufferedWgcFrames.size(),
                                            ce::capture_policy::WgcActiveDelayWindowClassToString(
                                                uniformRepeatRescueClass),
                                            uniformActiveDelaySoftLateTargetUs);
                                    }
                                }
                            } else if (playout.hold) {
                                const bool uniformHardSafeCandidate =
                                    uniformHasHardSafeCandidateForTarget(playoutTargetQpc);
                                const bool uniformSoftSafeCandidate =
                                    uniformHasSoftSafeCandidateForTarget(playoutTargetQpc);
                                recordUniformRepeatDiagnostics(uniformHardSafeCandidate, uniformSoftSafeCandidate);
                                const bool uniformHoldSourceLimited =
                                    uniformSyncDelayHoldSourceLimited(uniformSoftSafeCandidate);
                                const bool uniformHoldSourceAtOrAboveCfr =
                                    media_main_g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps);
                                if (!uniformHoldSourceLimited) {
                                    static uint32_t s_uniformPolicyHoldLogCount = 0;
                                    if (s_uniformPolicyHoldLogCount < 5) {
                                        ++s_uniformPolicyHoldLogCount;
                                        LogWarn(
                                            "[WGC CFR] Uniform playout held while source was at/above CFR target: "
                                            "inputMin=%u/%u outputFps=%u buffered=%zu hardSafe=%d softSafe=%d "
                                            "retained=%u/%u dropIngress=%u (policy repeat; not source-limited)",
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin250Fps() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u);
                                    }
                                } else if (uniformHoldSourceAtOrAboveCfr && !uniformSoftSafeCandidate) {
                                    static uint32_t s_uniformSyncProtectedHighSourceLogCount = 0;
                                    if (s_uniformSyncProtectedHighSourceLogCount < 5) {
                                        ++s_uniformSyncProtectedHighSourceLogCount;
                                        int64_t leadUs = 0;
                                        if (!bufferedWgcFrames.empty() && qpcFreq.QuadPart > 0 &&
                                            GetFrameSelectionTimestamp(bufferedWgcFrames.front()) > playoutTargetQpc) {
                                            leadUs = ((GetFrameSelectionTimestamp(bufferedWgcFrames.front()) -
                                                       playoutTargetQpc) *
                                                      1000000) /
                                                     qpcFreq.QuadPart;
                                        }
                                        LogInfo(
                                            "[WGC CFR] Uniform playout sync-protected repeat while source was "
                                            "at/above CFR target: lead=%lldus inputMin=%u/%u outputFps=%u "
                                            "buffered=%zu hardSafe=%d softSafe=%d span=%uus retained=%u/%u "
                                            "dropIngress=%u (no sync-safe frame for this slot; CFR/audio held)",
                                            static_cast<long long>(leadUs),
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin250Fps() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0, uniformRepeatReserveSpanUs(),
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            media_main_g_WgcCap ? media_main_g_WgcCap->GetIngressDecimatedCount() : 0u);
                                    }
                                }
                            }
                            // playout.hold -> leave the buffer intact; the encoder repeats the last
                            // emitted frame (an even source-limited / delivery-gap repeat).
                        }
                    } else if (tryPopBufferedWgcFrameForTarget(effectiveSelectionTargetQpc, liveSelectionTargetQpc,
                                                               selectionNowQpc.QuadPart,
                                                               wgcSelectionDelayAppliedThisTick, &frame)) {
                        popped = true;
                    }
                }

                updateWgcIngressPressure(popped ? "post-select" : "post-hold");
            } else {
                // VFR: keep the existing lowest-latency newest-frame sampling.
                QueuedFrame temp;
                while (media_main_g_FrameQueue.Pop(temp, 0)) {
                    if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "WGC VFR queue", true);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        } else {
            if (!bufferedWgcFrames.empty()) {
                ClearBufferedWgcFrames();
            }
            if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && !bufferedInjectFrames.empty()) {
                ClearBufferedInjectFrames();
            }

            if (!config.video.useVFR) {
                // Keep multiple inject frames in reserve so the encoder usually works on
                // textures whose GPU copy has already completed instead of blocking on the
                // newest frame's fence.
                drainedInjectFrames.clear();
                QueuedFrame temp;
                while (media_main_g_FrameQueue.Pop(temp, 0)) {
                    if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject CFR queue", true);
                        continue;
                    }
                    if (temp.isInjectMode && temp.timestamp > 0) {
                        injectInputPredictor.Update(temp.timestamp, qpcFreq.QuadPart);
                        observeCaptureSyncPhaseSource("inject", injectCfrPhaseLock, temp.timestamp);
                    }
                    drainedInjectFrames.push_back(std::move(temp));
                }

                for (auto& drainedFrame : drainedInjectFrames) {
                    bufferedInjectFrames.push_back(std::move(drainedFrame));
                }

                // Track frame arrival rate for source-health telemetry. Use a short
                // window during warmup/startup so the EMA is already calibrated
                // when recording goes live, then widen to half-second for
                // steady-state stability. Timestamp-target playout, not this EMA,
                // decides which source frame represents each CFR output slot.
                pacingInputThisWindow += (uint32_t)drainedInjectFrames.size();
                pacingTicksThisWindow++;
                const uint32_t pacingWindowSize = (pacingEmaUpdates < 6) ? std::max((uint32_t)config.video.fps / 8, 8u)
                                                                         : (uint32_t)config.video.fps / 2;
                if (pacingTicksThisWindow >= pacingWindowSize) {
                    double measuredRate = (double)pacingInputThisWindow / (double)pacingTicksThisWindow;
                    // Adaptive alpha: converge fast during startup (0.7), steady-state (0.5),
                    // or when FPS transitions are detected (>20% deviation -> 0.8) so
                    // diagnostics follow rapid source-rate changes promptly.
                    double alpha = 0.5;
                    if (pacingEmaUpdates < 6) {
                        alpha = 0.7;
                    } else if (smoothedInputPerTick > 0.01) {
                        double deviation = std::abs(measuredRate - smoothedInputPerTick) / smoothedInputPerTick;
                        if (deviation > 0.20) {
                            alpha = 0.8;
                        }
                    }
                    smoothedInputPerTick = smoothedInputPerTick * (1.0 - alpha) + measuredRate * alpha;
                    pacingInputThisWindow = 0;
                    pacingTicksThisWindow = 0;
                    ++pacingEmaUpdates;
                }

                const size_t injectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                    config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                // Only the physical GPU/fence safety tail is protected from selection. The A/V
                // content delay is a timestamp target below; treating it as additional protected
                // frames hides every useful candidate at normal queue depth and creates trim/repeat
                // churn even when the game supplies one fresh frame per CFR tick.
                const size_t protectedInjectTailFrames =
                    ce::capture_policy::GetMinBufferedInjectFrames(injectReserveFrames, recordingOutputLive);
                const size_t maxBufferedInjectFrames =
                    std::max(ce::capture_policy::GetMaxBufferedInjectFrames(injectReserveFrames, recordingOutputLive,
                                                                            recordingLiveTick, GetTickCount64()),
                             injectReserveFrames + injectContentDelayFrames + 2);
                maxBufferedInjectDepthSinceLog = std::max(maxBufferedInjectDepthSinceLog, bufferedInjectFrames.size());
                uint32_t trimmedInjectFrames = 0;
                while (bufferedInjectFrames.size() > maxBufferedInjectFrames) {
                    QueuedFrame staleFrame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    DiscardQueuedFrame(staleFrame);
                    ++trimmedInjectFrames;
                    ++injectBufferCapTrimTotal;
                }
                if (trimmedInjectFrames > 0) {
                    pendingInjectTrimmedLogCount += trimmedInjectFrames;
                    media_main_g_InjectBufferedTrimmedFrames.fetch_add(trimmedInjectFrames, std::memory_order_relaxed);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.injectTrimmedFrames.fetch_add(trimmedInjectFrames,
                                                                                 std::memory_order_relaxed);
                    }
                    if (lastDeferredLineage.IsValid() && !bufferedInjectFrames.empty() &&
                        !std::any_of(bufferedInjectFrames.begin(), bufferedInjectFrames.end(),
                                     [&](const QueuedFrame& candidate) {
                                         return MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                                     })) {
                        lastDeferredLineage = {};
                    }
                }
                DWORD now = GetTickCount();
                if (pendingInjectTrimmedLogCount > 0 && now - lastInjectTrimLog >= 1000) {
                    LogInfo(
                        "[EncoderThread] Trimmed %u inject frame(s) at the hard buffer cap "
                        "(peak=%zu cap=%zu fenceTail=%zu delayFrames=%zu total=%llu)",
                        pendingInjectTrimmedLogCount, maxBufferedInjectDepthSinceLog, maxBufferedInjectFrames,
                        protectedInjectTailFrames, injectContentDelayFrames,
                        static_cast<unsigned long long>(injectBufferCapTrimTotal));
                    pendingInjectTrimmedLogCount = 0;
                    maxBufferedInjectDepthSinceLog = bufferedInjectFrames.size();
                    lastInjectTrimLog = now;
                }

                auto recordInjectTargetDrop = [&](QueuedFrame& stale) {
                    DiscardQueuedFrame(stale);
                    media_main_g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1, std::memory_order_relaxed);
                    }
                    ++injectTargetSupersededThisWindow;
                    ++injectTargetSupersededTotal;
                };
                auto eligibleInjectFrameCount = [&]() -> size_t {
                    return bufferedInjectFrames.size() > protectedInjectTailFrames
                               ? bufferedInjectFrames.size() - protectedInjectTailFrames
                               : 0;
                };
                auto isFreshInjectCandidate = [&](const QueuedFrame& candidate) {
                    return ce::capture_policy::IsInjectFrameFreshAfterLastEmission(candidate.timestamp,
                                                                                   lastEmittedInjectSourceQpc);
                };

                // Remove only frames that can never be emitted again. Unlike the old wall-age trim,
                // this is relative to committed source lineage and cannot delete an intentional
                // delayed frame merely because the encoder thread is currently later than it.
                while (eligibleInjectFrameCount() > 0 && !isFreshInjectCandidate(bufferedInjectFrames.front())) {
                    QueuedFrame obsolete = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    recordInjectTargetDrop(obsolete);
                }

                if (!media_main_g_EncoderRunning && !bufferedInjectFrames.empty()) {
                    frame = std::move(bufferedInjectFrames.front());
                    bufferedInjectFrames.pop_front();
                    popped = true;
                    lastDeferredLineage = {};
                } else if (!recordingOutputLive || encoderGridStartQpc <= 0 || targetIntervalTicks <= 0) {
                    // Warmup/startup: the readiness gate below builds the content-delay history. Pop
                    // the oldest eligible source so the eventual first live frame is causal.
                    if (eligibleInjectFrameCount() > 0) {
                        frame = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        popped = true;
                        lastDeferredLineage = {};
                    }
                } else {
                    const size_t availableCount = eligibleInjectFrameCount();
                    const int64_t liveTargetQpc =
                        scheduledOutputQpc > 0
                            ? scheduledOutputQpc
                            : ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks);
                    const int64_t basePlayoutTargetQpc =
                        ComputeDelayedContentGridStartQpc(liveTargetQpc, avContentDelayQpc);
                    const int64_t phaseReferenceQpc =
                        bufferedInjectFrames.empty() ? 0 : bufferedInjectFrames.back().timestamp;
                    const int64_t playoutTargetQpc = applyCaptureSyncPhaseTarget(
                        "inject", injectCfrPhaseLock, basePlayoutTargetQpc, phaseReferenceQpc);
                    const int64_t leadToleranceQpc =
                        ce::capture_policy::GetInjectCfrSelectionLeadToleranceQpc(targetIntervalTicks);
                    auto isAllowedCandidate = [&](const QueuedFrame& candidate) {
                        return isFreshInjectCandidate(candidate) &&
                               !MatchesInjectFrameLineage(candidate, lastDeferredLineage);
                    };
                    size_t bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                      playoutTargetQpc, isAllowedCandidate);
                    bool usedDeferredFallback = false;
                    if (bestIdx >= availableCount) {
                        bestIdx = SelectFrameClosestToTimestampIf(bufferedInjectFrames, availableCount,
                                                                  playoutTargetQpc, isFreshInjectCandidate);
                        usedDeferredFallback = lastDeferredLineage.IsValid() && bestIdx < availableCount;
                    }

                    if (bestIdx < availableCount) {
                        const int64_t selectedTimestamp = bufferedInjectFrames[bestIdx].timestamp;
                        const auto decision = ce::capture_policy::DecideCfrNearestPlayout(
                            selectedTimestamp, playoutTargetQpc, leadToleranceQpc, lastEmittedInjectSourceQpc);
                        if (decision.emit) {
                            if (bestIdx > 0) {
                                ++selectionLogCounter;
                                if (selectionLogCounter <= 12 || (selectionLogCounter % 240) == 0) {
                                    LogInfo(
                                        "[EncoderThread] Inject target select tick=%lld targetQpc=%lld chose idx=%zu "
                                        "frame=%u tex=%d ts=%lld oldest=%lld avail=%zu fenceTail=%zu "
                                        "delayFrames=%zu delayUs=%lld%s",
                                        static_cast<long long>(selectionGridTick),
                                        static_cast<long long>(playoutTargetQpc), bestIdx,
                                        bufferedInjectFrames[bestIdx].frameIndex,
                                        bufferedInjectFrames[bestIdx].textureIndex,
                                        static_cast<long long>(selectedTimestamp),
                                        static_cast<long long>(bufferedInjectFrames.front().timestamp), availableCount,
                                        protectedInjectTailFrames, injectContentDelayFrames,
                                        static_cast<long long>(qpcToUs(avContentDelayQpc)),
                                        usedDeferredFallback ? " fallback=deferred-only" : "");
                                }
                            }
                            for (size_t i = 0; i < bestIdx; ++i) {
                                QueuedFrame superseded = std::move(bufferedInjectFrames.front());
                                bufferedInjectFrames.pop_front();
                                recordInjectTargetDrop(superseded);
                            }
                            frame = std::move(bufferedInjectFrames.front());
                            bufferedInjectFrames.pop_front();
                            popped = true;
                            lastDeferredLineage = {};
                            ++injectTargetSelectThisWindow;
                            ++injectTargetSelectTotal;
                            if (qpcFreq.QuadPart > 0) {
                                const uint64_t residualUs =
                                    ce::capture_policy::GetCfrTimestampDistanceQpc(selectedTimestamp,
                                                                                  playoutTargetQpc) *
                                    1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
                                injectTargetResidualMaxUs =
                                    std::max(injectTargetResidualMaxUs, SaturatingToUint32(residualUs));
                            }
                        } else if (decision.hold) {
                            ++injectTargetHoldThisWindow;
                            ++injectTargetHoldTotal;
                            ++injectTargetHoldWithCandidateThisWindow;
                            ++injectTargetHoldWithCandidateTotal;
                        }
                    } else {
                        ++injectTargetHoldThisWindow;
                        ++injectTargetHoldTotal;
                    }
                }
            } else {
                // VFR: keep the existing newest-frame sampling for the lowest latency.
                QueuedFrame temp;
                while (media_main_g_FrameQueue.Pop(temp, 0)) {
                    if (media_main_g_RejectInjectFrames.load(std::memory_order_acquire) && temp.isInjectMode) {
                        DiscardQueuedFrame(temp);
                        continue;
                    }
                    if (!ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, temp.isInjectMode)) {
                        discardActivePathMismatchFrame(temp, "inject VFR queue", true);
                        continue;
                    }
                    if (popped && !frame.isInjectMode && frame.texture) {
                        frame.texture->Release();
                    }
                    frame = std::move(temp);
                    popped = true;
                }
            }
        }

        QueuedFrame* frameToProcess = nullptr;
        bool isDuplicate = false;
        bool duplicateFromDeferred = false;
        bool duplicateFromTimerRebase = false;
        bool wantsTrueRepeatLastFrame = false;

        if (popped && !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, frame.isInjectMode)) {
            discardActivePathMismatchFrame(frame, "selected frame", true);
            popped = false;
        }

        const bool canPreserveLastFrameAcrossPathHandoff =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame) &&
            MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
        if (media_main_g_HasLastFrame &&
            !ce::capture_policy::ShouldAcceptFrameForActiveCapturePath(useScreenGrab, media_main_g_LastFrame.isInjectMode) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            discardActivePathMismatchFrame(media_main_g_LastFrame, "cached last frame", false);
            media_main_g_HasLastFrame = false;
        }

        if (popped && frame.isInjectMode && media_main_g_RejectInjectFrames.load(std::memory_order_acquire)) {
            DiscardQueuedFrame(frame);
            popped = false;
        }

        if (media_main_g_HasLastFrame && media_main_g_LastFrame.isInjectMode && media_main_g_RejectInjectFrames.load(std::memory_order_acquire) &&
            !canPreserveLastFrameAcrossPathHandoff) {
            media_main_g_LastFrame = QueuedFrame{};
            media_main_g_HasLastFrame = false;
        }

        const bool hasRepeatLastFramePath =
            !config.video.useVFR &&
            ((useScreenGrab && MediaEngine_RepeatLastFrameWithTimeline) || MediaEngine_RepeatLastFrame);
        auto selectCursorStateForScheduledQpc = [&](int64_t scheduledQpc, const QueuedFrame& referenceFrame,
                                                    const char* outputKind) {
            ce::cursor::CaptureState cursorState = referenceFrame.cursorState;
            if (config.video.captureCursor && scheduledQpc > 0) {
                const uint32_t captureWidth = referenceFrame.cursorState.captureWidth != 0
                                                  ? referenceFrame.cursorState.captureWidth
                                                  : referenceFrame.width;
                const uint32_t captureHeight = referenceFrame.cursorState.captureHeight != 0
                                                   ? referenceFrame.cursorState.captureHeight
                                                   : referenceFrame.height;
                const bool cursorEmbedded = useScreenGrab && referenceFrame.wgcCursorEmbedded;
                bool useDxgiPointerTimeline = false;
                if (useScreenGrab) {
                    const auto capture = media_main_g_WgcCap.Read();
                    useDxgiPointerTimeline =
                        capture && capture->IsUsingDesktopDuplication() &&
                        media_main_g_DxgiCursorTimelinePublished.load(std::memory_order_acquire) != 0;
                }
                // Source-embedded ownership belongs to the delayed reference
                // frame, not to this live pointer sample. Publishing it here
                // would delay suppression a second time and hide the cursor
                // after an embedded -> hardware-plane transition.
                ce::cursor::Timeline& timeline = useScreenGrab ? media_main_g_WgcCursorTimeline : media_main_g_InjectCursorTimeline;
                ce::cursor::CaptureState liveState;
                if (!useDxgiPointerTimeline) {
                    liveState =
                        CaptureCursorSnapshot(scheduledQpc, referenceFrame.captureLeft, referenceFrame.captureTop,
                                              captureWidth, captureHeight, false);
                    timeline.Publish(liveState);
                }
                const int64_t cursorTargetQpc =
                    useScreenGrab ? std::max<int64_t>(1, scheduledQpc - getWgcEffectiveContentDelayQpc())
                                  : scheduledQpc;
                if (!timeline.SelectAtOrBefore(cursorTargetQpc, &cursorState)) {
                    cursorState = useDxgiPointerTimeline ? referenceFrame.cursorState : liveState;
                }
                // Pixel ownership is authoritative for exactly this selected
                // source frame. Remove stale source ownership from the cursor
                // history when the selected frame has a separate hardware
                // pointer, while preserving a real CURSOR_SUPPRESSED state.
                cursorState.SetSourceEmbedded(cursorEmbedded);

                static uint64_t s_cursorTimelineLogCount = 0;
                ++s_cursorTimelineLogCount;
                if (s_cursorTimelineLogCount <= 5 || (s_cursorTimelineLogCount % 600ull) == 0ull) {
                    LogInfo(
                        "[Cursor] CFR timeline backend=%s output=%s scheduled=%lld target=%lld source=%lld "
                        "selected=%lld observed=%lld deltaUs=%lld dpi=%u size=%ux%u bounds=(%d,%d %ux%u) "
                        "visible=%d embedded=%d fallback=%d coord=%s samples=%s",
                        useScreenGrab ? "screen-grab" : "inject", outputKind ? outputKind : "unknown",
                        static_cast<long long>(scheduledQpc), static_cast<long long>(cursorTargetQpc),
                        static_cast<long long>(referenceFrame.cursorState.associationQpc),
                        static_cast<long long>(cursorState.associationQpc),
                        static_cast<long long>(cursorState.observedQpc),
                        static_cast<long long>(qpcFreq.QuadPart > 0
                                                   ? ((cursorTargetQpc - cursorState.associationQpc) * 1000000) /
                                                         qpcFreq.QuadPart
                                                   : 0),
                        cursorState.dpi, cursorState.requestedWidth, cursorState.requestedHeight,
                        cursorState.captureLeft, cursorState.captureTop, cursorState.captureWidth,
                        cursorState.captureHeight, cursorState.IsVisible() ? 1 : 0,
                        cursorState.IsSourceEmbedded() ? 1 : 0,
                        (cursorState.flags & ce::cursor::kStateHandleVisibilityFallback) != 0 ? 1 : 0,
                        cursorState.PositionIsShapeTopLeft() ? "shape-top-left" : "hotspot",
                        useDxgiPointerTimeline ? "dxgi-qpc" : "grid-query");
                }
            }
            return cursorState;
        };
        struct ScreenGrabPrivacyContext {
            HWND targetWindow = nullptr;
            HMONITOR targetMonitor = nullptr;
            bool stableCaptureTarget = false;
            ce::screen_grab_privacy::FullscreenFocusSnapshot focus;
        };
        auto sampleScreenGrabPrivacyContext = [&]() {
            ScreenGrabPrivacyContext context;
            HWND confirmedWindow = nullptr;
            HMONITOR confirmedMonitor = nullptr;
            const auto captureBefore = media_main_g_WgcCap.Read();
            if (captureBefore) {
                captureBefore->GetTargetIdentity(&context.targetWindow, &context.targetMonitor);
            }
            context.focus = ce::screen_grab_privacy::CaptureStableFullscreenFocus();
            const auto captureAfter = media_main_g_WgcCap.Read();
            if (captureAfter) {
                captureAfter->GetTargetIdentity(&confirmedWindow, &confirmedMonitor);
            }
            context.stableCaptureTarget = captureBefore && captureAfter && captureBefore.get() == captureAfter.get() &&
                                          context.targetWindow == confirmedWindow &&
                                          context.targetMonitor == confirmedMonitor;
            return context;
        };
        // Warmup frames never reach the file, but the WGC/DXGI look-ahead reservoir they
        // build is handed to the live output intact. Tracking focus from the first warmup
        // tick opens the verified-focus interval before that reservoir content is captured,
        // so the live handoff no longer has to blank a reservoir it just filled.
        auto observeScreenGrabPrivacyWarmup = [&]() {
            if (!privacyRuntime.IsEnabled()) {
                return;
            }
            const auto context = sampleScreenGrabPrivacyContext();
            privacyRuntime.ObserveWarmup(useScreenGrab, context.targetWindow, context.targetMonitor,
                                         context.stableCaptureTarget, context.focus);
        };
        auto evaluateScreenGrabPrivacy = [&](const QueuedFrame* freshFrame) {
            if (!privacyRuntime.IsEnabled()) {
                return ce::screen_grab_privacy::GateDecision{};
            }
            const auto context = sampleScreenGrabPrivacyContext();
            const int64_t freshFrameQpc =
                freshFrame ? (freshFrame->rawTimestamp > 0 ? freshFrame->rawTimestamp : freshFrame->timestamp) : 0;
            return privacyRuntime.Evaluate(useScreenGrab, context.targetWindow, context.targetMonitor,
                                           context.stableCaptureTarget, context.focus, freshFrame != nullptr,
                                           freshFrameQpc);
        };
        auto requestPrivacyFailClosedStop = [&](const char* reason) {
            LogError(
                "[PrivacyBlackout] FAIL-CLOSED: %s; cached video is invalidated and recording will stop rather than "
                "expose captured pixels",
                reason ? reason : "opaque-black submission failed");
            privacyRuntime.ResetSource();
            media_main_g_PrivacyFailClosedStopRequested.store(true, std::memory_order_release);
            media_main_g_EncoderRunning.store(false, std::memory_order_release);
        };
        auto submitPrivacyBlackFrame = [&](const QueuedFrame& referenceFrame, int64_t mediaTimestampQpc,
                                           int64_t scheduledQpc, int64_t timelineElapsedUs) {
            if (!privacyRuntime.SubmitBlack(referenceFrame.texture, referenceFrame.isHDR, mediaTimestampQpc,
                                            scheduledQpc, timelineElapsedUs, useScreenGrab && !config.video.useVFR)) {
                requestPrivacyFailClosedStop("opaque-black frame submission failed");
                return false;
            }
            return true;
        };
        auto repeatLastFrameForScheduledQpc = [&](int64_t scheduledQpc) {
            if (useScreenGrab && privacyRuntime.IsEnabled()) {
                const auto privacyDecision = evaluateScreenGrabPrivacy(nullptr);
                if (privacyDecision.useBlackFrame) {
                    if (!media_main_g_HasLastFrame || media_main_g_LastFrame.isInjectMode) {
                        requestPrivacyFailClosedStop("no screen-grab texture is available for an opaque-black tick");
                        return false;
                    }
                    return submitPrivacyBlackFrame(media_main_g_LastFrame, scheduledQpc,
                                                   scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc));
                }
            }
            ce::cursor::CaptureState cursorState;
            if (config.video.captureCursor && media_main_g_HasLastFrame && !privacyRuntime.RepeatCacheIsBlack()) {
                cursorState = selectCursorStateForScheduledQpc(scheduledQpc, media_main_g_LastFrame, "repeat");
            }
            bool succeeded = false;
            if (useScreenGrab && !config.video.useVFR && MediaEngine_RepeatLastFrameWithTimeline) {
                succeeded = MediaEngine_RepeatLastFrameWithTimeline(
                    scheduledQpc, computeLiveTimelineElapsedUs(scheduledQpc), &cursorState);
            } else {
                succeeded = MediaEngine_RepeatLastFrame && MediaEngine_RepeatLastFrame(scheduledQpc, &cursorState);
            }
            if (succeeded && useScreenGrab && privacyRuntime.IsEnabled()) {
                privacyRuntime.CommitRepeatOutput();
            }
            return succeeded;
        };
        auto recoverScheduledFreshEncodeFailure = [&](bool scheduledCfrTick, bool freshEncodeSucceeded,
                                                      bool freshEncodeDeferred, int64_t scheduledQpc,
                                                      const QueuedFrame* failedFrame, const char* context) {
            const bool repeatCacheAvailable = MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (!ce::capture_policy::ShouldRepeatAfterScheduledFreshEncodeFailure(
                    scheduledCfrTick, freshEncodeSucceeded, freshEncodeDeferred, hasRepeatLastFramePath,
                    repeatCacheAvailable)) {
                return false;
            }

            // The failed WGC attempt may have changed cursor suppression to
            // match pixels that were never emitted. Restore the metadata that
            // belongs to the cached successful source frame before repeating.
            if (failedFrame && !failedFrame->isInjectMode && hasSuccessfulWgcCursorMetadata) {
                SyncDuplicationCursorSuppression(lastSuccessfulWgcCursorEmbedded);
            }

            const bool repeatSucceeded = repeatLastFrameForScheduledQpc(scheduledQpc);
            const bool repeatDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
            if (!repeatSucceeded || repeatDeferred) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failed and cached-repeat recovery also failed: context=%s "
                    "scheduledQpc=%lld repeatSucceeded=%d repeatDeferred=%d",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc), repeatSucceeded ? 1 : 0,
                    repeatDeferred ? 1 : 0);
                return false;
            }

            static uint64_t s_freshEncodeRecoveryCount = 0;
            ++s_freshEncodeRecoveryCount;
            if (s_freshEncodeRecoveryCount <= 5 || (s_freshEncodeRecoveryCount % 120ull) == 0ull) {
                LogWarn(
                    "[EncoderThread] CFR fresh encode failure recovered with cached duplicate: context=%s "
                    "scheduledQpc=%lld recoveryCount=%llu",
                    context ? context : "unknown", static_cast<long long>(scheduledQpc),
                    static_cast<unsigned long long>(s_freshEncodeRecoveryCount));
            }
            return true;
        };
        auto releaseWgcLeaseAfterMediaEngineCopy = [&](QueuedFrame& encodedFrame, const char* context) {
            if (encodedFrame.isInjectMode || !encodedFrame.wgcPoolLease.IsValid()) {
                return;
            }
            if (!hasRepeatLastFramePath) {
                static bool s_loggedHeldForFallback = false;
                if (!s_loggedHeldForFallback) {
                    LogWarn(
                        "[WGC] Holding encoded pool slot lease because media-engine repeat cache is unavailable "
                        "(slot=%u generation=%llu context=%s). Pool pressure can rise on fallback duplicate paths.",
                        encodedFrame.wgcPoolSlot, static_cast<unsigned long long>(encodedFrame.wgcPoolGeneration),
                        context);
                    s_loggedHeldForFallback = true;
                }
                return;
            }

            const uint32_t slot = encodedFrame.wgcPoolSlot;
            const uint64_t generation = encodedFrame.wgcPoolGeneration;
            encodedFrame.wgcPoolLease.Reset();
            encodedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
            encodedFrame.wgcPoolGeneration = 0;

            static uint64_t s_releaseLogCount = 0;
            ++s_releaseLogCount;
            if (s_releaseLogCount <= 5 || (s_releaseLogCount % 1000ull) == 0ull) {
                LogInfo(
                    "[WGC] Pool slot lease released after media-engine copy: slot=%u generation=%llu context=%s "
                    "releaseCount=%llu",
                    slot, static_cast<unsigned long long>(generation), context,
                    static_cast<unsigned long long>(s_releaseLogCount));
            }
        };
        const bool warmupCaptureModeChanged = ce::capture_policy::ResetWarmupOnCaptureModeChange(
            recordingOutputLive, useScreenGrab, GetTickCount64(), warmupState);
        if (warmupCaptureModeChanged || !useScreenGrab) {
            ResetWarmupWgcFreshness();
            wgcLowSourceModeActive = false;
            wgcLowSourceStateChangeTick = 0;
            wgcLiveRecoveryModeActive = false;
            wgcLiveRecoveryStateChangeTick = 0;
            wgcSourceStarvedCurrent = false;
            wgcSchedulerLimitedCurrent = false;
            wgcEncoderRecoveryLimitedCurrent = false;
        }
        startupWarmupStartTick = warmupState.startupWarmupStartTick;
        hiddenStartupFrames = warmupState.hiddenStartupFrames;
        if (!recordingOutputLive && useScreenGrab) {
            observeScreenGrabPrivacyWarmup();
        }
        const size_t injectReserveFrames = (!useScreenGrab)
                                               ? ce::capture_policy::GetInjectReserveFrames(
                                                     config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs)
                                               : 0;
        if (!recordingOutputLive && !pendingLiveActivation && media_main_g_Recording && media_main_g_EncoderRunning) {
            const uint64_t warmupElapsedMs64 = GetTickCount64() - startupWarmupStartTick;
            const DWORD warmupElapsedMs =
                warmupElapsedMs64 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(warmupElapsedMs64);
            const bool warmupReady = useScreenGrab
                                         ? ce::capture_policy::ShouldCommitWgcWarmup(
                                               popped, bufferedWgcFrames.size(), warmupElapsedMs,
                                               smoothedInputPerTick * static_cast<double>(config.video.fps),
                                               static_cast<uint32_t>(config.video.fps))
                                         : ce::capture_policy::ShouldCommitRecordingWarmup(
                                               useScreenGrab, config.video.useVFR, popped, !bufferedWgcFrames.empty(),
                                               bufferedInjectFrames.size(), injectReserveFrames, warmupElapsedMs);
            const bool warmupFreshEnough =
                !useScreenGrab || wgcFreshWarmupFrameCount >= ce::capture_policy::kWgcWarmupFreshFrames;
            if (warmupReady && warmupFreshEnough) {
                const bool wgcCfrStartupSync = ce::capture_policy::ShouldUseWgcCfrStartupSyncBarrier(
                    useScreenGrab, config.video.useVFR, targetIntervalTicks);
                if (wgcCfrStartupSync) {
                    if (!wgcStartupPreLiveDelayComplete) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupPreLiveDelayDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }

                        const uint32_t smoothnessStartupDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                        const uint32_t smoothnessStartupRetainedFrames = getWgcSmoothnessRetainedFramesBudget();
                        const bool smoothnessStartupAttempt = shouldAttemptWgcStartupSmoothnessBufferNow();
                        const int64_t delayTicks =
                            ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks);
                        updateWgcIngressPressure("startup-pre-live-delay");
                        if (hTimer && delayTicks > 0 && qpcFreq.QuadPart > 0) {
                            const int64_t delay100ns = (delayTicks * 10000000) / qpcFreq.QuadPart;
                            LARGE_INTEGER dueTime;
                            dueTime.QuadPart = -delay100ns;
                            if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                WaitForSingleObject(hTimer, INFINITE);
                            }
                        }

                        QueuedFrame qf;
                        size_t queueFlushed = 0;
                        while (media_main_g_FrameQueue.Pop(qf, 0)) {
                            if (qf.isInjectMode)
                                DiscardQueuedFrame(qf);
                            else if (qf.texture)
                                ReleaseQueuedFrameTexture(qf);
                            queueFlushed++;
                        }
                        size_t bufferedFlushed = 0;
                        if (!bufferedWgcFrames.empty()) {
                            bufferedFlushed = bufferedWgcFrames.size();
                            ClearBufferedWgcFrames();
                        }
                        updateWgcIngressPressure("startup-post-delay-flush");

                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        wgcStartupBarrierDroppedFrames = 0;
                        wgcStartupPreLiveDelayComplete = true;
                        const uint64_t warmupElapsedWithDelayMs64 = GetTickCount64() - startupWarmupStartTick;
                        LogInfo(
                            "[EncoderThread] WGC startup pre-live delay complete: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld delayTicks=%lld hiddenFrames=%u discarded=%u queueFlushed=%zu "
                            "bufferedFlushed=%zu smoothAttempt=%d smoothDesiredFrames=%u "
                            "smoothnessRetainedFrames=%u smoothPreLiveDelayTicks=0 smoothReason=%s warmupMs=%llu",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks), static_cast<long long>(delayTicks),
                            hiddenStartupFrames, wgcStartupPreLiveDelayDroppedFrames, queueFlushed, bufferedFlushed,
                            smoothnessStartupAttempt ? 1 : 0, smoothnessStartupDesiredFrames,
                            smoothnessStartupRetainedFrames, getWgcSmoothnessBufferReason(),
                            static_cast<unsigned long long>(warmupElapsedWithDelayMs64));
                        continue;
                    }

                    if (wgcStartupBarrierQpc <= 0) {
                        LARGE_INTEGER barrierNow;
                        QueryPerformanceCounter(&barrierNow);
                        wgcStartupBarrierQpc =
                            ce::capture_policy::GetWgcStartupBarrierQpc(barrierNow.QuadPart, targetIntervalTicks);
                        LogInfo(
                            "[EncoderThread] WGC startup sync post-delay barrier armed: anchorQpc=%lld now=%lld "
                            "oneFrame=%lld",
                            static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(barrierNow.QuadPart),
                            static_cast<long long>(targetIntervalTicks));
                    }

                    if (!popped || frame.isInjectMode ||
                        !ce::capture_policy::IsWgcFramePastStartupBarrier(frame.timestamp, wgcStartupBarrierQpc)) {
                        if (popped) {
                            TrackWarmupWgcFreshFrame(frame);
                            ++hiddenStartupFrames;
                            ++wgcStartupBarrierDroppedFrames;
                            warmupState.hiddenStartupFrames = hiddenStartupFrames;
                            DiscardQueuedFrame(frame);
                        }
                        continue;
                    }

                    // Initialize codec/device/mux once WGC/DXGI format is known, then
                    // discard prewarm-era pool contents and arm the real barrier.
                    if (!wgcEncoderPrewarmAttempted) {
                        wgcEncoderPrewarmAttempted = true;
                        LARGE_INTEGER prewarmStartQpc = {};
                        LARGE_INTEGER prewarmEndQpc = {};
                        QueryPerformanceCounter(&prewarmStartQpc);
                        const bool privacyTextureReady = privacyRuntime.PrepareTexture(frame.texture);
                        wgcEncoderPrewarmSucceeded =
                            privacyTextureReady && MediaEngine_PrepareFrameD3D11 &&
                            MediaEngine_PrepareFrameD3D11(frame.texture, frame.width, frame.height, frame.isHDR);
                        QueryPerformanceCounter(&prewarmEndQpc);
                        wgcEncoderPrewarmElapsedUs = qpcToUs(prewarmEndQpc.QuadPart - prewarmStartQpc.QuadPart);
                        LogInfo(
                            "[EncoderThread] WGC transactional video prewarm %s: elapsed=%lldus frameQpc=%lld "
                            "dimensions=%ux%u hdr=%d queuedAfter=%zu bufferedAfter=%zu; timeline remains uncommitted",
                            wgcEncoderPrewarmSucceeded ? "complete" : "FAILED",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs), static_cast<long long>(frame.timestamp),
                            frame.width, frame.height, frame.isHDR ? 1 : 0, media_main_g_FrameQueue.Size(),

                            bufferedWgcFrames.size());
                        if (!privacyTextureReady) {
                            LogError(
                                "[PrivacyBlackout] Failed to create the GPU opaque-black texture for %ux%u; "
                                "recording start is rejected rather than exposing captured pixels",
                                frame.width, frame.height);
                        }
                        if (!wgcEncoderPrewarmSucceeded) {
                            PublishRecordingStartFailure(RecordingFailureCode::RecordingStartFailed,
                                                         "WGC deferred encoder/mux initialization");
                            media_main_g_EncoderRunning = false;
                            DiscardQueuedFrame(frame);
                            break;
                        }

                        // Restart after a long prewarm so live begins with fresh pool history.
                        size_t prewarmEraDiscarded = 1;
                        DiscardQueuedFrame(frame);
                        frame = {};
                        QueuedFrame prewarmEraFrame;
                        while (media_main_g_FrameQueue.Pop(prewarmEraFrame, 0)) {
                            DiscardQueuedFrame(prewarmEraFrame);
                            prewarmEraFrame = {};
                            ++prewarmEraDiscarded;
                        }
                        prewarmEraDiscarded += bufferedWgcFrames.size();
                        ClearBufferedWgcFrames();
                        wgcStartupBarrierDroppedFrames += SaturatingToUint32(prewarmEraDiscarded);
                        wgcStartupReserveWaitStartQpc = 0;
                        wgcStartupReserveWaitCount = 0;
                        wgcStartupReserveWaitInitialSpanUs = 0;
                        wgcStartupReserveWaitFreshenedMax = 0;
                        LARGE_INTEGER postPrewarmNow = {};
                        QueryPerformanceCounter(&postPrewarmNow);
                        wgcStartupBarrierQpc = ce::capture_policy::GetWgcStartupBarrierQpc(
                            postPrewarmNow.QuadPart, targetIntervalTicks);
                        updateWgcIngressPressure("startup-post-prewarm-refresh");
                        LogInfo(
                            "[EncoderThread] WGC startup barrier refreshed after transactional prewarm: "
                            "anchorQpc=%lld now=%lld discardedPrewarmEra=%zu elapsed=%lldus",
                            static_cast<long long>(wgcStartupBarrierQpc),
                            static_cast<long long>(postPrewarmNow.QuadPart), prewarmEraDiscarded,
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                        continue;
                    }

                    size_t startupBufferedExamined = 0;
                    size_t startupQueueExamined = 0;
                    size_t startupFreshened = 0;
                    size_t startupDiscardedOlder = 0;
                    size_t startupDiscardedBeforeBarrier = 0;
                    size_t startupDiscardedPathMismatch = 0;
                    struct StartupWgcCandidate {
                        QueuedFrame frame;
                        size_t sequence = 0;
                    };
                    std::vector<StartupWgcCandidate> startupCandidates;
                    startupCandidates.reserve(1 + bufferedWgcFrames.size() + 8);
                    size_t startupCandidateSequence = 0;
                    const int64_t initialStartupSelectionQpc = GetFrameSelectionTimestamp(frame);
                    auto considerStartupWgcCandidate = [&](QueuedFrame candidate, bool fromQueue,
                                                           bool initialCandidate = false) {
                        if (fromQueue) {
                            ++startupQueueExamined;
                        } else if (!initialCandidate) {
                            ++startupBufferedExamined;
                        }

                        if (candidate.isInjectMode) {
                            ++startupDiscardedPathMismatch;
                            DiscardQueuedFrame(candidate);
                            return;
                        }

                        if (!ce::capture_policy::IsWgcFramePastStartupBarrier(candidate.timestamp,
                                                                              wgcStartupBarrierQpc)) {
                            ++startupDiscardedBeforeBarrier;
                            ++wgcStartupBarrierDroppedFrames;
                            ReleaseQueuedFrameTexture(candidate);
                            return;
                        }

                        StartupWgcCandidate startupCandidate;
                        startupCandidate.frame = std::move(candidate);
                        startupCandidate.sequence = startupCandidateSequence++;
                        startupCandidates.push_back(std::move(startupCandidate));
                    };

                    considerStartupWgcCandidate(std::move(frame), false, true);
                    frame = QueuedFrame{};

                    while (!bufferedWgcFrames.empty()) {
                        QueuedFrame candidate = std::move(bufferedWgcFrames.front());
                        bufferedWgcFrames.pop_front();
                        considerStartupWgcCandidate(std::move(candidate), false);
                    }

                    QueuedFrame queuedStartupCandidate;
                    while (media_main_g_FrameQueue.Pop(queuedStartupCandidate, 0)) {
                        considerStartupWgcCandidate(std::move(queuedStartupCandidate), true);
                        queuedStartupCandidate = QueuedFrame{};
                    }

                    std::stable_sort(startupCandidates.begin(), startupCandidates.end(),
                                     [&](const StartupWgcCandidate& lhs, const StartupWgcCandidate& rhs) {
                                         const int64_t lhsSelectionQpc = GetFrameSelectionTimestamp(lhs.frame);
                                         const int64_t rhsSelectionQpc = GetFrameSelectionTimestamp(rhs.frame);
                                         if (lhsSelectionQpc != rhsSelectionQpc) {
                                             return lhsSelectionQpc < rhsSelectionQpc;
                                         }
                                         if (lhs.frame.timestamp != rhs.frame.timestamp) {
                                             return lhs.frame.timestamp < rhs.frame.timestamp;
                                         }
                                         return lhs.sequence < rhs.sequence;
                                     });

                    std::vector<int64_t> startupSelectionQpcs;
                    startupSelectionQpcs.reserve(startupCandidates.size());
                    for (const auto& candidate : startupCandidates) {
                        startupSelectionQpcs.push_back(GetFrameSelectionTimestamp(candidate.frame));
                        if (GetFrameSelectionTimestamp(candidate.frame) > initialStartupSelectionQpc) {
                            ++startupFreshened;
                        }
                    }

                    const uint32_t smoothnessDesiredFrames = getWgcSmoothnessDesiredFramesForConfig();
                    const uint32_t smoothnessRetainedFrames =
                        (media_main_g_WgcCap && smoothnessDesiredFrames > 0) ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;
                    const bool smoothnessStartupAttempted = ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
                        config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired,
                        targetIntervalTicks, smoothnessRetainedFrames);
                    const uint32_t smoothnessPoolSlots =
                        media_main_g_WgcCap ? media_main_g_WgcCap->GetTexturePoolSlotCount()
                                 : ce::capture_policy::GetWgcSmoothnessPoolFrameCount(smoothnessRetainedFrames);
                    const uint32_t smoothnessRetainedFrameCap =
                        media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCap() : smoothnessPoolSlots;
                    const uint32_t smoothnessReservedFreeSlots =
                        media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount()
                                 : ce::capture_policy::kWgcSmoothnessBufferPoolSafetyFrames;
                    const uint64_t smoothnessEstimatedVramBytes =
                        media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessEstimatedVramBytes() : 0ull;
                    const bool smoothnessCapLimited =
                        smoothnessDesiredFrames > 0 && smoothnessRetainedFrames < smoothnessDesiredFrames;
                    // Full buildable reservoir target (audio-latency path uses this unchanged).
                    const int64_t smoothnessReservoirTargetDelayQpc =
                        getWgcStartupSmoothnessTargetDelayQpc(smoothnessStartupAttempted);
                    // Resolve and lock the jitter/config floor once; the audio-latency
                    // reservoir already dominates it when active.
                    if (wgcSmoothnessFloorConfigured && smoothnessStartupAttempted &&
                        smoothnessReservoirTargetDelayQpc > 0) {
                        if (media_main_g_WgcCap) {
                            wgcSmoothnessFloorJitter.deliveryGapAvgUs =
                                SaturatingToUint32(media_main_g_WgcCap->GetCallbackGapAvgUs());
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs =
                                SaturatingToUint32(media_main_g_WgcCap->GetCallbackGapMaxUs());
                            wgcSmoothnessFloorJitter.sourceJitterAvgUs =
                                SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs());
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs =
                                SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterMaxUs());
                        }
                        if (config.wgcSmoothnessFloorAuto) {
                            wgcSmoothnessFloorSource = "auto";
                            wgcSmoothnessFloorRequestedQpc = ce::capture_policy::DeriveWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorJitter, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                            wgcSmoothnessFloorDelayQpc = wgcSmoothnessFloorRequestedQpc;
                        } else {
                            wgcSmoothnessFloorSource = "config";
                            wgcSmoothnessFloorRequestedQpc =
                                qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessFloorMs)) / 1000
                                    : 0;
                            wgcSmoothnessFloorDelayQpc = ce::capture_policy::ClampWgcSmoothnessFloorDelayQpc(
                                wgcSmoothnessFloorRequestedQpc, targetIntervalTicks, qpcFreq.QuadPart,
                                config.wgcSmoothnessBufferMaxMs, smoothnessRetainedFrames);
                        }
                        const int64_t floorCapQpc = ce::capture_policy::GetWgcSmoothnessFloorCapQpc(
                            targetIntervalTicks, qpcFreq.QuadPart, config.wgcSmoothnessBufferMaxMs,
                            smoothnessRetainedFrames);
                        const int64_t floorMinQpc =
                            targetIntervalTicks *
                            static_cast<int64_t>(ce::capture_policy::kWgcSmoothnessFloorMinFrames);
                        if (wgcSmoothnessFloorRequestedQpc >= floorCapQpc && floorCapQpc > 0) {
                            const int64_t maxMsQpc =
                                config.wgcSmoothnessBufferMaxMs > 0 && qpcFreq.QuadPart > 0
                                    ? (qpcFreq.QuadPart * static_cast<int64_t>(config.wgcSmoothnessBufferMaxMs)) / 1000
                                    : INT64_MAX;
                            wgcSmoothnessFloorClampedBy = (maxMsQpc <= floorCapQpc) ? "max_ms" : "reservoir";
                        } else if (wgcSmoothnessFloorRequestedQpc < floorMinQpc) {
                            wgcSmoothnessFloorClampedBy = "min";
                        } else {
                            wgcSmoothnessFloorClampedBy = "none";
                        }
                    }
                    // Without audio latency, use only the smaller jitter-sized floor.
                    const int64_t smoothnessTargetDelayQpc =
                        avContentDelayActive ? smoothnessReservoirTargetDelayQpc
                                             : std::min(wgcSmoothnessFloorDelayQpc, smoothnessReservoirTargetDelayQpc);
                    const int64_t startupContentDelayTargetQpc =
                        avContentDelayQpc + std::max<int64_t>(0, smoothnessTargetDelayQpc);
                    wgcSmoothnessDesiredFrames = smoothnessDesiredFrames;
                    wgcSmoothnessRetainedFrames = smoothnessRetainedFrames;
                    wgcSmoothnessPoolSlots = smoothnessPoolSlots;
                    wgcSmoothnessRetainedFrameCap = smoothnessRetainedFrameCap;
                    wgcSmoothnessReservedFreeSlots = smoothnessReservedFreeSlots;
                    wgcSmoothnessEstimatedVramBytes = smoothnessEstimatedVramBytes;
                    wgcSmoothnessCapLimited = smoothnessCapLimited;
                    wgcSmoothnessBufferReason = getWgcSmoothnessBufferReason();
                    if (smoothnessDesiredFrames > 0 && smoothnessRetainedFrames == 0) {
                        wgcSmoothnessBufferReason = "vram_budget_exhausted";
                    } else if (smoothnessCapLimited) {
                        wgcSmoothnessBufferReason = "vram_cap_limited";
                    }

                    // Log the smoothness-floor inputs, clamp, effective target, and
                    // with-audio no-op once so the startup decision stays auditable.
                    if (wgcSmoothnessFloorConfigured && !wgcSmoothnessFloorLogged) {
                        // This runs on each reserve re-evaluation, but the result is stable.
                        wgcSmoothnessFloorLogged = true;
                        const char* floorNote = avContentDelayActive
                                                    ? "no-op: audio-latency reservoir target dominates"
                                                    : (smoothnessReservoirTargetDelayQpc > 0
                                                           ? "active: video-only/low-confidence jitter buffer"
                                                           : "inactive: no reservoir capacity");
                        LogInfo(
                            "[AVSyncApply] wgc_smoothness_floor: source=%s auto=%d configuredMs=%u "
                            "deliveryGapUs(avg/max)=%u/%u sourceJitterUs(avg/max)=%u/%u requestedUs=%lld "
                            "derivedUs=%lld clampedBy=%s reservoirTargetUs=%lld effectiveTargetUs=%lld "
                            "avContentDelayUs=%lld note=\"%s\"",
                            wgcSmoothnessFloorSource, config.wgcSmoothnessFloorAuto ? 1 : 0,
                            config.wgcSmoothnessFloorMs, wgcSmoothnessFloorJitter.deliveryGapAvgUs,
                            wgcSmoothnessFloorJitter.deliveryGapMaxUs, wgcSmoothnessFloorJitter.sourceJitterAvgUs,
                            wgcSmoothnessFloorJitter.sourceJitterMaxUs,
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorRequestedQpc)),
                            static_cast<long long>(qpcToUs(wgcSmoothnessFloorDelayQpc)), wgcSmoothnessFloorClampedBy,
                            static_cast<long long>(qpcToUs(smoothnessReservoirTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(smoothnessTargetDelayQpc)),
                            static_cast<long long>(qpcToUs(avContentDelayQpc)), floorNote);
                    }

                    const int64_t startupReserveToleranceQpc =
                        qpcFreq.QuadPart > 0
                            ? std::min<int64_t>(targetIntervalTicks > 0 ? (targetIntervalTicks / 2) : 0,
                                                qpcFreq.QuadPart / 200)
                            : 0;
                    const auto startupReserveSelection = ce::capture_policy::SelectWgcStartupReserveCandidate(
                        startupSelectionQpcs.empty() ? nullptr : startupSelectionQpcs.data(),
                        startupSelectionQpcs.size(),
                        startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0,
                        startupReserveToleranceQpc);
                    size_t selectedStartupIndex = startupReserveSelection.selectedIndex;
                    if (selectedStartupIndex >= startupCandidates.size()) {
                        selectedStartupIndex = startupCandidates.empty() ? 0 : (startupCandidates.size() - 1);
                    }

                    const auto qpcDeltaToUs = [&](int64_t qpcDelta) -> int64_t {
                        return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;
                    };
                    wgcStartupReserveFrames = SaturatingToUint32(startupCandidates.size());
                    wgcStartupReserveSpanUs = qpcDeltaToUs(startupReserveSelection.reserveSpanQpc);
                    wgcStartupDelayTargetUs =
                        qpcDeltaToUs(startupContentDelayTargetQpc > 0 ? startupContentDelayTargetQpc : 0);
                    wgcStartupSelectedByDelayReserve = startupReserveSelection.usedDelayReserve;
                    if (startupContentDelayTargetQpc <= 0) {
                        wgcStartupReserveReason = "inactive";
                    } else if (startupCandidates.size() < 2) {
                        wgcStartupReserveReason = "insufficient_frames";
                    } else if (startupReserveSelection.usedDelayReserve) {
                        wgcStartupReserveReason = "selected";
                    } else {
                        wgcStartupReserveReason = "insufficient_span";
                    }

                    const size_t newerStartupReserveFrames = selectedStartupIndex < startupCandidates.size()
                                                                 ? (startupCandidates.size() - selectedStartupIndex - 1)
                                                                 : 0;
                    const bool startupReserveBelowLowWater =
                        startupContentDelayTargetQpc > 0 && startupReserveSelection.usedDelayReserve &&
                        newerStartupReserveFrames <
                            getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc);
                    const bool startupReserveMissing =
                        startupContentDelayTargetQpc > 0 &&
                        (!startupReserveSelection.usedDelayReserve || startupReserveBelowLowWater);
                    if (startupReserveMissing && targetIntervalTicks > 0 && qpcFreq.QuadPart > 0) {
                        LARGE_INTEGER waitNow;
                        QueryPerformanceCounter(&waitNow);
                        if (wgcStartupReserveWaitStartQpc <= 0) {
                            wgcStartupReserveWaitStartQpc = waitNow.QuadPart;
                            wgcStartupReserveWaitInitialSpanUs = wgcStartupReserveSpanUs;
                        }
                        wgcStartupReserveWaitFreshenedMax =
                            std::max<uint32_t>(wgcStartupReserveWaitFreshenedMax, SaturatingToUint32(startupFreshened));
                        const int64_t waitBudgetQpc = ce::capture_policy::GetWgcStartupReserveWaitBudgetQpc(
                            startupContentDelayTargetQpc, targetIntervalTicks, smoothnessTargetDelayQpc,
                            smoothnessStartupAttempted, qpcFreq.QuadPart);
                        const bool waitBudgetRemaining =
                            waitNow.QuadPart - wgcStartupReserveWaitStartQpc < waitBudgetQpc;
                        if (waitBudgetRemaining) {
                            ++wgcStartupReserveWaitCount;
                            for (auto& candidate : startupCandidates) {
                                if (candidate.frame.texture || candidate.frame.sharedHandle ||
                                    candidate.frame.timestamp > 0) {
                                    bufferedWgcFrames.push_back(std::move(candidate.frame));
                                }
                            }
                            trimBufferedWgcStartupWaitToRetainedCap("startup-wait");
                            wgcStartupReserveReason =
                                startupReserveBelowLowWater ? "waiting_low_water" : "waiting_span";
                            if (wgcStartupReserveWaitCount <= 3 || (wgcStartupReserveWaitCount % 30u) == 0u) {
                                LogInfo(
                                    "[EncoderThread] WGC startup delay-reserve wait: reason=%s candidates=%zu "
                                    "newer=%zu lowWater=%u target=%u span=%lldus initialSpan=%lldus "
                                    "freshened=%u waited=%lldus budget=%lldus smoothAttempt=%d "
                                    "smoothFrames=%u/%u capLimited=%d",
                                    wgcStartupReserveReason.c_str(), startupCandidates.size(),
                                    newerStartupReserveFrames,
                                    getWgcDelayReservoirLowWaterFramesForDelay(startupContentDelayTargetQpc),
                                    getWgcDelayReservoirTargetFramesForDelay(startupContentDelayTargetQpc),
                                    static_cast<long long>(wgcStartupReserveSpanUs),
                                    static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                                    wgcStartupReserveWaitFreshenedMax,
                                    static_cast<long long>(qpcToUs(waitNow.QuadPart - wgcStartupReserveWaitStartQpc)),
                                    static_cast<long long>(qpcToUs(waitBudgetQpc)), smoothnessStartupAttempted ? 1 : 0,
                                    smoothnessRetainedFrames, smoothnessDesiredFrames, smoothnessCapLimited ? 1 : 0);
                            }
                            continue;
                        }
                        const bool noStartupSpanGrowth = wgcStartupReserveSpanUs <= 0 &&
                                                         wgcStartupReserveSpanUs <= wgcStartupReserveWaitInitialSpanUs;
                        wgcStartupReserveReason =
                            noStartupSpanGrowth
                                ? "source_startup_underfeed"
                                : (startupReserveBelowLowWater ? "low_water_timeout" : "reserve_timeout");
                    }

                    bool startupPartialReserveFallback = false;
                    if (ce::capture_policy::ShouldPreserveWgcStartupPartialReserve(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc,
                            startupContentDelayTargetQpc > 0, startupReserveMissing)) {
                        selectedStartupIndex = 0;
                        startupPartialReserveFallback = true;
                        if (wgcStartupReserveReason != "source_startup_underfeed") {
                            wgcStartupReserveReason = "partial_span_timeout";
                        }
                    }

                    const int64_t latestStartupSelectionQpc =
                        startupSelectionQpcs.empty() ? 0 : startupSelectionQpcs.back();
                    int64_t selectedStartupSelectionQpc =
                        selectedStartupIndex < startupCandidates.size()
                            ? GetFrameSelectionTimestamp(startupCandidates[selectedStartupIndex].frame)
                            : 0;
                    int64_t actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                        ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                        : 0;
                    const int64_t pileupSmoothnessActiveDelayQpc =
                        ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                    // Avoid locking healthy-source startup pile-up as permanent delay.
                    const bool startupMinWindowSourceAtOrAboveCfr =
                        media_main_g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                        std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps)),
                                        media_main_g_WgcCap->GetInputMin250Fps(), media_main_g_WgcCap->GetInputMin500Fps());
                    const bool startupCandidateCadenceAtOrAboveCfr =
                        ce::capture_policy::IsWgcStartupCandidateCadenceAtOrAboveCfrTarget(
                            startupCandidates.size(), startupReserveSelection.reserveSpanQpc, targetIntervalTicks);
                    const bool startupSourceAtOrAboveCfr =
                        startupMinWindowSourceAtOrAboveCfr || startupCandidateCadenceAtOrAboveCfr;
                    wgcSmoothnessActiveDelayQpc = ce::capture_policy::ResolveWgcStartupSmoothnessActiveDelayQpc(
                        pileupSmoothnessActiveDelayQpc, wgcSmoothnessFloorDelayQpc, startupPartialReserveFallback,
                        startupSourceAtOrAboveCfr);
                    if (startupPartialReserveFallback && !startupSelectionQpcs.empty() &&
                        latestStartupSelectionQpc > 0) {
                        const size_t fallbackIndexBeforeContractRecalculation = selectedStartupIndex;
                        const int64_t recalculatedContentDelayQpc =
                            avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);
                        const int64_t recalculatedTargetQpc =
                            latestStartupSelectionQpc > recalculatedContentDelayQpc
                                ? latestStartupSelectionQpc - recalculatedContentDelayQpc
                                : latestStartupSelectionQpc;
                        selectedStartupIndex = ce::capture_policy::SelectNearestMonotonicTimestampIndex(
                            startupSelectionQpcs.data(), startupSelectionQpcs.size(), recalculatedTargetQpc);
                        selectedStartupSelectionQpc = startupSelectionQpcs[selectedStartupIndex];
                        actualStartupDelayQpc = latestStartupSelectionQpc > selectedStartupSelectionQpc
                                                    ? latestStartupSelectionQpc - selectedStartupSelectionQpc
                                                    : 0;
                        wgcSmoothnessActiveDelayQpc = ce::capture_policy::SelectWgcStartupSmoothnessExtraDelayQpc(
                            actualStartupDelayQpc, avContentDelayQpc, smoothnessTargetDelayQpc);
                        LogInfo(
                            "[EncoderThread] WGC partial reservoir contract recalculated: oldIndex=%zu newIndex=%zu "
                            "latestQpc=%lld targetQpc=%lld selectedQpc=%lld realizedContentDelayUs=%lld "
                            "renderDelayUs=%lld smoothReserveUs=%lld (frame selection and delay changed together)",
                            fallbackIndexBeforeContractRecalculation, selectedStartupIndex,
                            static_cast<long long>(latestStartupSelectionQpc),
                            static_cast<long long>(recalculatedTargetQpc),
                            static_cast<long long>(selectedStartupSelectionQpc),
                            static_cast<long long>(qpcDeltaToUs(actualStartupDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)));
                    }
                    if (wgcSmoothnessActiveDelayQpc < pileupSmoothnessActiveDelayQpc) {
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay capped to measured jitter floor: "
                            "pileupUs=%lld cappedUs=%lld floorUs=%lld minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld reason=%s (avoids startup-timing-dependent deep-lock "
                            "repeat clustering; sync-neutral)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupMinWindowSourceAtOrAboveCfr ? 1 : 0, startupCandidateCadenceAtOrAboveCfr ? 1 : 0,
                            startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            wgcStartupReserveReason.c_str());
                    } else if (startupPartialReserveFallback) {
                        // The fortistutter session showed this decision silently NOT engaging because the
                        // barrier-time min-window input rate was polluted by pre-live settling gaps
                        // (MinIn250=104 for a healthy 140 fps source). Log the gate inputs so a dormant
                        // cap is diagnosable instead of invisible.
                        LogInfo(
                            "[EncoderThread] WGC startup underfed active-delay cap NOT engaged: pileupUs=%lld "
                            "floorUs=%lld sourceAtOrAboveCfr=%d minWindowProof=%d candidateProof=%d "
                            "candidates=%zu candidateSpanUs=%lld inputMin250=%u inputMin500=%u outputFps=%u "
                            "reason=%s (deep pile-up lock retained for lull absorption)",
                            static_cast<long long>(qpcDeltaToUs(pileupSmoothnessActiveDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(wgcSmoothnessFloorDelayQpc)),
                            startupSourceAtOrAboveCfr ? 1 : 0, startupMinWindowSourceAtOrAboveCfr ? 1 : 0,
                            startupCandidateCadenceAtOrAboveCfr ? 1 : 0, startupCandidates.size(),
                            static_cast<long long>(qpcDeltaToUs(startupReserveSelection.reserveSpanQpc)),
                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin250Fps() : 0u,
                            media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin500Fps() : 0u, static_cast<uint32_t>(config.video.fps),
                            wgcStartupReserveReason.c_str());
                    }
                    wgcSmoothnessActualFrames =
                        targetIntervalTicks > 0
                            ? SaturatingToUint32(static_cast<uint64_t>(
                                  (wgcSmoothnessActiveDelayQpc + targetIntervalTicks / 2) / targetIntervalTicks))
                            : 0u;

                    uint32_t startupRetainedCapTrimmed = 0;
                    size_t startupKeptReserveFrames = 0;
                    for (size_t i = 0; i < startupCandidates.size(); ++i) {
                        if (i == selectedStartupIndex) {
                            frame = std::move(startupCandidates[i].frame);
                        } else if (startupReserveSelection.usedDelayReserve || startupPartialReserveFallback) {
                            if (i > selectedStartupIndex &&
                                (smoothnessRetainedFrameCap == 0 ||
                                 startupKeptReserveFrames < smoothnessRetainedFrameCap)) {
                                bufferedWgcFrames.push_back(std::move(startupCandidates[i].frame));
                                ++startupKeptReserveFrames;
                            } else {
                                ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                                ++startupRetainedCapTrimmed;
                            }
                        } else {
                            ReleaseQueuedFrameTexture(startupCandidates[i].frame);
                            ++startupDiscardedOlder;
                        }
                    }
                    if (startupRetainedCapTrimmed > 0) {
                        wgcRetainedCapTrimTotal += startupRetainedCapTrimmed;
                        wgcRetainedCapTrimWindow += startupRetainedCapTrimmed;
                    }

                    LARGE_INTEGER anchorNow;
                    QueryPerformanceCounter(&anchorNow);
                    ++pendingWgcStartContractGeneration;
                    const int64_t selectedContentDelayQpc = getWgcEffectiveContentDelayQpc();
                    if (frame.timestamp > 0) {
                        pendingWgcStartContract = ce::capture_policy::BuildWallAnchoredCfrTimelineStartContract(
                            anchorNow.QuadPart, selectedContentDelayQpc, avContentDelayQpc);
                    } else {
                        pendingWgcStartContract = {};
                    }
                    if (pendingWgcStartContract.valid) {
                        LogInfo(
                            "[EncoderThread] WGC CFR start contract selected: generation=%llu videoQpc=%lld "
                            "sourceQpc=%lld selectionQpc=%lld liveQpc=%lld contentDelayUs=%lld renderDelayUs=%lld "
                            "smoothReserveUs=%lld retainedNewer=%zu prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(pendingWgcStartContract.videoOriginQpc),
                            static_cast<long long>(frame.timestamp),
                            static_cast<long long>(GetFrameSelectionTimestamp(frame)),
                            static_cast<long long>(pendingWgcStartContract.liveQpc),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.contentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.renderLoopbackLatencyQpc)),
                            static_cast<long long>(qpcDeltaToUs(pendingWgcStartContract.smoothnessReserveQpc)),
                            bufferedWgcFrames.size(), wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    } else {
                        LogWarn(
                            "[EncoderThread] ERROR: WGC CFR start contract selection failed: generation=%llu "
                            "videoQpc=%lld contentDelayUs=%lld renderDelayUs=%lld prewarm=%s/%lldus",
                            static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                            static_cast<long long>(frame.timestamp),
                            static_cast<long long>(qpcDeltaToUs(selectedContentDelayQpc)),
                            static_cast<long long>(qpcDeltaToUs(avContentDelayQpc)),
                            wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                            static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                    }
                    updateWgcIngressPressure("startup-selected");

                    const int64_t startupReserveWaitedUs =
                        wgcStartupReserveWaitStartQpc > 0 ? qpcToUs(anchorNow.QuadPart - wgcStartupReserveWaitStartQpc)
                                                          : 0;
                    const int64_t startupReserveSpanGrowthUs =
                        wgcStartupReserveWaitStartQpc > 0
                            ? std::max<int64_t>(0, wgcStartupReserveSpanUs - wgcStartupReserveWaitInitialSpanUs)
                            : 0;
                    const int64_t startupSelectedDelayUs = qpcDeltaToUs(actualStartupDelayQpc);
                    const bool startupReserveFallback =
                        startupContentDelayTargetQpc > 0 && !startupReserveSelection.usedDelayReserve;
                    const int64_t startDeltaUs =
                        ((frame.timestamp - wgcStartupBarrierQpc) * 1000000) / qpcFreq.QuadPart;
                    const int64_t frameAgeUs =
                        anchorNow.QuadPart >= frame.timestamp
                            ? ((anchorNow.QuadPart - frame.timestamp) * 1000000) / qpcFreq.QuadPart
                            : 0;
                    const bool startupSmoothnessUnderfed =
                        smoothnessStartupAttempted && wgcSmoothnessActiveDelayQpc < smoothnessTargetDelayQpc;
                    LogInfo(
                        "[EncoderThread] WGC startup sync post-delay barrier satisfied: anchorQpc=%lld "
                        "firstFrameQpc=%lld delta=%lldus frameAge=%lldus droppedPostDelay=%u "
                        "discardedBeforeDelay=%u freshened=%zu bufferedExamined=%zu queueExamined=%zu "
                        "discardedOlder=%zu discardedBeforeBarrier=%zu pathMismatch=%zu startupReserveFrames=%u "
                        "startupReserveSpanUs=%lld startupDelayTargetUs=%lld startupSelectedByDelayReserve=%d "
                        "startupReserveReason=%s keptReserveFrames=%zu startupReserveWaits=%u "
                        "startupReserveWaitedUs=%lld startupReserveInitialSpanUs=%lld "
                        "startupReserveSpanGrowthUs=%lld startupReserveWaitFreshened=%u "
                        "startupSelectedIndex=%zu startupSelectedDelayUs=%lld startupFallback=%d "
                        "startupPartialReserveFallback=%d smoothAttempt=%d smoothDesiredFrames=%u "
                        "smoothRetainedFrames=%u smoothActualFrames=%u smoothTargetDelayUs=%lld "
                        "smoothDelayUs=%lld smoothStartupUnderfed=%d smoothPoolSlots=%u retainedCap=%u "
                        "reservedFreeSlots=%u retainedCapTrimmed=%u smoothCapLimited=%d "
                        "startupEffectiveDelayUs=%lld smoothReason=%s",
                        static_cast<long long>(wgcStartupBarrierQpc), static_cast<long long>(frame.timestamp),
                        static_cast<long long>(startDeltaUs), static_cast<long long>(frameAgeUs),
                        wgcStartupBarrierDroppedFrames, wgcStartupPreLiveDelayDroppedFrames, startupFreshened,
                        startupBufferedExamined, startupQueueExamined, startupDiscardedOlder,
                        startupDiscardedBeforeBarrier, startupDiscardedPathMismatch, wgcStartupReserveFrames,
                        static_cast<long long>(wgcStartupReserveSpanUs),
                        static_cast<long long>(wgcStartupDelayTargetUs), wgcStartupSelectedByDelayReserve ? 1 : 0,
                        wgcStartupReserveReason.c_str(), bufferedWgcFrames.size(), wgcStartupReserveWaitCount,
                        static_cast<long long>(startupReserveWaitedUs),
                        static_cast<long long>(wgcStartupReserveWaitInitialSpanUs),
                        static_cast<long long>(startupReserveSpanGrowthUs), wgcStartupReserveWaitFreshenedMax,
                        selectedStartupIndex, static_cast<long long>(startupSelectedDelayUs),
                        startupReserveFallback ? 1 : 0, startupPartialReserveFallback ? 1 : 0,
                        smoothnessStartupAttempted ? 1 : 0, smoothnessDesiredFrames, smoothnessRetainedFrames,
                        wgcSmoothnessActualFrames, static_cast<long long>(qpcDeltaToUs(smoothnessTargetDelayQpc)),
                        static_cast<long long>(qpcDeltaToUs(wgcSmoothnessActiveDelayQpc)),
                        startupSmoothnessUnderfed ? 1 : 0, smoothnessPoolSlots, smoothnessRetainedFrameCap,
                        smoothnessReservedFreeSlots, startupRetainedCapTrimmed, smoothnessCapLimited ? 1 : 0,
                        static_cast<long long>(qpcDeltaToUs(getWgcEffectiveContentDelayQpc())),
                        wgcSmoothnessBufferReason.c_str());
                }

                pendingLiveActivation = true;
                // Reset Bresenham credit for a clean start; keep smoothedInputPerTick
                // so the EMA calibration from warmup carries over.
                frameCreditAccumulator = 0.0;
                selectionLogCounter = 0;
                pacingInputThisWindow = 0;
                pacingTicksThisWindow = 0;
                encoderGridStartQpc = 0;
                encoderGridTickCount = 0;
                liveTicksOutput = 0;
                liveTicksScheduled = 0;
                liveTicksDiscardedByTimerRebase = 0;
                wgcVisualDebtMaxExcessTicks = 0;
                wgcStopDrainHeldFrameLogged = false;
                liveStartQpc = {};
                wgcInputPredictor.Reset();
                wgcCfrPhaseLock.Reset();
                smoothedEncCycleMs = 0.0;
                smoothedInjectServiceMs = 0.0;
                smoothedWgcFreshServiceMs = 0.0;
                smoothedWgcRepeatServiceMs = 0.0;
                wgcFreshServiceSamples = wgcRepeatServiceSamples = 0;
                wgcOverloadRepeatPacer = {};
                wgcRepeatCatchupTotal = 0;
                wgcFreshCatchupTotal = 0;
                injectServiceMaxUs = 0;
                injectCfrRecoveryActive = false;
                injectEncoderServiceTooSlowCurrent = false;
                injectCfrRecoveryStartTick = 0;
                injectCfrRecoveryStartDebt = 0;
                injectCfrRecoveryBestDebt = 0;
                injectCfrRecoveryStartFreshCatchup = injectFreshCatchupTotal;
                injectCfrRecoveryStartRepeatCatchup = injectRepeatCatchupTotal;
                injectCfrRecoveryLastProgressLogTick = 0;
                encCycleMaxMs = 0;
                dupTimestampCount = 0;
                lastWgcDuplicateTimestampSkipCountForCadence =
                    media_main_g_WgcCap ? media_main_g_WgcCap->GetDuplicateTimestampSkipCount() : 0u;
                wgcRecentDeliveredFps = 0;
                wgcRecentDeliveredMin250Fps = 0;
                wgcRecentDeliveredMin500Fps = 0;
                wgcRecentInputMin250Fps = 0;
                wgcRecentInputMin500Fps = 0;
                wgcNoFreshTickCount = 0;
                encodeSpikeCountThisSecond = 0;
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
                wgcRepeatPolicyHoldTotal = 0;
                wgcSyncDelayHoldCount = 0;
                wgcSyncDelayHoldTotal = 0;
                wgcSyncDelaySourceLimitedHoldCount = 0;
                wgcSyncDelaySourceLimitedHoldTotal = 0;
                wgcSyncDelayPolicyHoldCount = 0;
                wgcSyncDelayPolicyHoldTotal = 0;
                wgcTooNewLeadMaxUs = 0;
                wgcTooNewLeadSessionMaxUs = 0;
                wgcDelaySoftLateRejectedTotal = 0;
                wgcDelaySoftLateRejectedWindow = 0;
                wgcDelaySoftLateAcceptedTotal = 0;
                wgcDelaySoftLateAcceptedWindow = 0;
                wgcDelayNearCapAcceptedTotal = 0;
                wgcDelayNearCapAcceptedWindow = 0;
                wgcDelayUniformCadenceTotal = 0;
                wgcDelayUniformCadenceWindow = 0;
                wgcDelayUniformHoldTotal = 0;
                wgcDelayUniformHoldWindow = 0;
                wgcDelayPaceCapTrimTotal = 0;
                wgcDelayPaceCapTrimWindow = 0;
                wgcRetainedCapTrimTotal = 0;
                wgcRetainedCapTrimWindow = 0;
                wgcPoolPressureTrimTotal = 0;
                wgcPoolPressureTrimWindow = 0;
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
                wgcCoverageRepeatHoldCount = 0;
                wgcCoverageDelayTicksCurrent = 0;
                wgcRepeatTimerLateCount = 0;
                wgcRepeatCatchupCount = 0;
                wgcFreshCatchupCount = 0;
                wgcSelectFreshCount = 0;
                wgcSelectDuplicateSourceCount = 0;
                wgcDropObsoleteCount = 0;
                wgcEncoderLimitedSourceDropThisWindow = 0;
                wgcEncoderLimitedSourceDropTotal = 0;
                wgcEncoderLimitedSourceDropMaxTicks = 0;
                wgcEncoderLimitedCadenceEventCount = 0;
                wgcCoverageRepeatAccumulator = 0.0;
                lastEmittedWgcSourceQpc = 0;
                lastEmittedWgcSelectionQpc = 0;
                lastEmittedInjectSourceQpc = 0;
                const size_t liveInjectReserveFrames = ce::capture_policy::GetInjectReserveFrames(
                    config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs);
                pendingLiveInjectReadyFrames =
                    useScreenGrab
                        ? 0
                        : (config.video.useVFR
                               ? ce::capture_policy::GetWarmupInjectKeepCount(smoothedInjectFenceMs, frameIntervalMs)
                               : ce::capture_policy::GetInjectCfrStartupReadyFrames(liveInjectReserveFrames,
                                                                                   injectContentDelayFrames));
                // Establish a fresh post-warmup deadline; WGC paid it before its barrier.
                if (hTimer) {
                    LARGE_INTEGER afterLive;
                    QueryPerformanceCounter(&afterLive);
                    // Inject still needs its encoder warmup interval here.
                    const bool wgcCfrDelayAlreadyDone = ce::capture_policy::ShouldUseWgcCfrStartupSyncBarrier(
                                                            useScreenGrab, config.video.useVFR, targetIntervalTicks) &&
                                                        wgcStartupPreLiveDelayComplete;
                    // WGC CFR paid this before its final barrier.
                    int64_t sleepTicks =
                        wgcCfrDelayAlreadyDone
                            ? 0
                            : (useScreenGrab
                                   ? ce::capture_policy::GetWgcCfrStartupPreLiveDelayTicks(targetIntervalTicks)
                                   : (targetIntervalTicks * 4));
                    if (sleepTicks > 0) {
                        int64_t sleep100ns = (sleepTicks * 10000000) / qpcFreq.QuadPart;
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -sleep100ns;
                        if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                            WaitForSingleObject(hTimer, INFINITE);
                        }
                    }
                }
                QueryPerformanceCounter(&nextSampleTime);
                liveStartQpc.QuadPart = 0;  // Commit the pending start contract after the first successful encode.
                encoderGridStartQpc = nextSampleTime.QuadPart;
                // Inject warmup is causal and must discard stale queued work. WGC/DXGI,
                // however, just selected an intentional look-ahead reservoir; those
                // newer frames are part of the immutable start contract and must survive
                // the live handoff.
                if (!useScreenGrab) {
                    QueuedFrame qf;
                    size_t queueFlushed = 0;
                    while (media_main_g_FrameQueue.Pop(qf, 0)) {
                        if (qf.isInjectMode)
                            DiscardQueuedFrame(qf);
                        else if (qf.texture)
                            ReleaseQueuedFrameTexture(qf);
                        queueFlushed++;
                    }
                    if (queueFlushed > 0) {
                        LogInfo("[EncoderThread] Flushed %zu warmup frames from queue", queueFlushed);
                    }
                }
                if (!bufferedInjectFrames.empty()) {
                    // Preserve enough history to cover the configured A/V content-delay
                    // target plus the physical fence tail on the first live CFR slot.
                    const size_t keepCount =
                        config.video.useVFR
                            ? ce::capture_policy::GetWarmupInjectKeepCount(smoothedInjectFenceMs, frameIntervalMs)
                            : ce::capture_policy::GetInjectCfrStartupReadyFrames(liveInjectReserveFrames,
                                                                                injectContentDelayFrames);
                    size_t flushed = 0;
                    while (bufferedInjectFrames.size() > keepCount) {
                        QueuedFrame stale = std::move(bufferedInjectFrames.front());
                        bufferedInjectFrames.pop_front();
                        DiscardQueuedFrame(stale);
                        flushed++;
                    }
                    if (flushed > 0) {
                        LogInfo("[EncoderThread] Flushed %zu stale warmup inject frames (keep=%zu)", flushed,
                                keepCount);
                    }
                }
                if (useScreenGrab) {
                    LogInfo(
                        "[EncoderThread] Preserved transactional WGC startup reserve at live handoff: "
                        "generation=%llu buffered=%zu queued=%zu contractValid=%d contentDelayUs=%lld",
                        static_cast<unsigned long long>(pendingWgcStartContractGeneration), bufferedWgcFrames.size(),
                        media_main_g_FrameQueue.Size(), pendingWgcStartContract.valid ? 1 : 0,
                        static_cast<long long>(qpcToUs(pendingWgcStartContract.contentDelayQpc)));
                }
                // Reset counters so per-second logs start clean at going-live.
                media_main_g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
                media_main_g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
                LogInfo(
                    "[EncoderThread] Warmup ready after %llums hidden warmup (%s, hiddenFrames=%u, inputRate=%.3f, "
                    "readyFrames=%zu freshWgc=%u)",
                    static_cast<unsigned long long>(warmupElapsedMs64), useScreenGrab ? "WGC" : "inject",
                    hiddenStartupFrames, smoothedInputPerTick, pendingLiveInjectReadyFrames, wgcFreshWarmupFrameCount);
                ResetWarmupWgcFreshness(false);
            }
        }

        if (pendingLiveActivation) {
            const size_t bufferedInjectReadyFrames =
                bufferedInjectFrames.size() + ((!useScreenGrab && popped && frame.isInjectMode) ? 1u : 0u);
            const bool liveReady = useScreenGrab || bufferedInjectReadyFrames >= pendingLiveInjectReadyFrames;
            if (!liveReady) {
                if (popped) {
                    if (useScreenGrab) {
                        TrackWarmupWgcFreshFrame(frame);
                        ++hiddenStartupFrames;
                        warmupState.hiddenStartupFrames = hiddenStartupFrames;
                        DiscardQueuedFrame(frame);
                    } else if (frame.isInjectMode) {
                        bufferedInjectFrames.push_front(std::move(frame));
                    } else {
                        ++hiddenStartupFrames;
                        warmupState.hiddenStartupFrames = hiddenStartupFrames;
                        DiscardQueuedFrame(frame);
                    }
                }
                continue;
            }

            if (!TryCommitCapturePipelineLive()) {
                const uint32_t phase = media_main_g_pSharedMem
                                           ? media_main_g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire)
                                           : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
                LogInfo(
                    "[RecordingLifecycle] Warmup-to-live commit rejected (phase=%s requested=%d); pending frame "
                    "discarded",
                    CapturePipelinePhaseToString(phase), media_main_g_Recording.load(std::memory_order_acquire) ? 1 : 0);
                if (popped) {
                    DiscardQueuedFrame(frame);
                }
                continue;
            }

            pendingLiveActivation = false;
            recordingOutputLive = true;
            recordingLiveTick = GetTickCount64();
            if (MediaEngine_SetWgcStartupExtraDelayQpc) {
                const int64_t startupSmoothExtraDelayQpc = useScreenGrab ? wgcSmoothnessActiveDelayQpc : 0;
                MediaEngine_SetWgcStartupExtraDelayQpc(startupSmoothExtraDelayQpc);
                if (useScreenGrab) {
                    LogInfo(
                        "[EncoderThread] WGC startup smoothness delay applied to media engine: smoothDelayUs=%lld "
                        "smoothFrames=%u/%u/%u smoothReason=%s",
                        static_cast<long long>(qpcToUs(startupSmoothExtraDelayQpc)), wgcSmoothnessActualFrames,
                        wgcSmoothnessRetainedFrames, wgcSmoothnessDesiredFrames, wgcSmoothnessBufferReason.c_str());
                }
            }
            lastDeferredLineage = InjectFrameLineage{};
            ResetWarmupWgcFreshness(false);
            if (media_main_g_HasLastFrame && media_main_g_LastFrame.isInjectMode && !useScreenGrab) {
                media_main_g_LastFrame = QueuedFrame{};
                media_main_g_HasLastFrame = false;
            }
            SetRecordingVisibleState(true);
            LogInfo("[EncoderThread] Recording live (%s, hiddenFrames=%u, bufferedInject=%zu)",
                    useScreenGrab ? "WGC" : "inject", hiddenStartupFrames, bufferedInjectFrames.size());
        }

        if (!recordingOutputLive) {
            if (popped) {
                if (useScreenGrab) {
                    TrackWarmupWgcFreshFrame(frame);
                }
                ++hiddenStartupFrames;
                warmupState.hiddenStartupFrames = hiddenStartupFrames;
                DiscardQueuedFrame(frame);
            }
            continue;
        }

        if (popped) {
            if (!config.video.useVFR && encoderGridStartQpc == 0) {
                encoderGridStartQpc = frame.timestamp;
            }
            // Keep the last successfully emitted frame authoritative until the
            // fresh candidate has actually encoded. This also keeps inject ring
            // leases attached to deferred candidates instead of accidentally
            // moving them into g_LastFrame before the fence result is known.
            frameToProcess = &frame;
        } else if (media_main_g_HasLastFrame && media_main_g_EncoderRunning && media_main_g_Recording) {
            if (hasRepeatLastFramePath) {
                wantsTrueRepeatLastFrame = true;
                isDuplicate = true;
            } else {
                if (!media_main_g_LastFrame.isInjectMode && media_main_g_LastFrame.timestamp > 0) {
                    lastEmittedWgcSourceQpc = media_main_g_LastFrame.timestamp;
                }
                if (!media_main_g_LastFrame.isInjectMode && GetFrameSelectionTimestamp(media_main_g_LastFrame) > 0) {
                    lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(media_main_g_LastFrame);
                }
                frameToProcess = &media_main_g_LastFrame;
                isDuplicate = true;
            }
        }

        const bool refreshedDrainOutstandingLiveTicks = !media_main_g_EncoderRunning &&
                                                        media_main_g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                                        recordingOutputLive && !config.video.useVFR;
        if (!popped && !drainingOutstandingLiveTicks && refreshedDrainOutstandingLiveTicks) {
            LogInfo("[EncoderThread] CFR stop drain picked up mid-cycle");
            continue;
        }
        drainingOutstandingLiveTicks = refreshedDrainOutstandingLiveTicks;

        if (!media_main_g_EncoderRunning && !popped && !drainingOutstandingLiveTicks) {
            break;
        }

        const bool consumesCfrTick =
            !config.video.useVFR && ((media_main_g_EncoderRunning && media_main_g_Recording) || drainingOutstandingLiveTicks);
        const bool isDrainPhase = !media_main_g_Recording.load(std::memory_order_acquire);
        const bool isLivePhase =
            recordingOutputLive && (media_main_g_Recording.load(std::memory_order_acquire) || drainingOutstandingLiveTicks);
        const bool scheduledLiveCfrTick = consumesCfrTick && isLivePhase;
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

        auto recordDuplicate = [&](const QueuedFrame* duplicateFrame, const InjectFrameLineage* duplicateLineage,
                                   bool duplicateFromDrainReason, bool duplicateFromDeferredReason,
                                   bool duplicateFromTimerRebaseReason, bool duplicateFromCatchupReason = false,
                                   bool duplicateFromCapacityPacerReason = false) {
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
        };
        auto advanceWakeDeadlineForCatchupTick = [&]() {
            if (ce::capture_policy::ShouldAdvanceWakeDeadlineForCfrCatchupTick(useScreenGrab,
                                                                               injectCfrRecoveryActive)) {
                nextSampleTime.QuadPart += targetIntervalTicks;
            }
        };
        auto emitCatchupRepeats = [&](const InjectFrameLineage* duplicateLineage) {
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
        };

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
                continue;
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
                                wgcProactiveOverloadRepeatThisTick);
                cadenceCounters.liveTickEmitCount++;
                cadenceCounters.liveTickDuplicateCount++;
                cadenceCounters.holdTicksRunning++;
                ++liveTicksOutput;
                emitCatchupRepeats(duplicateLineage.IsValid() ? &duplicateLineage : nullptr);
            } else {
                cadenceCounters.liveTickMissCount++;
            }
            continue;
        }

        if (frameToProcess) {
            LARGE_INTEGER startEnc, endEnc;
            QueryPerformanceCounter(&startEnc);

            bool encodeSucceeded = true;
            bool encodeDeferred = false;
            const bool duplicateFromDrain = isDuplicate && isDrainPhase;

            const int64_t idealQpc =
                (encoderGridStartQpc > 0 && targetIntervalTicks > 0)
                    ? ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTick, targetIntervalTicks)
                    : 0;
            int64_t signedSelectionErrorUs = 0;
            int64_t absoluteSelectionErrorUs = 0;
            int64_t signedRawSelectionErrorUs = 0;
            const bool firstTransactionalWgcFrame = !frameToProcess->isInjectMode && liveTicksOutput == 0 &&
                                                    pendingWgcStartContract.valid;
            const int64_t selectionMetricTargetQpc =
                firstTransactionalWgcFrame
                    ? pendingWgcStartContract.videoOriginQpc
                    : (!frameToProcess->isInjectMode
                           ? (wgcSelectionDelayAppliedThisTick ? computeDelayedWgcSelectionTargetQpc()
                                                               : computeLiveWgcSelectionTargetQpc())
                           : idealQpc);
            if (selectionMetricTargetQpc > 0) {
                const int64_t selectionTimestampQpc = !frameToProcess->isInjectMode
                                                          ? GetFrameSelectionTimestamp(*frameToProcess)
                                                          : frameToProcess->timestamp;
                signedSelectionErrorUs =
                    ((selectionTimestampQpc - selectionMetricTargetQpc) * 1000000) / qpcFreq.QuadPart;
                absoluteSelectionErrorUs =
                    signedSelectionErrorUs >= 0 ? signedSelectionErrorUs : -signedSelectionErrorUs;
                if (!frameToProcess->isInjectMode) {
                    const int64_t rawSelectionTimestampQpc = getWgcRawSelectionTimestamp(*frameToProcess);
                    if (rawSelectionTimestampQpc > 0) {
                        signedRawSelectionErrorUs =
                            ((rawSelectionTimestampQpc - selectionMetricTargetQpc) * 1000000) / qpcFreq.QuadPart;
                    } else {
                        signedRawSelectionErrorUs = signedSelectionErrorUs;
                    }
                }
            }

            uint64_t frameAgeUs = 0;
            if (frameToProcess->timestamp > 0 && startEnc.QuadPart > frameToProcess->timestamp) {
                frameAgeUs =
                    static_cast<uint64_t>((startEnc.QuadPart - frameToProcess->timestamp) * 1000000 / qpcFreq.QuadPart);
            }
            if (scheduledLiveCfrTick && scheduledOutputQpc > 0) {
                const int64_t signedOutputScheduleErrorUs =
                    ((startEnc.QuadPart - scheduledOutputQpc) * 1000000) / qpcFreq.QuadPart;
                cadenceCounters.RecordOutputScheduleError(signedOutputScheduleErrorUs);
            }

            auto encodeCurrentFrame = [&]() {
                ce::cursor::CaptureState scheduledCursorState;
                const ce::cursor::CaptureState* cursorState = &frameToProcess->cursorState;
                if (scheduledLiveCfrTick && scheduledOutputQpc > 0) {
                    scheduledCursorState =
                        selectCursorStateForScheduledQpc(scheduledOutputQpc, *frameToProcess, "fresh");
                    cursorState = &scheduledCursorState;
                }
                if (frameToProcess->isInjectMode) {
                    encodeSucceeded = MediaEngine_ProcessFrame(
                        (uint64_t)frameToProcess->sharedHandle, (uint64_t)frameToProcess->fenceHandle,
                        frameToProcess->fenceValue, frameToProcess->timestamp, frameToProcess->luidLow,
                        frameToProcess->luidHigh, frameToProcess->sourcePid, frameToProcess->width,
                        frameToProcess->height, frameToProcess->format, frameToProcess->isHDR, frameToProcess->isShmem,
                        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                        frameToProcess->shmemSlot, cursorState);
                    encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                } else {
                    const int64_t liveTimelineElapsedUs =
                        scheduledLiveCfrTick ? computeLiveTimelineElapsedUs(scheduledOutputQpc) : -1;
                    const int64_t wgcMediaTimestampQpc =
                        firstTransactionalWgcFrame ? pendingWgcStartContract.videoOriginQpc
                                                   : frameToProcess->timestamp;
                    const auto privacyDecision = evaluateScreenGrabPrivacy(frameToProcess);
                    if (privacyDecision.useBlackFrame) {
                        encodeSucceeded =
                            submitPrivacyBlackFrame(*frameToProcess, wgcMediaTimestampQpc, scheduledOutputQpc,
                                                    liveTimelineElapsedUs);
                    } else {
                        SyncDuplicationCursorSuppression(frameToProcess->wgcCursorEmbedded);
                        encodeSucceeded = MediaEngine_ProcessFrameD3D11(
                            frameToProcess->texture, wgcMediaTimestampQpc, frameToProcess->width,
                            frameToProcess->height, frameToProcess->isHDR, frameToProcess->captureLeft,
                            frameToProcess->captureTop, liveTimelineElapsedUs, cursorState);
                        if (encodeSucceeded && privacyRuntime.IsEnabled()) {
                            privacyRuntime.CommitRealOutput();
                        }
                    }
                    encodeDeferred = false;
                }
            };

            const bool attemptedFreshCandidate = popped && frameToProcess == &frame && !isDuplicate;
            const bool attemptedFreshWgcCandidate =
                attemptedFreshCandidate && frameToProcess && !frameToProcess->isInjectMode;
            encodeCurrentFrame();
            const bool recoveredFreshEncodeFailure =
                !encodeSucceeded &&
                recoverScheduledFreshEncodeFailure(scheduledLiveCfrTick, encodeSucceeded, encodeDeferred,
                                                   scheduledOutputQpc, frameToProcess, "main fresh frame");
            if (recoveredFreshEncodeFailure) {
                encodeSucceeded = true;
                encodeDeferred = false;
                isDuplicate = true;
            }

            if (attemptedFreshCandidate && !encodeDeferred) {
                if (recoveredFreshEncodeFailure) {
                    // The scheduled output contains the previous cached frame,
                    // not this candidate. Consume its ownership without
                    // changing last-successful source metadata.
                    if (frame.isInjectMode) {
                        frame.injectRingLease.Reset();
                    } else {
                        releaseWgcLeaseAfterMediaEngineCopy(frame, "main encode-failure repeat");
                        ReleaseQueuedFrameTexture(frame);
                    }
                    frame = QueuedFrame{};
                    frameToProcess = media_main_g_HasLastFrame ? &media_main_g_LastFrame : nullptr;
                    popped = false;
                } else if (encodeSucceeded) {
                    if (frame.isInjectMode) {
                        // The synchronous call has finished using the shared
                        // slot. Deferred candidates never enter this branch and
                        // retain their lease while queued for retry.
                        frame.injectRingLease.Reset();
                    } else {
                        releaseWgcLeaseAfterMediaEngineCopy(frame, "main");
                    }
                    if (media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode) {
                        ReleaseQueuedFrameTexture(media_main_g_LastFrame);
                    }
                    media_main_g_LastFrame = std::move(frame);
                    frame = QueuedFrame{};
                    media_main_g_HasLastFrame = true;
                    frameToProcess = &media_main_g_LastFrame;
                } else {
                    // A hard fresh-frame failure consumed the synchronous call
                    // but emitted nothing. Release the candidate (including its
                    // inject ring lease) and preserve g_LastFrame unchanged.
                    if (frame.isInjectMode) {
                        frame.injectRingLease.Reset();
                    } else {
                        ReleaseQueuedFrameTexture(frame);
                    }
                    frame = QueuedFrame{};
                    frameToProcess = nullptr;
                    popped = false;
                }
            } else if (encodeSucceeded && frameToProcess && !frameToProcess->isInjectMode) {
                releaseWgcLeaseAfterMediaEngineCopy(*frameToProcess, "main duplicate fallback");
            }

            QueryPerformanceCounter(&endEnc);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            double currentEncodeMs = (double)(endEnc.QuadPart - startEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
            double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
            if (pureEncodeMs > 0.0) {
                if (smoothedEncodeMs == 0.0) {
                    smoothedEncodeMs = pureEncodeMs;
                } else {
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - media_main_kEncodeEmaAlpha) + pureEncodeMs * media_main_kEncodeEmaAlpha;
                }
            }
            if (attemptedFreshWgcCandidate && encodeSucceeded && !recoveredFreshEncodeFailure) {
                ce::capture_policy::UpdateWgcServiceTimeEma(currentEncodeMs, pureEncodeMs, media_main_kEncodeEmaAlpha,
                                                            smoothedWgcFreshServiceMs, wgcFreshServiceSamples);
            }
            const bool encoderStartupWindowActive =
                ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, GetTickCount64());
            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs, encoderStartupWindowActive);

            if (popped && frameToProcess->isInjectMode) {
                if (encodeDeferred) {
                    const InjectFrameLineage deferredLineage = MakeInjectFrameLineage(*frameToProcess);
                    frameCreditAccumulator = std::max(frameCreditAccumulator, 1.0);
                    media_main_g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                    if (media_main_g_pSharedMem) {
                        media_main_g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                        if (lastDeferredLineage.IsValid() &&
                            MatchesInjectFrameLineage(*frameToProcess, lastDeferredLineage)) {
                            media_main_g_pSharedMem->runtimeState.repeatedDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    cadenceCounters.consecutiveDeferredFrames++;
                    cadenceCounters.maxConsecutiveDeferredFrames = std::max(
                        cadenceCounters.maxConsecutiveDeferredFrames, cadenceCounters.consecutiveDeferredFrames);
                    lastDeferredLineage = deferredLineage;
                    QueuedFrame deferredFrame = std::move(frame);
                    frame = QueuedFrame{};
                    deferredFrame.deferCount++;
                    if (!media_main_g_RejectInjectFrames.load(std::memory_order_acquire) &&
                        deferredFrame.deferCount <= ce::capture_policy::kMaxInjectDeferredFrameRetries) {
                        bufferedInjectFrames.push_front(std::move(deferredFrame));
                        ++injectDeferredRequeuedThisWindow;
                        ++injectDeferredRequeuedTotal;
                    } else {
                        DiscardQueuedFrame(deferredFrame);
                        ++injectDeferredDroppedThisWindow;
                        ++injectDeferredDroppedTotal;
                    }
                    frameToProcess = nullptr;
                    popped = false;
                    static uint64_t s_lastDeferredLogTick = 0;
                    uint64_t nowTick = GetTickCount64();
                    if (nowTick - s_lastDeferredLogTick >= 1000) {
                        LogInfo(
                            "[EncoderThread] Deferred inject frame=%u ring=%u tex=%d fence=%llu ts=%lld buffered=%zu "
                            "credit=%.3f requeued=%llu dropped=%llu",
                            deferredLineage.frameIndex, deferredLineage.ringIndex, deferredLineage.textureIndex,
                            static_cast<unsigned long long>(deferredLineage.fenceValue),
                            static_cast<long long>(deferredLineage.timestamp), bufferedInjectFrames.size(),
                            frameCreditAccumulator, static_cast<unsigned long long>(injectDeferredRequeuedTotal),
                            static_cast<unsigned long long>(injectDeferredDroppedTotal));
                        s_lastDeferredLogTick = nowTick;
                    }

                    if (consumesCfrTick && isLivePhase && hasRepeatLastFramePath) {
                        isDuplicate = true;
                        duplicateFromDeferred = true;
                        encodeSucceeded = repeatLastFrameForScheduledQpc(scheduledOutputQpc);
                        encodeDeferred = MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                        if (!encodeSucceeded || encodeDeferred) {
                            if (scheduledLiveCfrTick) {
                                cadenceCounters.liveTickMissCount++;
                            }
                            continue;
                        }
                    } else {
                        if (scheduledLiveCfrTick) {
                            cadenceCounters.liveTickMissCount++;
                        }
                        continue;
                    }
                }

                if (media_main_g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    media_main_g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                }

                if (popped && frameToProcess && frameToProcess->isInjectMode && encodeSucceeded) {
                    const double currentFenceMs = (double)MediaEngine_GetLastFrameFenceWaitUs() / 1000.0;
                    if (smoothedInjectFenceMs == 0.0) {
                        smoothedInjectFenceMs = currentFenceMs;
                    } else {
                        smoothedInjectFenceMs = smoothedInjectFenceMs * 0.90 + currentFenceMs * 0.10;
                    }
                }
                static DWORD lastWarningTime = 0;
                if (ce::capture_policy::ShouldWarnEncoderApproachingCapacity(
                        smoothedEncodeMs, frameIntervalMs, encoderStartupWindowActive)) {
                    DWORD now = GetTickCount();
                    if (now - lastWarningTime > 5000) {
                        LogWarn("Encoder approaching capacity: %.2fms avg vs %.2fms budget", smoothedEncodeMs,
                                frameIntervalMs);
                        lastWarningTime = now;
                    }
                }

                cadenceCounters.consecutiveDeferredFrames = 0;

                if (!isDuplicate && frameToProcess && frameToProcess->frameIndex != 0) {
                    if (lastEncodedInjectFrameIndex != 0 && frameToProcess->frameIndex < lastEncodedInjectFrameIndex) {
                        LogWarn(
                            "[EncoderThread] Inject lineage regression: encoded frame=%u after frame=%u (ring=%u "
                            "tex=%d ts=%lld)",
                            frameToProcess->frameIndex, lastEncodedInjectFrameIndex, frameToProcess->ringIndex,
                            frameToProcess->textureIndex, static_cast<long long>(frameToProcess->timestamp));
                        if (media_main_g_pSharedMem) {
                            media_main_g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    lastEncodedInjectFrameIndex = frameToProcess->frameIndex;
                }
                if (!isDuplicate && frameToProcess && IsInjectTextureIndexValid(frameToProcess->textureIndex)) {
                    uint32_t& lastTextureFrame =
                        lastEncodedFrameByTextureIndex[static_cast<size_t>(frameToProcess->textureIndex)];
                    if (lastTextureFrame != 0 && frameToProcess->frameIndex != 0 &&
                        frameToProcess->frameIndex <= lastTextureFrame) {
                        LogWarn(
                            "[EncoderThread] Texture slot reuse anomaly: tex=%d frame=%u previous=%u ring=%u "
                            "fence=%llu ts=%lld",
                            frameToProcess->textureIndex, frameToProcess->frameIndex, lastTextureFrame,
                            frameToProcess->ringIndex, static_cast<unsigned long long>(frameToProcess->fenceValue),
                            static_cast<long long>(frameToProcess->timestamp));
                        if (media_main_g_pSharedMem) {
                            media_main_g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    lastTextureFrame = frameToProcess->frameIndex;
                }
                lastDeferredLineage = {};

                if (encodeSucceeded && !isDuplicate && frameToProcess) {
                    if (frameToProcess->timestamp > 0) {

                        lastEmittedInjectSourceQpc = frameToProcess->timestamp;
                    }
                    lastSuccessfullyEncodedInjectLineage = MakeInjectFrameLineage(*frameToProcess);
                }

                if (media_main_g_pSharedMem) {
                    media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        media_main_g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (encodeSucceeded && frameToProcess) {
                    frameToProcess->injectRingLease.Reset();
                }
            } else {
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (encodeSucceeded && media_main_g_pSharedMem) {
                    media_main_g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        media_main_g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        media_main_g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }

            if (encodeSucceeded && !isDuplicate && frameToProcess && !frameToProcess->isInjectMode) {
                if (frameToProcess->timestamp > 0) {
                    lastEmittedWgcSourceQpc = frameToProcess->timestamp;
                }
                if (GetFrameSelectionTimestamp(*frameToProcess) > 0) {
                    lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(*frameToProcess);
                }
                lastSuccessfulWgcCursorEmbedded = frameToProcess->wgcCursorEmbedded;
                hasSuccessfulWgcCursorMetadata = true;
            }

            if (encodeSucceeded && !isDuplicate && frameToProcess) {
                cadenceCounters.frameAgeAccumUs += frameAgeUs;
                cadenceCounters.frameAgeSamples++;
                cadenceCounters.frameAgeMaxUs = std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
            }

            if (encodeSucceeded) {
                if (selectionMetricTargetQpc > 0 && frameToProcess && !frameToProcess->isInjectMode && !isDuplicate &&
                    wgcSelectionDelayAppliedThisTick && scheduledLiveCfrTick && !wgcDelayRealizationRecordedThisTick) {
                    recordWgcDelayRealization(signedSelectionErrorUs, signedRawSelectionErrorUs);
                }

                if (selectionMetricTargetQpc > 0 && frameToProcess && !frameToProcess->isInjectMode && !isDuplicate) {
                    cadenceCounters.RecordSelectionError(signedSelectionErrorUs);
                    wgcSelectionErrorAccumUs += static_cast<uint64_t>(absoluteSelectionErrorUs);
                    wgcSelectionErrorSignedAccumUs += signedSelectionErrorUs;
                    ++wgcSelectionErrorSamples;
                    wgcSelectionErrorMaxUs = std::max(
                        wgcSelectionErrorMaxUs, SaturatingToUint32(static_cast<uint64_t>(absoluteSelectionErrorUs)));
                    if (signedSelectionErrorUs < 0) {
                        wgcSelectionEarlyMaxUs = std::max(
                            wgcSelectionEarlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedSelectionErrorUs)));
                    } else {
                        wgcSelectionLateMaxUs = std::max(
                            wgcSelectionLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedSelectionErrorUs)));
                    }
                }

                if (isDuplicate) {
                    const InjectFrameLineage duplicateLineage =
                        recoveredFreshEncodeFailure && frameToProcess && frameToProcess->isInjectMode
                            ? lastSuccessfullyEncodedInjectLineage
                            : (frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{});
                    recordDuplicate(recoveredFreshEncodeFailure ? nullptr : frameToProcess,
                                    duplicateLineage.IsValid() ? &duplicateLineage : nullptr, duplicateFromDrain,
                                    duplicateFromDeferred, duplicateFromTimerRebase);
                } else {
                    cadenceCounters.consecutiveDuplicateFrames = 0;
                    captureSessionSummary.currentContiguousDupTicks = 0;
                }
                cadenceCounters.liveTickEmitCount += (consumesCfrTick && isLivePhase) ? 1u : 0u;
                if (consumesCfrTick && isLivePhase) {
                    if (isDuplicate) {
                        cadenceCounters.liveTickDuplicateCount++;
                        cadenceCounters.holdTicksRunning++;
                    } else {
                        cadenceCounters.liveTickUniqueCount++;
                        cadenceCounters.CommitHoldRun();
                        cadenceCounters.holdTicksRunning = 1;
                    }
                }
                if (consumesCfrTick && isLivePhase) {
                    if (liveStartQpc.QuadPart == 0 && liveTicksOutput == 0) {
                        LARGE_INTEGER afterInit;
                        QueryPerformanceCounter(&afterInit);
                        ce::capture_policy::CfrTimelineStartContract committedStartContract{};
                        const bool canCommitTransactionalWgcStart = useScreenGrab && !recoveredFreshEncodeFailure &&
                                                                    frameToProcess && !frameToProcess->isInjectMode &&
                                                                    pendingWgcStartContract.valid;
                        if (canCommitTransactionalWgcStart) {
                            committedStartContract = pendingWgcStartContract;
                        }
                        if (committedStartContract.valid) {
                            liveStartQpc.QuadPart = committedStartContract.liveQpc;
                            committedWgcStartContractGeneration = pendingWgcStartContractGeneration;
                            const int64_t selectionOriginQpc = GetFrameSelectionTimestamp(*frameToProcess);
                            const int64_t selectionOffsetUs =
                                qpcToUs(selectionOriginQpc - committedStartContract.videoOriginQpc);
                            const int64_t commitLatenessUs = qpcToUs(afterInit.QuadPart - liveStartQpc.QuadPart);
                            LogInfo(
                                "[EncoderThread] WGC CFR start contract committed after first successful encode: "
                                "generation=%llu videoQpc=%lld selectionQpc=%lld selectionOffsetUs=%lld "
                                "liveQpc=%lld contentDelayUs=%lld commitLatenessUs=%lld prewarm=%s/%lldus",
                                static_cast<unsigned long long>(committedWgcStartContractGeneration),
                                static_cast<long long>(committedStartContract.videoOriginQpc),
                                static_cast<long long>(selectionOriginQpc), static_cast<long long>(selectionOffsetUs),
                                static_cast<long long>(liveStartQpc.QuadPart),
                                static_cast<long long>(qpcToUs(committedStartContract.contentDelayQpc)),
                                static_cast<long long>(commitLatenessUs), wgcEncoderPrewarmSucceeded ? "ok" : "failed",
                                static_cast<long long>(wgcEncoderPrewarmElapsedUs));
                        } else {
                            liveStartQpc = afterInit;
                            if (useScreenGrab) {
                                LogWarn(
                                    "[EncoderThread] ERROR: WGC first frame encoded without a valid transactional "
                                    "start contract: pendingGeneration=%llu pendingValid=%d recoveredFailure=%d "
                                    "frame=%d; using encode-completion wall anchor",
                                    static_cast<unsigned long long>(pendingWgcStartContractGeneration),
                                    pendingWgcStartContract.valid ? 1 : 0, recoveredFreshEncodeFailure ? 1 : 0,
                                    frameToProcess && !frameToProcess->isInjectMode ? 1 : 0);
                            }
                        }
                        // Set warmup window: give the capture system 200ms to accumulate a small buffer
                        // before making policy decisions. Prevents early startup starvation (slow WGC
                        // callback delivery) from permanently poisoning the entire session.
                        wgcWarmupUntilQpc = afterInit.QuadPart + targetIntervalTicks * 24;
                        // Publish the shared startup anchor whenever an effective video delay exists -- the
                        // audio-latency delay OR a realized smoothness floor (video-only / low-confidence
                        // path). The audio anchor delay stays = avContentDelayQpc (true latency, 0 for the
                        // floor case): the extra smoothness/floor delay S is absorbed purely by the later
                        // live-start (scheduleOffset), so audio stays byte-exact and the floor is
                        // sync-neutral by construction (no ghost-image judder).
                        if (useScreenGrab && isWgcEffectiveContentDelayActive()) {
                            if (committedStartContract.valid) {
                                const int64_t startupVideoQpc = committedStartContract.videoOriginQpc;
                                const int64_t startupEffectiveDelayQpc = committedStartContract.contentDelayQpc;
                                const int64_t startupAudioAnchorQpc = committedStartContract.audioAnchorQpc;
                                const int64_t startupAudioAnchorDelayQpc =
                                    committedStartContract.renderLoopbackLatencyQpc;
                                wgcSmoothnessActiveDelayQpc = committedStartContract.smoothnessReserveQpc;
                                wgcAvSyncStartupVideoQpc = startupVideoQpc;
                                wgcAvSyncStartupAudioAnchorQpc = startupAudioAnchorQpc;
                                wgcAvSyncStartupEffectiveDelayQpc = startupEffectiveDelayQpc;
                                wgcAvSyncScheduleOffsetQpc = liveStartQpc.QuadPart - startupAudioAnchorQpc;
                                const int64_t requestedDelayUs =
                                    qpcFreq.QuadPart > 0 ? (startupEffectiveDelayQpc * 1000000) / qpcFreq.QuadPart : 0;
                                const int64_t audioAnchorDelayUs =
                                    qpcFreq.QuadPart > 0 ? (startupAudioAnchorDelayQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                const int64_t renderDelayUs =
                                    qpcFreq.QuadPart > 0 ? (avContentDelayQpc * 1000000) / qpcFreq.QuadPart : 0;
                                const int64_t smoothExtraDelayUs =
                                    qpcFreq.QuadPart > 0 ? (wgcSmoothnessActiveDelayQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                const int64_t startupDelayUs = requestedDelayUs;
                                const int64_t scheduleOffsetUs =
                                    qpcFreq.QuadPart > 0 ? (wgcAvSyncScheduleOffsetQpc * 1000000) / qpcFreq.QuadPart
                                                         : 0;
                                LogInfo(
                                    "[AVSyncApply] wgc_cfr_start_contract: generation=%llu videoQpc=%lld "
                                    "audioAnchorQpc=%lld "
                                    "liveStartQpc=%lld requestedDelayUs=%lld startupDelayUs=%lld "
                                    "scheduleOffsetUs=%lld selectionOffsetUs=%lld audioAnchorDelayUs=%lld "
                                    "renderDelayUs=%lld "
                                    "smoothExtraDelayUs=%lld confidence=%s reason=%s",
                                    static_cast<unsigned long long>(committedWgcStartContractGeneration),
                                    static_cast<long long>(startupVideoQpc),
                                    static_cast<long long>(startupAudioAnchorQpc),
                                    static_cast<long long>(liveStartQpc.QuadPart),
                                    static_cast<long long>(requestedDelayUs), static_cast<long long>(startupDelayUs),
                                    static_cast<long long>(scheduleOffsetUs),
                                    static_cast<long long>(
                                        qpcToUs(GetFrameSelectionTimestamp(*frameToProcess) - startupVideoQpc)),
                                    static_cast<long long>(audioAnchorDelayUs), static_cast<long long>(renderDelayUs),
                                    static_cast<long long>(smoothExtraDelayUs), config.avSyncConfidence.c_str(),
                                    config.avSyncReason.c_str());
                            } else {
                                LogInfo(
                                    "[AVSyncApply] ERROR: invalid WGC CFR start contract: videoQpc=%lld "
                                    "liveStartQpc=%lld renderDelayUs=%lld observedContentDelayUs=%lld; "
                                    "startup audio anchor not published",
                                    static_cast<long long>(frameToProcess ? frameToProcess->timestamp : 0),
                                    static_cast<long long>(liveStartQpc.QuadPart),
                                    static_cast<long long>(qpcToUs(avContentDelayQpc)),
                                    static_cast<long long>(qpcToUs(liveStartQpc.QuadPart -
                                                                   (frameToProcess ? frameToProcess->timestamp : 0))));
                            }
                        }
                        pendingWgcStartContract = {};
                        // For the selection grid, we treat the first frame as tick 1.
                        // To align future idealQpc calculations perfectly with scheduledSampleQpc,
                        // we must offset the anchor back by one target interval.
                        encoderGridStartQpc = liveStartQpc.QuadPart - targetIntervalTicks;
                        // Continue from the immutable contract grid. Deferred initialization time
                        // is commit-lateness telemetry and never changes the selected content delay.
                        nextSampleTime.QuadPart = liveStartQpc.QuadPart + targetIntervalTicks;
                        LogInfo("[EncoderThread] Anchored CFR live timeline after first frame (contract grid kept)");
                    }
                    ++liveTicksOutput;
                }
                const InjectFrameLineage catchupLineage =
                    recoveredFreshEncodeFailure && frameToProcess && frameToProcess->isInjectMode
                        ? lastSuccessfullyEncodedInjectLineage
                        : (frameToProcess ? MakeInjectFrameLineage(*frameToProcess) : InjectFrameLineage{});
                emitCatchupRepeats(catchupLineage.IsValid() ? &catchupLineage : nullptr);
            } else if (scheduledLiveCfrTick) {
                cadenceCounters.liveTickMissCount++;
            }
        }

        if (popped && !frame.isInjectMode && frame.texture) {
            frame.texture->Release();
        }

        // Track encoder processing cycle time (timer wake through end of encode)
        if (cycleStartQpc.QuadPart > 0) {
            LARGE_INTEGER cycleEndQpc;
            QueryPerformanceCounter(&cycleEndQpc);
            const double cycleMs =
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                static_cast<double>(cycleEndQpc.QuadPart - cycleStartQpc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
            if (smoothedEncCycleMs < 0.001) {
                smoothedEncCycleMs = cycleMs;
            } else {
                smoothedEncCycleMs = smoothedEncCycleMs * 0.85 + cycleMs * 0.15;
            }
            if (!activeScreenGrab && liveTicksOutput >= cycleLiveTicksOutputStart) {
                const uint64_t outputTicksThisCycle64 = liveTicksOutput - cycleLiveTicksOutputStart;
                const uint32_t outputTicksThisCycle = SaturatingToUint32(outputTicksThisCycle64);
                const double injectServiceMs =
                    ce::capture_policy::GetInjectCfrServiceMsPerOutputTick(cycleMs, outputTicksThisCycle);
                if (injectServiceMs > 0.0) {
                    if (smoothedInjectServiceMs < 0.001) {
                        smoothedInjectServiceMs = injectServiceMs;
                    } else {
                        smoothedInjectServiceMs = smoothedInjectServiceMs * 0.85 + injectServiceMs * 0.15;
                    }
                    injectServiceMaxUs = std::max(
                        injectServiceMaxUs, SaturatingToUint32(static_cast<uint64_t>(injectServiceMs * 1000.0)));
                }
            }
            encCycleMaxMs = std::max(encCycleMaxMs, static_cast<uint32_t>(cycleMs * 1000.0));
            // Log encode spikes > 10ms (pure encode, not full cycle)
            if (smoothedEncodeMs > 10.0) {
                ++encodeSpikeCountThisSecond;
                static uint32_t s_spikeLogCount = 0;
                ++s_spikeLogCount;
                if (s_spikeLogCount <= 5 || s_spikeLogCount % 120 == 0) {
                    LogInfo("[EncoderThread] Spike: encode=%.2fms cycle=%.2fms frame=%llu", smoothedEncodeMs, cycleMs,
                            static_cast<unsigned long long>(liveTicksOutput));
                }
            }
        }

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
                "[Inject CFR QUALITY SUMMARY] TargetSelect=%llu Superseded=%llu TargetHold=%llu "
                "HoldWithCandidate=%llu BufferCapTrim=%llu TargetResidualMax=%uus",
                static_cast<unsigned long long>(injectTargetSelectTotal),
                static_cast<unsigned long long>(injectTargetSupersededTotal),
                static_cast<unsigned long long>(injectTargetHoldTotal),
                static_cast<unsigned long long>(injectTargetHoldWithCandidateTotal),
                static_cast<unsigned long long>(injectBufferCapTrimTotal), injectTargetResidualMaxUs);
            if (media_main_g_pSharedMem) {
                const auto& contention = media_main_g_pSharedMem->runtimeState;
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
