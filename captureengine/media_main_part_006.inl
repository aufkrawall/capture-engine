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
        std::max<uint64_t>(100ull, static_cast<uint64_t>(frameIntervalMs * 8.0 + 0.5));
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
        if (g_WgcCap) {
            wgcStarvedEpisode.maxCallbackGapUs = std::max(
                wgcStarvedEpisode.maxCallbackGapUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetCallbackGapMaxUs()))));
            wgcStarvedEpisode.maxCopyUs = std::max(
                wgcStarvedEpisode.maxCopyUs,
                SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetLastCopyTimeUs()))));
        }
        if (g_pSharedMem) {
            const auto& runtimeState = g_pSharedMem->runtimeState;
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
        if (g_WgcCap) {
            const uint32_t currentPoolSaturatedDrops = g_WgcCap->GetPoolSaturatedDropCount();
            const uint32_t currentPoolOverwritePrevented = g_WgcCap->GetPoolSlotOverwritePreventedCount();
            const uint32_t currentIngressDecimated = g_WgcCap->GetIngressDecimatedCount();
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
        if (!g_Recording.load(std::memory_order_acquire)) {
            const int64_t drainStopQpc = g_CfrDrainStopQpc.load(std::memory_order_acquire);
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
