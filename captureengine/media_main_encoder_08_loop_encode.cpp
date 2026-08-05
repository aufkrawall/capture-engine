#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopEncode() {
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
            continueMainLoop = true;
            return;
                        }
                    } else {
                        if (scheduledLiveCfrTick) {
                            cadenceCounters.liveTickMissCount++;
                        }
            continueMainLoop = true;
            return;
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
}
