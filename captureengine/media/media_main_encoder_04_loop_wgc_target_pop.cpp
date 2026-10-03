#include "media_main_internal.h"
#include "media_main_encoder_session.h"

bool MediaEncoderSession::tryPopBufferedWgcFrameForTarget(int64_t selectionTargetQpc, int64_t liveSelectionTargetQpc, int64_t liveNowQpc, bool selectionDelayApplied, QueuedFrame* selectedFrame, bool* repeatedBecauseNoFrameCoverage) {
    this->selectionTargetQpc = selectionTargetQpc;
    this->liveSelectionTargetQpc = liveSelectionTargetQpc;
    this->liveNowQpc = liveNowQpc;
    this->selectionDelayApplied = selectionDelayApplied;
    this->selectedFrame = selectedFrame;
    this->repeatedBecauseNoFrameCoverage = repeatedBecauseNoFrameCoverage;
    // Dispatch to the selection chunks (decomposed from the original tryPop lambda).
    return TryPopWgcSelectionCore();
}

bool MediaEncoderSession::TryPopWgcSelectionCore() {
if (repeatedBecauseNoFrameCoverage) {
    *repeatedBecauseNoFrameCoverage = false;
}
if (!selectedFrame || bufferedWgcFrames.empty()) {
    return false;
}

pruneStaleWgcVisualDebt(liveNowQpc, "selection", media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode,
                        selectionTargetQpc);

lowSourceMode = wgcLowSourceModeActive;
deepUnderfeed = ce::capture_policy::IsWgcDeepUnderfeed(
    outputFps, wgcRecentDeliveredMin250Fps, wgcRecentInputMin250Fps, wgcNoFreshTickPermille);
// When an A/V content delay is active, a GPU-bound source that under-delivers cannot
// sustain the delay reservoir. Defending it per-tick by selecting older-than-target
// frames (reserve-preservation index-0 bias + soft-late older search) perturbs the
// otherwise-uniform CFR cadence into abnormal judder. In uniform-cadence mode we take
// the closest-to-target frame (monotonic + hard-cap guards stay intact) and let the
// realized content delay float gracefully; sync-safe relaxed rescue paths are kept.
preferUniformActiveDelayCadence = ce::capture_policy::IsWgcActiveDelayUniformCadenceMode(
    selectionDelayApplied, config.wgcActiveDelayUniformCadence);
activeDelayTelemetry.outputFps = outputFps;
activeDelayTelemetry.recentDeliveredFps = wgcRecentDeliveredFps;
activeDelayTelemetry.recentDeliveredMin250Fps = wgcRecentDeliveredMin250Fps;
activeDelayTelemetry.recentDeliveredMin500Fps = wgcRecentDeliveredMin500Fps;
activeDelayTelemetry.recentInputMin250Fps = wgcRecentInputMin250Fps;
activeDelayTelemetry.recentInputMin500Fps = wgcRecentInputMin500Fps;
wgcSourceJitterAvgUs = media_main_g_WgcCap ? SaturatingToUint32(media_main_g_WgcCap->GetSourceJitterAvgUs()) : 0u;
wgcPredictorJitterUs =
    wgcInputPredictor.IsCalibrated()
        ? SaturatingToUint32(static_cast<uint64_t>(wgcInputPredictor.GetJitterUs(qpcFreq.QuadPart)))
        : 0u;
activeDelayTelemetry.averageJitterUs = std::max(wgcSourceJitterAvgUs, wgcPredictorJitterUs);
activeDelayTelemetry.emptyTickPermille = wgcNoFreshTickPermille;
activeDelayTelemetry.bufferedWgcFrames =
    static_cast<uint32_t>(std::min<size_t>(bufferedWgcFrames.size(), static_cast<size_t>(UINT32_MAX)));
effectiveSelectionTargetQpc =
    selectionTargetQpc > 0 ? selectionTargetQpc : liveSelectionTargetQpc;
activeDelaySoftLateTargetUs =
    ce::capture_policy::GetWgcActiveDelaySoftLateTargetUs(targetIntervalTicks, qpcFreq.QuadPart);
minFreshTimestampQpc = ce::capture_policy::GetWgcMinimumFreshTimestampQpc(
    lastEmittedWgcSourceQpc, liveSelectionTargetQpc, targetIntervalTicks, lowSourceMode);
baseStaleFallbackMinTimestampQpc =
    ce::capture_policy::GetWgcStaleUniqueFallbackMinTimestampQpc(
        lastEmittedWgcSourceQpc, effectiveSelectionTargetQpc, targetIntervalTicks, lowSourceMode,
        deepUnderfeed);
staleFallbackMinTimestampQpc = baseStaleFallbackMinTimestampQpc;
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

skippedTooNewForSlot = false;
olderFrameAvoidedRepeatThisTick = false;
    return TryPopWgcSelectionFinalize();
}

