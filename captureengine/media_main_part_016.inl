                        lastEmittedInjectSourceQpc = frameToProcess->timestamp;
                    }
                    lastSuccessfullyEncodedInjectLineage = MakeInjectFrameLineage(*frameToProcess);
                }

                if (g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                if (encodeSucceeded && frameToProcess) {
                    frameToProcess->injectRingLease.Reset();
                }
            } else {
                cadenceCounters.consecutiveDeferredFrames = 0;
                if (encodeSucceeded && g_pSharedMem) {
                    g_pSharedMem->runtimeState.framesEncoded.fetch_add(1, std::memory_order_relaxed);
                    if (isLivePhase) {
                        g_pSharedMem->runtimeState.liveFramesEncoded.fetch_add(1, std::memory_order_relaxed);
                    } else if (isDrainPhase) {
                        g_pSharedMem->runtimeState.drainFramesEncoded.fetch_add(1, std::memory_order_relaxed);
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

        if (g_pSharedMem && GetTickCount() - lastHealthLog >= 1000) {
            auto& state = g_pSharedMem->runtimeState;
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
                if (g_WgcCap) {
                    wgcStarvedEpisode.maxCallbackGapUs = std::max(
                        wgcStarvedEpisode.maxCallbackGapUs, SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(
                                                                0, g_WgcCap->GetCallbackGapMaxUs()))));
                    wgcStarvedEpisode.maxCopyUs = std::max(
                        wgcStarvedEpisode.maxCopyUs,
                        SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, g_WgcCap->GetLastCopyTimeUs()))));
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
                g_WgcCap ? g_WgcCap->GetDuplicateTimestampSkipCount() : lastWgcDuplicateTimestampSkipCountForCadence;
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

            if (useScreenGrab && recordingOutputLive && g_WgcCap) {
                const uint32_t currentIngressAccepted = g_WgcCap->GetIngressAcceptedCount();
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
                static_cast<unsigned long long>(g_ActivePathMismatchFramesDiscarded.load(std::memory_order_relaxed)),
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
                nonMonotonicPts - lastNonMonotonicPtsCount, g_WgcProducerTargetFps.load(std::memory_order_relaxed),
                wgcProducerRateRetuneCount, overloadFlags, (muxQueueBytes + 1023u) / 1024u, muxQueuePackets,
                muxBackpressureCount, muxBackpressureWaitUs, muxBackpressureMaxWaitUs, smoothedEncodeMs,
                smoothedWgcFreshServiceMs, encoderBudgetUtilizationPermille, sustainableOutputFps,
                encoderTooSlowForTarget ? 1 : 0,
                g_IsEncoderBottlenecked.load(std::memory_order_relaxed) ? 1 : 0, wgcLowSourceModeActive ? 1 : 0,
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
                const uint32_t wgcPoolFreeNow = g_WgcCap ? g_WgcCap->GetPoolSlotFreeCurrentCount() : 0u;
                const uint32_t wgcPoolFreeMin = g_WgcCap ? g_WgcCap->GetPoolSlotFreeMinCount() : 0u;
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
