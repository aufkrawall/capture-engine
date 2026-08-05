#include "media_main_internal.h"
#include "media_main_encoder_session.h"

uint32_t MediaEncoderSession::activeDelayRepeatClusterTicks() {

return std::max<uint32_t>(
    cadenceCounters.consecutiveDuplicateFrames,
    cadenceCounters.holdTicksRunning > 1 ? (cadenceCounters.holdTicksRunning - 1) : 0);

}

uint32_t MediaEncoderSession::currentDelayResidualAvgAbsUs() {

if (wgcDelayResidualWindowSamples > 0) {
    return SaturatingToUint32(wgcDelayResidualWindowAbsAccumUs / wgcDelayResidualWindowSamples);
}
return wgcDelayResidualSamples > 0
           ? SaturatingToUint32(wgcDelayResidualAbsAccumUs / wgcDelayResidualSamples)
           : 0u;

}

uint32_t MediaEncoderSession::currentDelayResidualP95Us() {

const uint32_t windowP95 = wgcDelayResidualWindowP95Us();
return windowP95 > 0 ? windowP95 : wgcDelayResidualP95Us();

}

uint32_t MediaEncoderSession::currentDelayResidualLateMaxUs() {

return wgcDelayResidualWindowLateMaxUs > 0 ? wgcDelayResidualWindowLateMaxUs
                                           : wgcDelayResidualLateMaxUs;

}

uint32_t MediaEncoderSession::currentRawDelayResidualAvgAbsUs() {

if (wgcDelayRawResidualWindowSamples > 0) {
    return SaturatingToUint32(wgcDelayRawResidualWindowAbsAccumUs / wgcDelayRawResidualWindowSamples);
}
return wgcDelayRawResidualSamples > 0
           ? SaturatingToUint32(wgcDelayRawResidualAbsAccumUs / wgcDelayRawResidualSamples)
           : 0u;

}

uint32_t MediaEncoderSession::currentRawDelayResidualP95Us() {

const uint32_t windowP95 = wgcDelayRawResidualWindowP95Us();
return windowP95 > 0 ? windowP95 : wgcDelayRawResidualP95Us();

}

uint32_t MediaEncoderSession::currentRawDelayResidualLateMaxUs() {

return wgcDelayRawResidualWindowLateMaxUs > 0 ? wgcDelayRawResidualWindowLateMaxUs
                                              : wgcDelayRawResidualLateMaxUs;

}

uint32_t MediaEncoderSession::currentCombinedDelayResidualAvgAbsUs() {

return std::max(currentDelayResidualAvgAbsUs(), currentRawDelayResidualAvgAbsUs());

}

uint32_t MediaEncoderSession::currentCombinedDelayResidualP95Us() {

return std::max(currentDelayResidualP95Us(), currentRawDelayResidualP95Us());

}

uint32_t MediaEncoderSession::currentCombinedDelayResidualLateMaxUs() {

return std::max(currentDelayResidualLateMaxUs(), currentRawDelayResidualLateMaxUs());

}

ce::capture_policy::WgcActiveDelayWindowClass MediaEncoderSession::activeDelayWindowClassFor(bool hardSafeCandidateAvailable) {

const bool activeDelaySourceRecovery = wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64();
return ce::capture_policy::ClassifyWgcActiveDelayWindow(
    activeDelayTelemetry, lowSourceMode, wgcLiveRecoveryModeActive, wgcSourceStarvedCurrent,
    deepUnderfeed, activeDelaySourceRecovery, hardSafeCandidateAvailable);

}

bool MediaEncoderSession::rawActiveDelayCandidateSafe(int64_t rawSelectionTimestamp) {

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

}

uint32_t MediaEncoderSession::activeDelayCandidateLateResidualUs(const QueuedFrame& candidate) {

return ce::capture_policy::GetWgcActiveDelayFinalSelectionLateResidualUs(
    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
    effectiveSelectionTargetQpc, qpcFreq.QuadPart);

}

bool MediaEncoderSession::isActiveDelayCandidateHardSafe(const QueuedFrame& candidate) {

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

}

