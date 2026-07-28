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
                } else if (!g_EncoderRunning && !bufferedWgcFrames.empty()) {
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
                            g_WgcCap ? SaturatingToUint32(g_WgcCap->GetSourceJitterAvgUs()) : 0u;
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
                            if (playout.hold && playoutTargetQpc > 0 && !bufferedWgcFrames.empty() && g_HasLastFrame &&
                                !g_LastFrame.isInjectMode) {
                                ++wgcDelayRepeatRescueAttemptWindow;
                                ++wgcDelayRepeatRescueAttemptTotal;
                                ++wgcDelayRepeatPromotionAttemptWindow;
                                ++wgcDelayRepeatPromotionAttemptTotal;
                                const QueuedFrame& rescueCandidate = bufferedWgcFrames.front();
                                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
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
                                    g_WgcCap && ce::capture_policy::IsWgcIngressSourceAtOrAboveCfrTarget(
                                                    outputFps, wgcRecentInputMin250Fps, wgcRecentInputMin500Fps);
                                if (!uniformHoldSourceLimited) {
                                    static uint32_t s_uniformPolicyHoldLogCount = 0;
                                    if (s_uniformPolicyHoldLogCount < 5) {
                                        ++s_uniformPolicyHoldLogCount;
                                        LogWarn(
                                            "[WGC CFR] Uniform playout held while source was at/above CFR target: "
                                            "inputMin=%u/%u outputFps=%u buffered=%zu hardSafe=%d softSafe=%d "
                                            "retained=%u/%u dropIngress=%u (policy repeat; not source-limited)",
                                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u);
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
                                            g_WgcCap ? g_WgcCap->GetInputMin250Fps() : 0u,
                                            g_WgcCap ? g_WgcCap->GetInputMin500Fps() : 0u, outputFps,
                                            bufferedWgcFrames.size(), uniformHardSafeCandidate ? 1 : 0,
                                            uniformSoftSafeCandidate ? 1 : 0, uniformRepeatReserveSpanUs(),
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCount() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressRetainedFrameCap() : 0u,
                                            g_WgcCap ? g_WgcCap->GetIngressDecimatedCount() : 0u);
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
