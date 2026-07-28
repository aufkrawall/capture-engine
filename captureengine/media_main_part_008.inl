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
            dropWgcVisualTimelineDebtToLiveWindow(g_Recording.load(std::memory_order_acquire) ? "live" : "drain");
        }
        const bool recordingActive = g_Recording.load(std::memory_order_acquire);
        const bool drainOutstandingCfrTicks = g_DrainOutstandingCfrTicks.load(std::memory_order_acquire);
        if (ce::capture_policy::ShouldAbortCfrStopDrainBeforeOutputIsLive(recordingActive, recordingOutputLive,
                                                                          drainOutstandingCfrTicks)) {
            LogWarn(
                "[EncoderThread] CFR stop drain skipped before first live video frame; no output timeline or "
                "captured frame exists to drain");
            g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
        }
        if (!recordingActive && recordingOutputLive && drainOutstandingCfrTicks) {
            const bool mediaEngineCanRepeatLastFrame =
                MediaEngine_CanRepeatLastFrame && MediaEngine_CanRepeatLastFrame();
            if (activeScreenGrab && !bufferedWgcFrames.empty()) {
                int64_t drainPolicyQpc = g_CfrDrainStopQpc.load(std::memory_order_acquire);
                if (drainPolicyQpc <= 0) {
                    LARGE_INTEGER drainNowQpc;
                    QueryPerformanceCounter(&drainNowQpc);
                    drainPolicyQpc = drainNowQpc.QuadPart;
                }
                pruneStaleWgcVisualDebt(drainPolicyQpc, "stop-drain",
                                        g_HasLastFrame && mediaEngineCanRepeatLastFrame, 0);
            }
            const bool bufferedFrameAvailable =
                activeScreenGrab ? !bufferedWgcFrames.empty() : !bufferedInjectFrames.empty();
            const size_t bufferedFrameCount = activeScreenGrab ? bufferedWgcFrames.size() : bufferedInjectFrames.size();
            const bool canDrainOutstandingTicks = ce::capture_policy::CanDrainOutstandingCfrTicks(
                activeScreenGrab, g_FrameQueue.Size() > 0, bufferedFrameAvailable, g_HasLastFrame,
                mediaEngineCanRepeatLastFrame);
            static uint64_t s_lastStopDrainProgressLogTick = 0;
            const uint64_t nowTick = GetTickCount64();
            if (outputShortfallTicks > 0 && nowTick - s_lastStopDrainProgressLogTick >= 5000) {
                LogInfo(
                    "[EncoderThread] CFR stop drain progress: shortfall=%u/%.1fms queue=%u buffered=%zu hostLast=%d "
                    "cachedRepeat=%d",
                    outputShortfallTicks,
                    ce::capture_policy::GetCfrShortfallDurationMs(outputShortfallTicks, frameIntervalMs),
                    static_cast<unsigned>(g_FrameQueue.Size()), bufferedFrameCount, g_HasLastFrame ? 1 : 0,
                    mediaEngineCanRepeatLastFrame ? 1 : 0);
                s_lastStopDrainProgressLogTick = nowTick;
            }
            if (activeScreenGrab && outputShortfallTicks > 0 && !bufferedFrameAvailable && g_FrameQueue.Size() == 0 &&
                g_HasLastFrame && mediaEngineCanRepeatLastFrame && !wgcStopDrainHeldFrameLogged) {
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
                        static_cast<unsigned>(g_FrameQueue.Size()), bufferedFrameCount, g_HasLastFrame ? 1 : 0,
                        mediaEngineCanRepeatLastFrame ? 1 : 0);
                }
                s_lastStopDrainProgressLogTick = 0;
                g_DrainOutstandingCfrTicks.store(false, std::memory_order_release);
            }
        }

        const bool frameAvailableForCatchup =
            activeScreenGrab ? (!bufferedWgcFrames.empty()) : (!bufferedInjectFrames.empty());
        bool shouldCatchUpToWallClock = false;
        uint32_t catchupTicksThisLoop = 0;
        const auto loadWgcAudioLeadExcessMs = [&]() -> double {
            if (!g_pSharedMem) {
                return 0.0;
            }
            const uint32_t audioLeadExcessSamples =
                g_pSharedMem->runtimeState.wgcAudioLeadExcessSamples.load(std::memory_order_relaxed);
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
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed), effectiveDeliveredFps,
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
                encoderTooSlowForTargetCurrent || g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
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
                    g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
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
                        g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0,
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
                                                                frameAvailableForCatchup, g_HasLastFrame);
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
        bool drainingOutstandingLiveTicks = !g_EncoderRunning &&
                                            g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                            recordingOutputLive && !config.video.useVFR;
        if (g_EncoderRunning || drainingOutstandingLiveTicks) {
            scheduledSampleQpc = nextSampleTime.QuadPart;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (g_EncoderRunning) {
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
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.encoderTimerWakeLateAvgUs.store(
                            SaturatingToUint32(encoderWakeLateAccumUs / encoderWakeLateSamples),
                            std::memory_order_relaxed);
                        g_pSharedMem->runtimeState.encoderTimerWakeLateMaxUs.store(encoderWakeLateMaxUs,
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

            if (g_EncoderRunning) {
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
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.timerRebases.fetch_add(1, std::memory_order_relaxed);
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
            const bool encoderBottlenecked = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
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

            pruneStaleWgcVisualDebt(liveNowQpc, "selection", g_HasLastFrame && !g_LastFrame.isInjectMode,
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
            const uint32_t wgcSourceJitterAvgUs = g_WgcCap ? SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs()) : 0u;
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
