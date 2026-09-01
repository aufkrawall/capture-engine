#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopStartup() {
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
        injectReserveFrames = (!useScreenGrab)
                                               ? ce::capture_policy::GetInjectReserveFrames(
                                                     config.video.useVFR, smoothedInjectFenceMs, frameIntervalMs)
                                               : 0;
        if (!recordingOutputLive && !pendingLiveActivation && media_main_g_Recording && media_main_g_EncoderRunning) {
            warmupElapsedMs64 = GetTickCount64() - startupWarmupStartTick;
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
            CommitWarmupSync();

            // The WGC CFR startup-sync phase (CommitWarmupSync) is a
            // multi-iteration state machine: pre-live delay, barrier arm,
            // frame-past-barrier wait, transactional prewarm, and the
            // delay-reserve wait each return early with continueMainLoop
            // (or breakMainLoop on prewarm failure). The original monolithic
            // loop used `continue`/`break` for those states, which skipped the
            // go-live reset below. Calling CommitWarmupReset unconditionally
            // here committed the live timeline before the start contract was
            // selected, so the first encoded frame always fell back to the
            // encode-completion wall anchor (wgc_start_contract_error).
            // Restore the exact original semantics: only proceed to the reset
            // when the sync phase reached its terminal contract-selected state.
            if (!continueMainLoop && !breakMainLoop) {
                CommitWarmupReset();
            }
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
            continueMainLoop = true;
            return;
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
            continueMainLoop = true;
            return;
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
            continueMainLoop = true;
            return;
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
            continueMainLoop = true;
            return;
        }
        drainingOutstandingLiveTicks = refreshedDrainOutstandingLiveTicks;

        if (!media_main_g_EncoderRunning && !popped && !drainingOutstandingLiveTicks) {
            breakMainLoop = true;
            return;
        }
}

void MediaEncoderSession::CommitWarmupReset() {
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
injectOverloadRepeatRuntime = {};
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
