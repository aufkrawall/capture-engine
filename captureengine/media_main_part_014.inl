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
                    while (g_FrameQueue.Pop(qf, 0)) {
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
                        g_FrameQueue.Size(), pendingWgcStartContract.valid ? 1 : 0,
                        static_cast<long long>(qpcToUs(pendingWgcStartContract.contentDelayQpc)));
                }
                // Reset counters so per-second logs start clean at going-live.
                g_InjectBufferedTrimmedFrames.store(0, std::memory_order_relaxed);
                g_InjectCadenceDroppedFrames.store(0, std::memory_order_relaxed);
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
                const uint32_t phase = g_pSharedMem
                                           ? g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_acquire)
                                           : static_cast<uint32_t>(CapturePipelinePhase::kIdle);
                LogInfo(
                    "[RecordingLifecycle] Warmup-to-live commit rejected (phase=%s requested=%d); pending frame "
                    "discarded",
                    CapturePipelinePhaseToString(phase), g_Recording.load(std::memory_order_acquire) ? 1 : 0);
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
            if (g_HasLastFrame && g_LastFrame.isInjectMode && !useScreenGrab) {
                g_LastFrame = QueuedFrame{};
                g_HasLastFrame = false;
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
        } else if (g_HasLastFrame && g_EncoderRunning && g_Recording) {
            if (hasRepeatLastFramePath) {
                wantsTrueRepeatLastFrame = true;
                isDuplicate = true;
            } else {
                if (!g_LastFrame.isInjectMode && g_LastFrame.timestamp > 0) {
                    lastEmittedWgcSourceQpc = g_LastFrame.timestamp;
                }
                if (!g_LastFrame.isInjectMode && GetFrameSelectionTimestamp(g_LastFrame) > 0) {
                    lastEmittedWgcSelectionQpc = GetFrameSelectionTimestamp(g_LastFrame);
                }
                frameToProcess = &g_LastFrame;
                isDuplicate = true;
            }
        }

        const bool refreshedDrainOutstandingLiveTicks = !g_EncoderRunning &&
                                                        g_DrainOutstandingCfrTicks.load(std::memory_order_acquire) &&
                                                        recordingOutputLive && !config.video.useVFR;
        if (!popped && !drainingOutstandingLiveTicks && refreshedDrainOutstandingLiveTicks) {
            LogInfo("[EncoderThread] CFR stop drain picked up mid-cycle");
            continue;
        }
        drainingOutstandingLiveTicks = refreshedDrainOutstandingLiveTicks;

        if (!g_EncoderRunning && !popped && !drainingOutstandingLiveTicks) {
            break;
        }

        const bool consumesCfrTick =
            !config.video.useVFR && ((g_EncoderRunning && g_Recording) || drainingOutstandingLiveTicks);
        const bool isDrainPhase = !g_Recording.load(std::memory_order_acquire);
        const bool isLivePhase =
            recordingOutputLive && (g_Recording.load(std::memory_order_acquire) || drainingOutstandingLiveTicks);
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
            if (g_pSharedMem) {
                g_pSharedMem->runtimeState.duplicateFrames.fetch_add(1, std::memory_order_relaxed);
                if (duplicateFromDrainReason) {
                    g_pSharedMem->runtimeState.duplicateFramesDrain.fetch_add(1, std::memory_order_relaxed);
                } else if (duplicateFromDeferredReason ||
                           (duplicateFrame && MatchesInjectFrameLineage(*duplicateFrame, lastDeferredLineage)) ||
                           (duplicateLineage && MatchesInjectFrameLineage(*duplicateLineage, lastDeferredLineage))) {
                    g_pSharedMem->runtimeState.duplicateFramesDeferred.fetch_add(1, std::memory_order_relaxed);
                } else if (duplicateFromTimerRebaseReason) {
                    g_pSharedMem->runtimeState.duplicateFramesTimerRebase.fetch_add(1, std::memory_order_relaxed);
                } else if (!duplicateFromCapacityPacerReason) {
                    g_pSharedMem->runtimeState.duplicateFramesNoSource.fetch_add(1, std::memory_order_relaxed);
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
            if (!scheduledLiveCfrTick || catchupTicksThisLoop <= 1 || !g_HasLastFrame) {
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
                          catchupTicksThisLoop, g_IsEncoderBottlenecked.load(std::memory_order_relaxed),
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
                    const bool encoderBottleneckedNow = g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
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
                                g_InjectCadenceDroppedFrames.fetch_add(1, std::memory_order_relaxed);
                                if (g_pSharedMem) {
                                    g_pSharedMem->runtimeState.injectCadenceDrops.fetch_add(1,
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
                                catchupFrame.format, catchupFrame.isHDR, catchupFrame.isShmem, catchupFrame.shmemSlot,
                                &catchupFrame.cursorState);
                            const bool catchupEncodeDeferred =
                                MediaEngine_WasLastFrameDeferred && MediaEngine_WasLastFrameDeferred();
                            QueryPerformanceCounter(&catchupEndEnc);

                            const double currentEncodeMs =
                                (double)(catchupEndEnc.QuadPart - catchupStartEnc.QuadPart) * 1000.0 / qpcFreq.QuadPart;
                            const double pureEncodeMs = (double)MediaEngine_GetLastFrameEncodeTimeUs() / 1000.0;
                            if (pureEncodeMs > 0.0) {
                                if (smoothedEncodeMs == 0.0) {
                                    smoothedEncodeMs = pureEncodeMs;
                                } else {
                                    smoothedEncodeMs =
                                        smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                                }
                            }
                            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                                        ce::capture_policy::IsEncoderStartupWindow(
                                                            recordingOutputLive, recordingLiveTick, GetTickCount64()));

                            if (catchupEncodeSucceeded && !catchupEncodeDeferred) {
                                if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                                    ReleaseQueuedFrameTexture(g_LastFrame);
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
                                        if (g_pSharedMem) {
                                            g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(
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
                                        if (g_pSharedMem) {
                                            g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(
                                                1, std::memory_order_relaxed);
                                        }
                                    }
                                    lastTextureFrame = catchupFrame.frameIndex;
                                }

                                if (g_pSharedMem) {
                                    if (currentEncodeMs > frameIntervalMs * 1.10) {
                                        g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                                    }
                                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1,
                                                                                           std::memory_order_relaxed);
                                }

                                catchupFrame.injectRingLease.Reset();
                                g_LastFrame = std::move(catchupFrame);
                                g_HasLastFrame = true;
                                lastSuccessfullyEncodedInjectLineage = catchupLineage;
                                if (g_LastFrame.timestamp > 0) {
                                    lastEmittedInjectSourceQpc = g_LastFrame.timestamp;
                                }
                                lastDeferredLineage = {};
                                ++injectTargetSelectThisWindow;
                                ++injectTargetSelectTotal;
                                if (qpcFreq.QuadPart > 0) {
                                    const uint64_t residualUs =
                                        ce::capture_policy::GetCfrTimestampDistanceQpc(
                                            g_LastFrame.timestamp, catchupPlayoutTargetQpc) *
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
                                g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                                if (g_pSharedMem) {
                                    g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                                }
                                cadenceCounters.consecutiveDeferredFrames++;
                                cadenceCounters.maxConsecutiveDeferredFrames =
                                    std::max(cadenceCounters.maxConsecutiveDeferredFrames,
                                             cadenceCounters.consecutiveDeferredFrames);
                                lastDeferredLineage = catchupLineage;
                                catchupFrame.deferCount++;
                                if (!g_RejectInjectFrames.load(std::memory_order_acquire) &&
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
