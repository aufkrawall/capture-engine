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
                        g_HasLastFrame && !g_LastFrame.isInjectMode &&
                        (selectionDelayApplied
                             ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks)
                             : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks));
                    if (tooNewForSlot) {
                        bool useRelaxedActiveDelayCandidate = false;
                        if (selectionDelayApplied && allowRelaxedActiveDelayResidual && g_HasLastFrame &&
                            !g_LastFrame.isInjectMode) {
                            const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
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

            if (selectionDelayApplied && skippedTooNewForSlot && candidateIndices->empty() && g_HasLastFrame &&
                !g_LastFrame.isInjectMode) {
                wgcRepeatRescueCandidateIndices.clear();
                ++wgcDelayRepeatRescueAttemptWindow;
                ++wgcDelayRepeatRescueAttemptTotal;
                ++wgcDelayRepeatPromotionAttemptWindow;
                ++wgcDelayRepeatPromotionAttemptTotal;
                const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(g_LastFrame);
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
                const int64_t repeatSelectionTimestamp = g_HasLastFrame ? GetFrameSelectionTimestamp(g_LastFrame) : 0;
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