bool MediaEncoderSession::TryPopWgcSelectionFinalize() {
buildCandidateList(&wgcFreshCandidateIndices, true, false);
if (wgcFreshCandidateIndices.empty()) {
    buildCandidateList(&wgcFallbackCandidateIndices, false, false);
}

candidateIndices =
    !wgcFreshCandidateIndices.empty() ? &wgcFreshCandidateIndices : &wgcFallbackCandidateIndices;
usingFreshCandidateSet = !wgcFreshCandidateIndices.empty();
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

if (selectionDelayApplied && skippedTooNewForSlot && candidateIndices->empty() && media_main_g_HasLastFrame &&
    !media_main_g_LastFrame.isInjectMode) {
    wgcRepeatRescueCandidateIndices.clear();
    ++wgcDelayRepeatRescueAttemptWindow;
    ++wgcDelayRepeatRescueAttemptTotal;
    ++wgcDelayRepeatPromotionAttemptWindow;
    ++wgcDelayRepeatPromotionAttemptTotal;
    const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(media_main_g_LastFrame);
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

selectedIndex = candidateIndices->front();
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
candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
candidateRawSelectionTimestamp = getWgcRawSelectionTimestamp(candidate);
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
encoderLimitedSmoothnessForBacktrack = isWgcEncoderLimitedSmoothnessMode();
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
selectedActiveDelayCandidateRelaxed =
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
    const int64_t repeatSelectionTimestamp = media_main_g_HasLastFrame ? GetFrameSelectionTimestamp(media_main_g_LastFrame) : 0;
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
selectedActiveDelayWindowClass =
    selectionDelayApplied && isActiveDelayCandidateHardSafe(candidate) ? activeDelayWindowClassFor(true)
                                                                       : activeDelayWindowClassFor(false);
selectedPostStallSafeFrame =
    selectionDelayApplied &&
    selectedActiveDelayWindowClass == ce::capture_policy::WgcActiveDelayWindowClass::kPostStallRecovery;
canHoldFreshFrame =

    selectionDelayApplied && !selectedPostStallSafeFrame && selectedIndex == 0 &&
    bufferedWgcFrames.size() == 1 &&
    ce::capture_policy::ShouldHoldSingleFreshWgcFrame(
        wgcReservePressureActive, lowSourceMode, wgcRecentInputMin250Fps, outputFps, smoothedInputPerTick,
        outputShortfallTicks, media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed), false,
        deepUnderfeed);
if (canHoldFreshFrame && effectiveSelectionTargetQpc > 0 &&
    ShouldHoldFrameForNextTick(candidateSelectionTimestamp, effectiveSelectionTargetQpc,
                               targetIntervalTicks, targetIntervalTicks / 10)) {
    const bool softSafeCandidateAvailable = isActiveDelayCandidateSoftSafe(candidate);
    recordActiveDelayRepeatLowerBound(softSafeCandidateAvailable,
                                      isCurrentSyncDelayHoldSourceLimited(softSafeCandidateAvailable));
    ++wgcHoldForNextTickCount;
    ++wgcHeldFreshFrameTickCount;
    if (repeatedBecauseNoFrameCoverage) {
        *repeatedBecauseNoFrameCoverage = true;
    }
    return false;
}
if (selectedPostStallSafeFrame) {
    ++wgcDelayPostStallSafeFrameWindow;
    ++wgcDelayPostStallSafeFrameTotal;
}

/*
if (liveSelectionTargetQpc > 0 && candidateSelectionTimestamp > liveSelectionTargetQpc && g_HasLastFrame &&
    !g_LastFrame.isInjectMode) {
    ++wgcFreshSelectionMissCount;
    if (repeatedBecauseNoFrameCoverage) {
        *repeatedBecauseNoFrameCoverage = true;
    }
    return false;
}
*/

if (!usingFreshCandidateSet) {
    ++wgcStaleUniqueFallbackCount;
    if (effectiveSelectionTargetQpc > 0 &&
        candidateSelectionTimestamp + targetIntervalTicks < effectiveSelectionTargetQpc) {
        ++wgcAncientSelectionCount;
    }
}

// Frame age limit: when the encoder is severely behind, reject frames that are
// too old.  This prevents encoding ancient content (e.g., 17 seconds old) that
// doesn't match the intended output position. Instead, we emit a duplicate frame
// which "stutters honestly" while the encoder recovers.
kMaxFrameAgeMs = 1000;  // 1 second maximum frame age
if (encoderTooSlowForTargetCurrent && effectiveSelectionTargetQpc > 0 && qpcFreq.QuadPart > 0) {
    const int64_t frameAgeTicks = effectiveSelectionTargetQpc - candidateSelectionTimestamp;
    const int64_t frameAgeMs = (frameAgeTicks * 1000) / qpcFreq.QuadPart;
    if (frameAgeMs > kMaxFrameAgeMs) {
        ++wgcFreshSelectionMissCount;
        if (repeatedBecauseNoFrameCoverage) {
            *repeatedBecauseNoFrameCoverage = true;
        }
        return false;
    }
}

for (size_t i = 0; i < selectedIndex; ++i) {
    QueuedFrame obsolete = std::move(bufferedWgcFrames.front());
    bufferedWgcFrames.pop_front();
    ReleaseQueuedFrameTexture(obsolete);
    ++wgcDropObsoleteCount;
}

if (preferUniformActiveDelayCadence) {
    ++wgcDelayUniformCadenceWindow;
    ++wgcDelayUniformCadenceTotal;
}

*selectedFrame = std::move(bufferedWgcFrames.front());
bufferedWgcFrames.pop_front();
if (selectedFrame->duplicateSourceTimestamp) {
    ++wgcSelectDuplicateSourceCount;
} else {
    ++wgcSelectFreshCount;
}

return true;
}