bool MediaEncoderSession::isActiveDelayCandidateSoftSafe(const QueuedFrame& candidate) {

if (!isActiveDelayCandidateHardSafe(candidate)) {
    return false;
}
return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinSoftLateTarget(
    GetFrameSelectionTimestamp(candidate), getWgcRawSelectionTimestamp(candidate),
    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart, activeDelaySoftLateTargetUs);

}

bool MediaEncoderSession::hasActiveDelayHardSafeCandidate() {

for (const QueuedFrame& candidate : bufferedWgcFrames) {
    if (isActiveDelayCandidateHardSafe(candidate)) {
        return true;
    }
}
return false;

}

bool MediaEncoderSession::hasActiveDelaySoftSafeCandidate() {

for (const QueuedFrame& candidate : bufferedWgcFrames) {
    if (isActiveDelayCandidateSoftSafe(candidate)) {
        return true;
    }
}
return false;

}

uint32_t MediaEncoderSession::currentOldestSoftSafeAgeUs() {

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

}

uint32_t MediaEncoderSession::currentRepeatReserveSpanUs() {

if (bufferedWgcFrames.size() < 2 || qpcFreq.QuadPart <= 0) {
    return 0u;
}
const int64_t firstQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.front());
const int64_t lastQpc = GetFrameSelectionTimestamp(bufferedWgcFrames.back());
if (firstQpc <= 0 || lastQpc <= firstQpc) {
    return 0u;
}
return SaturatingToUint32(static_cast<uint64_t>((lastQpc - firstQpc) * 1000000 / qpcFreq.QuadPart));

}

void MediaEncoderSession::recordActiveDelayRepeatClass(ce::capture_policy::WgcActiveDelayWindowClass repeatWindowClass, bool hardSafeCandidateAvailable, bool softSafeCandidateAvailable) {

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

}

bool MediaEncoderSession::isCurrentSyncDelayHoldSourceLimited(bool softSafeCandidateAvailable) {

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

}

void MediaEncoderSession::recordActiveDelayRepeatLowerBound(bool softSafeCandidateAvailable, bool syncDelayHoldSourceLimited) {

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

}

void MediaEncoderSession::recordSyncDelayRepeatHold(bool countRepeatClusterPressure, bool hardSafeCandidateAvailable, bool softSafeCandidateAvailable) {

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

}

void MediaEncoderSession::buildCandidateList(std::vector<size_t>* outIndices, bool requireFresh, bool allowRelaxedActiveDelayResidual) {

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
        media_main_g_HasLastFrame && !media_main_g_LastFrame.isInjectMode &&
        (selectionDelayApplied
             ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks)
             : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                   candidateSelectionTimestamp, effectiveSelectionTargetQpc, targetIntervalTicks));
    if (tooNewForSlot) {
        bool useRelaxedActiveDelayCandidate = false;
        if (selectionDelayApplied && allowRelaxedActiveDelayResidual && media_main_g_HasLastFrame &&
            !media_main_g_LastFrame.isInjectMode) {
            const int64_t repeatSelectionTimestamp = GetFrameSelectionTimestamp(media_main_g_LastFrame);
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

}

int64_t MediaEncoderSession::bestCandidateDistance(const std::vector<size_t>& indices) {

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

}

bool MediaEncoderSession::finalSelectionWithinActiveDelayHardLimit(size_t candidateIndex) {

if (!selectionDelayApplied || effectiveSelectionTargetQpc <= 0 ||
    candidateIndex >= bufferedWgcFrames.size()) {
    return true;
}
const QueuedFrame& finalCandidate = bufferedWgcFrames[candidateIndex];
return ce::capture_policy::IsWgcActiveDelayFinalSelectionWithinHardLimit(
    GetFrameSelectionTimestamp(finalCandidate), getWgcRawSelectionTimestamp(finalCandidate),
    effectiveSelectionTargetQpc, targetIntervalTicks, qpcFreq.QuadPart);

}

uint32_t MediaEncoderSession::activeDelayLateResidualUsForIndex(size_t candidateIndex) {

if (candidateIndex >= bufferedWgcFrames.size() || effectiveSelectionTargetQpc <= 0 ||
    qpcFreq.QuadPart <= 0) {
    return 0u;
}
return activeDelayCandidateLateResidualUs(bufferedWgcFrames[candidateIndex]);

}
