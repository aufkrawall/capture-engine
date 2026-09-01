#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopStart() {
        cycleLiveTicksOutputStart = liveTicksOutput;
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
            const uint32_t ingressQueueDepth =
                static_cast<uint32_t>(media_main_g_FrameQueue.Size());
            uint32_t queueDepth = ingressQueueDepth;
            queueDepth += static_cast<uint32_t>(bufferedInjectFrames.size());
            queueDepth += static_cast<uint32_t>(bufferedWgcFrames.size());
            const int64_t fenceWaitUs = MediaEngine_GetLastFrameFenceWaitUs();
            const bool shouldThrottle = ce::capture_policy::ShouldThrottleInjectProducer(
                ingressQueueDepth, media_main_g_FrameQueue.Capacity(), fenceWaitUs);

            media_main_g_pSharedMem->encoderQueueDepth.store(queueDepth, std::memory_order_relaxed);
            const bool wasThrottled =
                media_main_g_pSharedMem->throttleCapture.exchange(shouldThrottle,
                                                                   std::memory_order_acq_rel);
            if (wasThrottled != shouldThrottle) {
                ++injectProducerThrottleTransitionCount;
                if (injectProducerThrottleTransitionCount <= 8 ||
                    (injectProducerThrottleTransitionCount % 120ull) == 0) {
                    LogInfo(
                        "[Inject Capture] Producer throttle %s: ingressQ=%u/%zu retainedInject=%zu "
                        "retainedWgc=%zu totalOwned=%u fenceWait=%lldus reason=%s transitions=%llu",
                        shouldThrottle ? "entered" : "exited", ingressQueueDepth,
                        media_main_g_FrameQueue.Capacity(), bufferedInjectFrames.size(),
                        bufferedWgcFrames.size(), queueDepth,
                        static_cast<long long>(fenceWaitUs),
                        !shouldThrottle ? "recovered"
                        : fenceWaitUs > 16'000 ? "gpu_fence"
                                               : "ingress_queue",
                        static_cast<unsigned long long>(injectProducerThrottleTransitionCount));
                }
            }
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

        outputShortfallTicks = 0;
        activeScreenGrab = IsActiveScreenGrab();
        useScreenGrab = activeScreenGrab;
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
}

void MediaEncoderSession::LoopPressure() {
        outputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
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
            breakMainLoop = true;
            return;
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
        if (!config.video.useVFR && recordingOutputLive) {
            LARGE_INTEGER shortfallNow;
            QueryPerformanceCounter(&shortfallNow);
            outputShortfallTicks = updateLiveCfrShortfall(shortfallNow.QuadPart);
            dropWgcVisualTimelineDebtToLiveWindow(media_main_g_Recording.load(std::memory_order_acquire) ? "live" : "drain");
        }
}

void MediaEncoderSession::LoopCatchup() {
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
        catchupTicksThisLoop = 0;
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
            const uint32_t catchupOverloadFlags = loadEncoderOverloadFlags();
            const bool injectRepeatCatchupHeadroom =
                ce::capture_policy::ShouldAllowCfrRepeatCatchupUnderFreshPressure(
                    injectOverloadRepeatRuntime.pacer.active,
                    (catchupOverloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0,
                    injectOverloadRepeatRuntime.repeatServiceMs, frameIntervalMs,
                    injectOverloadRepeatRuntime.repeatServiceSamples);
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
                        "bottleneck=%d serviceSlow=%d repeatHeadroom=%d",
                        static_cast<unsigned long long>(recoveryNowTick - injectCfrRecoveryStartTick),
                        outputShortfallTicks, injectCfrRecoveryStartDebt, injectCfrRecoveryBestDebt,
                        static_cast<unsigned long long>(injectFreshCatchupTotal - injectCfrRecoveryStartFreshCatchup),
                        static_cast<unsigned long long>(injectRepeatCatchupTotal - injectCfrRecoveryStartRepeatCatchup),
                        smoothedEncodeMs, smoothedInjectServiceMs, smoothedEncCycleMs, bufferedInjectFrames.size(),
                        frameCreditAccumulator,
                        media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
                        injectEncoderServiceTooSlowCurrent ? 1 : 0, injectRepeatCatchupHeadroom ? 1 : 0);
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
                const bool injectCatchupBlocked =
                    (catchupOverloadFlags & ce::capture_policy::kEncoderOverloadFlagMux) != 0 ||
                    (!injectRepeatCatchupHeadroom &&
                     (encoderCatchupBottleneckedCurrent || injectEncoderServiceTooSlowCurrent));
                catchupTicksThisLoop = ce::capture_policy::GetInjectCfrCatchupTicksThisLoop(
                    outputShortfallTicks, injectCfrRecoveryActive, injectCatchupBlocked);
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

        selectionGridTick =
            (!config.video.useVFR && recordingOutputLive) ? (encoderGridTickCount + 1) : encoderGridTickCount;
        scheduledSampleQpc = 0;
        int64_t encoderLateQpc = 0;
        encoderLateTickCount = 0;
        drainingOutstandingLiveTicks = !media_main_g_EncoderRunning &&
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
}

uint32_t MediaEncoderSession::loadEncoderOverloadFlags() {

return media_main_g_pSharedMem ? media_main_g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed) : 0u;

}

bool MediaEncoderSession::isWgcCapacityPressureActive() {

const uint32_t overloadFlags = loadEncoderOverloadFlags();
return media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed) || encoderTooSlowForTargetCurrent ||
       (overloadFlags & (ce::capture_policy::kEncoderOverloadFlagEncoder |
                         ce::capture_policy::kEncoderOverloadFlagMux)) != 0;

}

bool MediaEncoderSession::isWgcTrueSourceStarvedForCapacityPolicy() {

return ce::capture_policy::IsWgcTrueSourceStarvedForRecovery(
    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps, wgcNoFreshTickPermille,
    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), 0xFFFFFFFFull)),
    isWgcCapacityPressureActive());

}

bool MediaEncoderSession::isWgcEncoderLimitedSmoothnessMode() {

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

}

size_t MediaEncoderSession::pruneStaleWgcVisualDebt(int64_t liveNowQpc, const char* reason, bool allowDropAll, int64_t immutableSelectionTargetQpc) {

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

}

void MediaEncoderSession::noteActivePathMismatchDiscard(bool frameIsInjectMode, const char* source) {

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

}

void MediaEncoderSession::discardActivePathMismatchFrame(QueuedFrame& mismatchedFrame, const char* source, bool queuedFrame) {


noteActivePathMismatchDiscard(mismatchedFrame.isInjectMode, source);
if (queuedFrame) {
    DiscardQueuedFrame(mismatchedFrame);
} else {
    if (!mismatchedFrame.isInjectMode) {
        ReleaseQueuedFrameTexture(mismatchedFrame);
    }
    mismatchedFrame = QueuedFrame{};
}

}
