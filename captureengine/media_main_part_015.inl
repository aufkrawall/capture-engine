
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
                                                   : smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) +
                                                         pureEncodeMs * kEncodeEmaAlpha;
                        }
                        if (freshCatchupEncodeSucceeded) {
                            ce::capture_policy::UpdateWgcServiceTimeEma(
                                currentEncodeMs, pureEncodeMs, kEncodeEmaAlpha, smoothedWgcFreshServiceMs,
                                wgcFreshServiceSamples);
                        }
                        UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                                    ce::capture_policy::IsEncoderStartupWindow(
                                                        recordingOutputLive, recordingLiveTick, GetTickCount64()));
                        if (g_pSharedMem) {
                            if (currentEncodeMs > frameIntervalMs * 1.10) {
                                g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                            }
                            g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                            g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
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
                        if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                            ReleaseQueuedFrameTexture(g_LastFrame);
                        }
                        g_LastFrame = std::move(catchupFrame);
                        g_HasLastFrame = true;
                        cadenceCounters.frameAgeAccumUs += frameAgeUs;
                        ++cadenceCounters.frameAgeSamples;
                        cadenceCounters.frameAgeMaxUs =
                            std::max(cadenceCounters.frameAgeMaxUs, SaturatingToUint32(frameAgeUs));
                        lastEmittedWgcSourceQpc = g_LastFrame.timestamp;
                        lastEmittedWgcSelectionQpc = selectedQpc;
                        lastSuccessfulWgcCursorEmbedded = g_LastFrame.wgcCursorEmbedded;
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
                        if (g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ||
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
                        smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                    }
                }
                if (useScreenGrab) {
                    ce::capture_policy::UpdateWgcServiceTimeEma(
                        currentEncodeMs, pureEncodeMs, kEncodeEmaAlpha, smoothedWgcRepeatServiceMs,
                        wgcRepeatServiceSamples);
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));

                if (g_pSharedMem) {
                    if (currentEncodeMs > frameIntervalMs * 1.10) {
                        g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                }

                recordDuplicate(&g_LastFrame, duplicateLineage, false, false, false, true);
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
            if (privacyDecision.useBlackFrame && g_HasLastFrame && !g_LastFrame.isInjectMode) {
                LARGE_INTEGER privacyVfrQpc = {};
                QueryPerformanceCounter(&privacyVfrQpc);
                const bool blackSucceeded =
                    submitPrivacyBlackFrame(g_LastFrame, privacyVfrQpc.QuadPart, privacyVfrQpc.QuadPart, -1);
                if (blackSucceeded && g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
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
                    : (g_HasLastFrame ? MakeInjectFrameLineage(g_LastFrame) : InjectFrameLineage{});
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
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                }
                if (useScreenGrab) {
                    ce::capture_policy::UpdateWgcServiceTimeEma(
                        currentEncodeMs, pureEncodeMs, kEncodeEmaAlpha, smoothedWgcRepeatServiceMs,
                        wgcRepeatServiceSamples);
                }
                UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs,
                                            ce::capture_policy::IsEncoderStartupWindow(
                                                recordingOutputLive, recordingLiveTick, GetTickCount64()));
                if (g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                } else if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
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
                    frameToProcess = g_HasLastFrame ? &g_LastFrame : nullptr;
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
                    if (g_HasLastFrame && !g_LastFrame.isInjectMode) {
                        ReleaseQueuedFrameTexture(g_LastFrame);
                    }
                    g_LastFrame = std::move(frame);
                    frame = QueuedFrame{};
                    g_HasLastFrame = true;
                    frameToProcess = &g_LastFrame;
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
                    smoothedEncodeMs = smoothedEncodeMs * (1.0 - kEncodeEmaAlpha) + pureEncodeMs * kEncodeEmaAlpha;
                }
            }
            if (attemptedFreshWgcCandidate && encodeSucceeded && !recoveredFreshEncodeFailure) {
                ce::capture_policy::UpdateWgcServiceTimeEma(currentEncodeMs, pureEncodeMs, kEncodeEmaAlpha,
                                                            smoothedWgcFreshServiceMs, wgcFreshServiceSamples);
            }
            const bool encoderStartupWindowActive =
                ce::capture_policy::IsEncoderStartupWindow(recordingOutputLive, recordingLiveTick, GetTickCount64());
            UpdateEncoderBottleneckFlag(smoothedEncodeMs, frameIntervalMs, encoderStartupWindowActive);

            if (popped && frameToProcess->isInjectMode) {
                if (encodeDeferred) {
                    const InjectFrameLineage deferredLineage = MakeInjectFrameLineage(*frameToProcess);
                    frameCreditAccumulator = std::max(frameCreditAccumulator, 1.0);
                    g_InjectDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                    if (g_pSharedMem) {
                        g_pSharedMem->runtimeState.deferredFrames.fetch_add(1, std::memory_order_relaxed);
                        if (lastDeferredLineage.IsValid() &&
                            MatchesInjectFrameLineage(*frameToProcess, lastDeferredLineage)) {
                            g_pSharedMem->runtimeState.repeatedDeferredFrames.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    cadenceCounters.consecutiveDeferredFrames++;
                    cadenceCounters.maxConsecutiveDeferredFrames = std::max(
                        cadenceCounters.maxConsecutiveDeferredFrames, cadenceCounters.consecutiveDeferredFrames);
                    lastDeferredLineage = deferredLineage;
                    QueuedFrame deferredFrame = std::move(frame);
                    frame = QueuedFrame{};
                    deferredFrame.deferCount++;
                    if (!g_RejectInjectFrames.load(std::memory_order_acquire) &&
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

                if (g_pSharedMem && currentEncodeMs > frameIntervalMs * 1.10) {
                    g_pSharedMem->runtimeState.lateFrames.fetch_add(1, std::memory_order_relaxed);
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
                        if (g_pSharedMem) {
                            g_pSharedMem->runtimeState.frameIndexRegressions.fetch_add(1, std::memory_order_relaxed);
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
                        if (g_pSharedMem) {
                            g_pSharedMem->runtimeState.textureReuseAnomalies.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    lastTextureFrame = frameToProcess->frameIndex;
                }
                lastDeferredLineage = {};

                if (encodeSucceeded && !isDuplicate && frameToProcess) {
                    if (frameToProcess->timestamp > 0) {
