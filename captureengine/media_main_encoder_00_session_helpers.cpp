#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::accumulateCaptureSummarySample(bool useScreenGrabSession, uint32_t srcFpsX100Val, uint32_t srcJitterUsVal, uint32_t dupNoSource, uint32_t dupDeferred, uint32_t dupTimer, uint32_t dupDrain, uint32_t oldestBufferedFrameAgeUs, double shortfallDurationMs, double sustainableOutputFps) {

captureSessionSummary.duplicateTicks += cadenceCounters.liveTickDuplicateCount;
if (cadenceCounters.liveTickEmitCount > 0 &&
    (cadenceCounters.liveTickDuplicateCount > captureSessionSummary.worstOneSecondRepeatCount ||
     (cadenceCounters.liveTickDuplicateCount == captureSessionSummary.worstOneSecondRepeatCount &&
      (captureSessionSummary.worstOneSecondEmitCount == 0 ||
       cadenceCounters.liveTickUniqueCount < captureSessionSummary.worstOneSecondUniqueCount)))) {
    captureSessionSummary.worstOneSecondEmitCount = cadenceCounters.liveTickEmitCount;
    captureSessionSummary.worstOneSecondUniqueCount = cadenceCounters.liveTickUniqueCount;
    captureSessionSummary.worstOneSecondRepeatCount = cadenceCounters.liveTickDuplicateCount;
}
captureSessionSummary.duplicateNoSourceTicks += dupNoSource - lastDuplicateReasonNoSource;
captureSessionSummary.duplicateDeferredTicks += dupDeferred - lastDuplicateReasonDeferred;
captureSessionSummary.duplicateTimerTicks += dupTimer - lastDuplicateReasonTimerRebase;
captureSessionSummary.duplicateDrainTicks += dupDrain - lastDuplicateReasonDrain;
captureSessionSummary.maxEncodeEmaMs = std::max(captureSessionSummary.maxEncodeEmaMs, smoothedEncodeMs);
captureSessionSummary.minEncoderSustainFps =
    std::min(captureSessionSummary.minEncoderSustainFps, sustainableOutputFps);

if (useScreenGrabSession) {
    captureSessionSummary.queueTickSamples += wgcQueueTickSampleCount;
    captureSessionSummary.noFreshTicks += wgcNoFreshTickCount;
    captureSessionSummary.noReserveTicks += wgcNoReserveTickCount;
    captureSessionSummary.worstFreshMissPermille =
        std::max(captureSessionSummary.worstFreshMissPermille, wgcNoFreshTickPermille);
    if (srcFpsX100Val > 0) {
        captureSessionSummary.worstSourceFpsX100 =
            std::min(captureSessionSummary.worstSourceFpsX100, srcFpsX100Val);
        captureSessionSummary.bestSourceFpsX100 =
            std::max(captureSessionSummary.bestSourceFpsX100, srcFpsX100Val);
    }
    if (wgcRecentInputMin250Fps > 0) {
        captureSessionSummary.worstInputMin250Fps =
            std::min(captureSessionSummary.worstInputMin250Fps, wgcRecentInputMin250Fps);
    }
    if (wgcRecentDeliveredMin250Fps > 0) {
        captureSessionSummary.worstDeliveredMin250Fps =
            std::min(captureSessionSummary.worstDeliveredMin250Fps, wgcRecentDeliveredMin250Fps);
    }
    captureSessionSummary.worstSourceJitterUs =
        std::max(captureSessionSummary.worstSourceJitterUs, srcJitterUsVal);
    captureSessionSummary.worstSelectionErrorUs =
        std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
    captureSessionSummary.worstWgcSelectionErrorUs =
        std::max(captureSessionSummary.worstWgcSelectionErrorUs, wgcSelectionErrorMaxUs);
    if (wgcSelectionErrorSamples > 0) {
        const int64_t avgContentPhaseErrorUs =
            wgcSelectionErrorSignedAccumUs / static_cast<int64_t>(wgcSelectionErrorSamples);
        captureSessionSummary.maxWgcContentPhaseErrorUs =
            std::max(captureSessionSummary.maxWgcContentPhaseErrorUs,
                     SaturatingToUint32(static_cast<uint64_t>(
                         avgContentPhaseErrorUs >= 0 ? avgContentPhaseErrorUs : -avgContentPhaseErrorUs)));
    }
    captureSessionSummary.worstOldestBufferedFrameAgeUs =
        std::max(captureSessionSummary.worstOldestBufferedFrameAgeUs, oldestBufferedFrameAgeUs);
    captureSessionSummary.maxShortfallDurationMs =
        std::max(captureSessionSummary.maxShortfallDurationMs, shortfallDurationMs);
} else {
    captureSessionSummary.worstSelectionErrorUs =
        std::max(captureSessionSummary.worstSelectionErrorUs, cadenceCounters.selectionErrorMaxUs);
    injectWorstSelectionErrorUs =
        std::max(injectWorstSelectionErrorUs, cadenceCounters.outputScheduleErrorMaxUs);
    if (srcFpsX100Val > 0) {
        injectWorstSourceFpsX100 = std::min(injectWorstSourceFpsX100, srcFpsX100Val);
        injectBestSourceFpsX100 = std::max(injectBestSourceFpsX100, srcFpsX100Val);
    }
    injectWorstSourceJitterUs = std::max(injectWorstSourceJitterUs, srcJitterUsVal);
}

}

bool MediaEncoderSession::shouldLogWgcStarvedEpisode(uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks, uint32_t peakFreshMissPermille) {

if (duplicateTicks > 0 || durationMs >= minLoggedWgcStarvedEpisodeMs) {
    return true;
}

// Suppress single-tick/no-duplicate blips that can occur when the rolling
// no-fresh telemetry briefly spikes without a visible cadence miss.
return outputTicks > 1 && peakFreshMissPermille >= ce::capture_policy::kWgcDeepUnderfeedEmptyTickPermille;

}

void MediaEncoderSession::finishWgcStarvedEpisode(uint64_t durationMs, uint64_t outputTicks, uint64_t duplicateTicks) {

++captureSessionSummary.starvedEpisodes;
const uint32_t minInputFps =
    wgcStarvedEpisode.minInputFps == std::numeric_limits<uint32_t>::max() ? 0u : wgcStarvedEpisode.minInputFps;
const uint32_t minDeliveredFps = wgcStarvedEpisode.minDeliveredFps == std::numeric_limits<uint32_t>::max()
                                     ? 0u
                                     : wgcStarvedEpisode.minDeliveredFps;
const uint32_t minBufferedFrames = wgcStarvedEpisode.minBufferedFrames == std::numeric_limits<uint32_t>::max()
                                       ? 0u
                                       : wgcStarvedEpisode.minBufferedFrames;
const uint32_t targetOutputFps = std::max<uint32_t>(1u, static_cast<uint32_t>(config.video.fps));
wgcStarvedEpisode.maxEncodeEmaMs = std::max(wgcStarvedEpisode.maxEncodeEmaMs, smoothedEncodeMs);
wgcStarvedEpisode.maxFenceUs = std::max(
    wgcStarvedEpisode.maxFenceUs,
    SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, MediaEngine_GetLastFrameFenceWaitUs()))));
if (media_main_g_WgcCap) {
    wgcStarvedEpisode.maxCallbackGapUs = std::max(
        wgcStarvedEpisode.maxCallbackGapUs,
        SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, media_main_g_WgcCap->GetCallbackGapMaxUs()))));
    wgcStarvedEpisode.maxCopyUs = std::max(
        wgcStarvedEpisode.maxCopyUs,
        SaturatingToUint32(static_cast<uint64_t>(std::max<int64_t>(0, media_main_g_WgcCap->GetLastCopyTimeUs()))));
}
if (media_main_g_pSharedMem) {
    const auto& runtimeState = media_main_g_pSharedMem->runtimeState;
    const uint32_t muxQueueBytes = runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
    wgcStarvedEpisode.maxMuxQueueKb =
        std::max(wgcStarvedEpisode.maxMuxQueueKb, (muxQueueBytes + 1023u) / 1024u);
    wgcStarvedEpisode.maxMuxBackpressureCount =
        std::max(wgcStarvedEpisode.maxMuxBackpressureCount,
                 runtimeState.muxBackpressureCount.load(std::memory_order_relaxed));
    wgcStarvedEpisode.maxMuxBackpressureWaitUs =
        std::max(wgcStarvedEpisode.maxMuxBackpressureWaitUs,
                 runtimeState.muxBackpressureMaxWaitUs.load(std::memory_order_relaxed));
    wgcStarvedEpisode.peakOverloadFlags |= runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
}
LARGE_INTEGER endQpc = {};
QueryPerformanceCounter(&endQpc);
const uint32_t frameBudgetUs = static_cast<uint32_t>(std::max(1.0, frameIntervalMs * 1000.0));
const bool muxPressure =
    wgcStarvedEpisode.maxMuxBackpressureCount > 0 || wgcStarvedEpisode.maxMuxBackpressureWaitUs > 0;
const bool capacityPressure = wgcStarvedEpisode.peakOverloadFlags != 0 || muxPressure;
const bool sourceBelowCfrTarget = minInputFps > 0 && minInputFps < targetOutputFps;
const bool deliveredBelowCfrTarget = minDeliveredFps > 0 && minDeliveredFps < targetOutputFps;
const bool callbackDeliveryGap = wgcStarvedEpisode.maxCallbackGapUs > frameBudgetUs * 2u;
uint32_t poolSaturatedDrops = 0;
uint32_t poolOverwritePrevented = 0;
uint32_t ingressDecimated = 0;
if (media_main_g_WgcCap) {
    const uint32_t currentPoolSaturatedDrops = media_main_g_WgcCap->GetPoolSaturatedDropCount();
    const uint32_t currentPoolOverwritePrevented = media_main_g_WgcCap->GetPoolSlotOverwritePreventedCount();
    const uint32_t currentIngressDecimated = media_main_g_WgcCap->GetIngressDecimatedCount();
    poolSaturatedDrops = currentPoolSaturatedDrops - wgcStarvedEpisode.startPoolSaturatedDrops;
    poolOverwritePrevented = currentPoolOverwritePrevented - wgcStarvedEpisode.startPoolOverwritePrevented;
    ingressDecimated = currentIngressDecimated - wgcStarvedEpisode.startIngressDecimated;
}
const bool wgcFramepoolPressure = poolSaturatedDrops > 0 || ingressDecimated > 0;
const bool wgcDeliveryGap = deliveredBelowCfrTarget || callbackDeliveryGap;
const char* faultHint = capacityPressure               ? "ce_capacity_pressure"
                        : wgcFramepoolPressure          ? "wgc_framepool_pressure"
                        : sourceBelowCfrTarget          ? "source_below_cfr_target"
                        : wgcDeliveryGap                ? "wgc_delivery_gap"
                        : poolOverwritePrevented > 0    ? "wgc_pool_lease_contention"
                                                       : "source_starved";
const bool copySlow = wgcStarvedEpisode.maxCopyUs > frameBudgetUs;
const bool fenceSlow = wgcStarvedEpisode.maxFenceUs > frameBudgetUs;
if (durationMs >= captureSessionSummary.longestStarvedEpisodeMs) {
    captureSessionSummary.longestStarvedEpisodeMs = durationMs;
    captureSessionSummary.longestStarvedEpisodeOutputTicks = outputTicks;
    captureSessionSummary.longestStarvedEpisodeDuplicateTicks = duplicateTicks;
    captureSessionSummary.longestStarvedEpisodeMinInputFps = minInputFps;
    captureSessionSummary.longestStarvedEpisodeMinDeliveredFps = minDeliveredFps;
}
if (shouldLogWgcStarvedEpisode(durationMs, outputTicks, duplicateTicks,
                               wgcStarvedEpisode.peakFreshMissPermille)) {
    LogInfo(
        "[WGC CFR] Source-starved episode: duration=%llums out=%llu dup=%llu minIn=%u minDel=%u "
        "freshMiss=%upm minBuf=%u",
        static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
        static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
        wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames);
    LogInfo(
        "[WGC CFR ATTRIBUTION] fault_hint=%s qpc=%lld..%lld duration=%llums out=%llu dup=%llu "
        "minIn=%u minDel=%u freshMiss=%upm minBuf=%u cbGapMax=%uus encEmaMax=%.2fms "
        "muxBp=%u waitMax=%uus muxMax=%uKB overload=0x%X copyMax=%uus copyHealth=%s "
        "fenceMax=%uus fenceHealth=%s poolSat=%u overwritePrevented=%u ingressDecimated=%u",
        faultHint, static_cast<long long>(wgcStarvedEpisode.startQpc), static_cast<long long>(endQpc.QuadPart),
        static_cast<unsigned long long>(durationMs), static_cast<unsigned long long>(outputTicks),
        static_cast<unsigned long long>(duplicateTicks), minInputFps, minDeliveredFps,
        wgcStarvedEpisode.peakFreshMissPermille, minBufferedFrames, wgcStarvedEpisode.maxCallbackGapUs,
        wgcStarvedEpisode.maxEncodeEmaMs, wgcStarvedEpisode.maxMuxBackpressureCount,
        wgcStarvedEpisode.maxMuxBackpressureWaitUs, wgcStarvedEpisode.maxMuxQueueKb,
        wgcStarvedEpisode.peakOverloadFlags, wgcStarvedEpisode.maxCopyUs, copySlow ? "slow" : "ok",
        wgcStarvedEpisode.maxFenceUs, fenceSlow ? "slow" : "ok", poolSaturatedDrops, poolOverwritePrevented,
        ingressDecimated);
}
wgcStarvedEpisode.Reset();

}

void MediaEncoderSession::ClearBufferedInjectFrames() {

while (!bufferedInjectFrames.empty()) {
    QueuedFrame queuedFrame = std::move(bufferedInjectFrames.front());
    bufferedInjectFrames.pop_front();
    DiscardQueuedFrame(queuedFrame);
}

}

void MediaEncoderSession::ClearBufferedWgcFrames() {

while (!bufferedWgcFrames.empty()) {
    QueuedFrame queuedFrame = std::move(bufferedWgcFrames.front());
    bufferedWgcFrames.pop_front();
    ReleaseQueuedFrameTexture(queuedFrame);
}

}

void MediaEncoderSession::ResetWarmupWgcFreshness(bool resetStartupDiagnostics) {

lastWarmupWgcSourceQpc = 0;
wgcStartupBarrierQpc = 0;
wgcStartupBarrierDroppedFrames = 0;
wgcStartupPreLiveDelayComplete = false;
wgcStartupPreLiveDelayDroppedFrames = 0;
wgcStartupReserveWaitStartQpc = 0;
wgcStartupReserveWaitCount = 0;
wgcStartupHistoryProtectionLogged = false;
wgcStartupReserveWaitInitialSpanUs = 0;
wgcStartupReserveWaitFreshenedMax = 0;
wgcFreshWarmupFrameCount = 0;
if (resetStartupDiagnostics) {
    wgcAvSyncScheduleOffsetQpc = 0;
    wgcAvSyncStartupAudioAnchorQpc = 0;
    wgcAvSyncStartupVideoQpc = 0;
    wgcAvSyncStartupEffectiveDelayQpc = 0;
    wgcSmoothnessActiveDelayQpc = 0;
    pendingWgcStartContract = {};
    pendingWgcStartContractGeneration = 0;
    committedWgcStartContractGeneration = 0;
    wgcEncoderPrewarmAttempted = false;
    wgcEncoderPrewarmSucceeded = false;
    wgcEncoderPrewarmElapsedUs = 0;
    wgcSmoothnessFloorDelayQpc = 0;
    wgcSmoothnessFloorRequestedQpc = 0;
    wgcSmoothnessFloorSource = "off";
    wgcSmoothnessFloorClampedBy = "none";
    wgcSmoothnessFloorLogged = false;
    wgcSmoothnessFloorJitter = ce::capture_policy::WgcSmoothnessFloorJitter{};
    wgcSmoothnessDesiredFrames = 0;
    wgcSmoothnessRetainedFrames = 0;
    wgcSmoothnessActualFrames = 0;
    wgcSmoothnessPoolSlots = 0;
    wgcSmoothnessRetainedFrameCap = 0;
    wgcSmoothnessReservedFreeSlots = 0;
    wgcSmoothnessEstimatedVramBytes = 0;
    wgcSmoothnessCapLimited = false;
    wgcSmoothnessBufferReason = "not-run";
}

}

void MediaEncoderSession::TrackWarmupWgcFreshFrame(const QueuedFrame& queuedFrame) {

if (queuedFrame.isInjectMode || queuedFrame.timestamp <= 0) {
    return;
}
if (queuedFrame.timestamp > lastWarmupWgcSourceQpc) {
    lastWarmupWgcSourceQpc = queuedFrame.timestamp;
    ++wgcFreshWarmupFrameCount;
}

}

uint32_t MediaEncoderSession::updateLiveCfrShortfall(int64_t nowQpc) {

if (config.video.useVFR || !recordingOutputLive || liveStartQpc.QuadPart <= 0 || targetIntervalTicks <= 0 ||
    nowQpc <= liveStartQpc.QuadPart) {
    liveTicksScheduled = 0;
    return 0u;
}
int64_t scheduledUntilQpc = nowQpc;
if (!media_main_g_Recording.load(std::memory_order_acquire)) {
    const int64_t drainStopQpc = media_main_g_CfrDrainStopQpc.load(std::memory_order_acquire);
    if (drainStopQpc > liveStartQpc.QuadPart) {
        scheduledUntilQpc = drainStopQpc;
    }
}
const uint64_t elapsedTicks = static_cast<uint64_t>(scheduledUntilQpc - liveStartQpc.QuadPart) /
                              static_cast<uint64_t>(targetIntervalTicks);
liveTicksScheduled = ce::capture_policy::GetCfrScheduledTicksForEndpoint(
    elapsedTicks, liveTicksDiscardedByTimerRebase, wgcVisualDebtMaxExcessTicks);
return ce::capture_policy::GetCfrOutputShortfallTicks(liveTicksScheduled, liveTicksOutput);

}

int64_t MediaEncoderSession::qpcToUs(int64_t qpcDelta) {

return qpcFreq.QuadPart > 0 ? (qpcDelta * 1000000) / qpcFreq.QuadPart : 0;

}

void MediaEncoderSession::observeCaptureSyncPhaseSource(const char* path, ce::capture_policy::CfrCadencePhaseLockState& state, int64_t sourceTimestampQpc) {

if (!captureSyncPhaseLockEnabled) {
    return;
}
const uint64_t releasesBefore = state.releases;
ce::capture_policy::ObserveCfrCaptureSyncSourceTimestamp(state, sourceTimestampQpc,
                                                         captureSyncSourceIntervalTicks);
if (state.releases != releasesBefore) {
    LogInfo(
        "[CFR PhaseLock] path=%s state=released reason=variable_source stable=%u unstable=%u "
        "multiplier=%u releases=%llu",
        path, state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
        static_cast<unsigned long long>(state.releases));
}

}

int64_t MediaEncoderSession::applyCaptureSyncPhaseTarget(const char* path, ce::capture_policy::CfrCadencePhaseLockState& state, int64_t baseTargetQpc, int64_t sourceReferenceQpc) {

const uint64_t acquisitionsBefore = state.acquisitions;
const uint64_t releasesBefore = state.releases;
const uint64_t rephasesBefore = state.rephases;
const int64_t adjustedTargetQpc = ce::capture_policy::ApplyCfrCaptureSyncPhaseLock(
    state, baseTargetQpc, sourceReferenceQpc, captureSyncSourceIntervalTicks,
    captureSyncPhaseLockEnabled);
if (state.acquisitions != acquisitionsBefore || state.releases != releasesBefore ||
    state.rephases != rephasesBefore) {
    const char* transition = state.acquisitions != acquisitionsBefore
                                 ? "acquired"
                                 : (state.rephases != rephasesBefore ? "rephased" : "released");
    LogInfo(
        "[CFR PhaseLock] path=%s state=%s offset=%lldus stable=%u unstable=%u multiplier=%u "
        "transitions=%llu/%llu/%llu",
        path, transition, static_cast<long long>(qpcToUs(state.lockedPhaseQpc)),
        state.stableSourceIntervals, state.unstableSourceIntervals, captureSyncMultiplier,
        static_cast<unsigned long long>(state.acquisitions),
        static_cast<unsigned long long>(state.rephases), static_cast<unsigned long long>(state.releases));
}
return adjustedTargetQpc;

}

int64_t MediaEncoderSession::getWgcRawSelectionTimestamp(const QueuedFrame& frame) {

return frame.rawTimestamp > 0 ? frame.rawTimestamp : frame.timestamp;

}

int64_t MediaEncoderSession::getWgcEffectiveContentDelayQpc() {

return avContentDelayQpc + std::max<int64_t>(0, wgcSmoothnessActiveDelayQpc);

}

bool MediaEncoderSession::isWgcEffectiveContentDelayActive() {
return getWgcEffectiveContentDelayQpc() > 0; 
}

uint32_t MediaEncoderSession::getWgcSmoothnessOutputFps() {

return config.video.fps > 0 ? static_cast<uint32_t>(config.video.fps) : 0u;

}

bool MediaEncoderSession::shouldUseWgcSmoothnessBaseConfig() {

// Pass wgcSmoothnessDelayDesired (audio-latency delay OR configured floor) so the buffer
// arms for video-only / low-confidence captures too, not only when audio latency is present.
return ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                        wgcSmoothnessDelayDesired, targetIntervalTicks);

}

uint32_t MediaEncoderSession::getWgcSmoothnessDesiredFramesForConfig() {

if (!shouldUseWgcSmoothnessBaseConfig()) {
    return 0u;
}
return ce::capture_policy::GetWgcSmoothnessDesiredFrames(getWgcSmoothnessOutputFps(),
                                                         config.wgcSmoothnessBufferMaxMs);

}

uint32_t MediaEncoderSession::getWgcSmoothnessRetainedFramesBudget() {

const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
return (media_main_g_WgcCap && desiredFrames > 0) ? media_main_g_WgcCap->GetSmoothnessRetainedFrameCount() : 0u;

}

bool MediaEncoderSession::isWgcSmoothnessSourceRateEligibleNow() {

if (!ce::capture_policy::ShouldUseWgcSmoothnessBuffer(config.wgcSmoothnessBufferEnabled, config.video.useVFR,
                                                      wgcSmoothnessDelayDesired, targetIntervalTicks)) {
    return false;
}
const uint32_t inputMin250Fps = media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin250Fps() : wgcRecentInputMin250Fps;
const uint32_t inputMin500Fps = media_main_g_WgcCap ? media_main_g_WgcCap->GetInputMin500Fps() : wgcRecentInputMin500Fps;
return ce::capture_policy::ShouldArmWgcSmoothnessBufferForSourceRate(getWgcSmoothnessOutputFps(),
                                                                     inputMin250Fps, inputMin500Fps);

}

bool MediaEncoderSession::shouldAttemptWgcStartupSmoothnessBufferNow() {

return ce::capture_policy::ShouldAttemptWgcStartupSmoothnessBuffer(
    config.wgcSmoothnessBufferEnabled, config.video.useVFR, wgcSmoothnessDelayDesired, targetIntervalTicks,
    getWgcSmoothnessRetainedFramesBudget());

}

int64_t MediaEncoderSession::getWgcStartupSmoothnessTargetDelayQpc(bool attempted) {

return attempted
           ? ce::capture_policy::GetWgcStartupSmoothnessTargetDelayQpc(
                 getWgcSmoothnessRetainedFramesBudget(), targetIntervalTicks, getWgcSmoothnessOutputFps(),
                 config.wgcSmoothnessBufferMaxMs)
           : 0;

}

const char* MediaEncoderSession::getWgcSmoothnessBufferReason() {

if (!config.wgcSmoothnessBufferEnabled) {
    return "disabled";
}
if (config.video.useVFR) {
    return "vfr";
}
if (!wgcSmoothnessDelayDesired) {
    return "sync_delay_inactive";
}
if (targetIntervalTicks <= 0) {
    return "invalid_target";
}
const uint32_t desiredFrames = getWgcSmoothnessDesiredFramesForConfig();
if (desiredFrames == 0) {
    return "target_zero";
}
const uint32_t retainedFrames = getWgcSmoothnessRetainedFramesBudget();
if (retainedFrames == 0) {
    return "vram_budget_exhausted";
}
if (!isWgcSmoothnessSourceRateEligibleNow()) {
    return "startup_attempt_source_rate_low";
}
return "startup_attempt";

}

uint32_t MediaEncoderSession::getWgcDelayReservoirLowWaterFramesForDelay(int64_t delayQpc) {

return ce::capture_policy::GetWgcDelayReservoirLowWaterFrames(delayQpc, targetIntervalTicks);

}

uint32_t MediaEncoderSession::getWgcDelayReservoirTargetFramesForDelay(int64_t delayQpc) {

return ce::capture_policy::GetWgcDelayReservoirTargetFrames(delayQpc, targetIntervalTicks);

}

uint32_t MediaEncoderSession::getWgcDelayReservoirLowWaterFrames() {

return getWgcDelayReservoirLowWaterFramesForDelay(getWgcEffectiveContentDelayQpc());

}

uint32_t MediaEncoderSession::getWgcDelayReservoirTargetFrames() {

return getWgcDelayReservoirTargetFramesForDelay(getWgcEffectiveContentDelayQpc());

}

uint32_t MediaEncoderSession::getWgcRetainedFrameCap() {

if (!media_main_g_WgcCap) {
    return 0u;
}
return media_main_g_WgcCap->GetSmoothnessRetainedFrameCap();

}

void MediaEncoderSession::updateWgcIngressPressure(const char*) {

if (!media_main_g_WgcCap) {
    return;
}
const uint32_t retainedCap = getWgcRetainedFrameCap();
const uint32_t retainedFrames = SaturatingToUint32(
    static_cast<uint64_t>(bufferedWgcFrames.size()) +
    static_cast<uint64_t>(std::min<size_t>(media_main_g_FrameQueue.Size(), static_cast<size_t>(UINT32_MAX))));
const uint32_t lowWaterFrames = getWgcDelayReservoirLowWaterFrames();
const bool delayReservoirActive = lowWaterFrames > 0;
const bool recovering = delayReservoirActive && (wgcLowSourceModeActive || wgcLiveRecoveryModeActive ||
                                                 (wgcActiveDelaySourceRecoveryUntilTick > GetTickCount64()) ||
                                                 retainedFrames <= lowWaterFrames);
const bool uniformPlayoutOwnsSurplus =
    isWgcEffectiveContentDelayActive() && config.wgcActiveDelayUniformCadence;
media_main_g_WgcCap->SetRetainedFramePressure(retainedFrames, retainedCap, lowWaterFrames, recovering,
                                   uniformPlayoutOwnsSurplus);

}

uint32_t MediaEncoderSession::trimBufferedWgcToRetainedCap(const char* reason) {

const uint32_t retainedCap = getWgcRetainedFrameCap();
if (retainedCap == 0) {
    updateWgcIngressPressure(reason);
    return 0u;
}
uint32_t trimmed = 0;
while (bufferedWgcFrames.size() > retainedCap) {
    QueuedFrame surplus = std::move(bufferedWgcFrames.back());
    bufferedWgcFrames.pop_back();
    ReleaseQueuedFrameTexture(surplus);
    ++trimmed;
}
if (trimmed > 0) {
    wgcRetainedCapTrimTotal += trimmed;
    wgcRetainedCapTrimWindow += trimmed;
    const DWORD nowTick = GetTickCount();
    if (trimmed >= 3 || nowTick - wgcRetainedCapTrimLastLogTick >= 1000) {
        LogInfo(
            "[WGC CFR] retained reservoir capped: trimmedNewest=%u reason=%s retained=%zu cap=%u "
            "reservedFree=%u poolSlots=%u (pool safety protected; surplus source frames become planned CFR "
            "decimation/repeats, audio/PTS unchanged)",
            trimmed, reason ? reason : "unknown", bufferedWgcFrames.size(), retainedCap,
            media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
            media_main_g_WgcCap ? media_main_g_WgcCap->GetTexturePoolSlotCount() : 0u);
        wgcRetainedCapTrimLastLogTick = nowTick;
    }
}
updateWgcIngressPressure(reason);
return trimmed;

}

uint32_t MediaEncoderSession::trimBufferedWgcStartupWaitToRetainedCap(const char* reason) {

const uint32_t retainedCap = getWgcRetainedFrameCap();
if (retainedCap == 0) {
    updateWgcIngressPressure(reason);
    return 0u;
}
uint32_t trimmed = 0;
while (bufferedWgcFrames.size() > retainedCap) {
    QueuedFrame surplus = std::move(bufferedWgcFrames.front());
    bufferedWgcFrames.pop_front();
    ReleaseQueuedFrameTexture(surplus);
    ++trimmed;
}
if (trimmed > 0) {
    wgcRetainedCapTrimTotal += trimmed;
    wgcRetainedCapTrimWindow += trimmed;
    LogInfo(
        "[WGC CFR] startup wait retained reservoir capped: trimmedOldest=%u reason=%s retained=%zu cap=%u "
        "reservedFree=%u poolSlots=%u",
        trimmed, reason ? reason : "startup-wait", bufferedWgcFrames.size(), retainedCap,
        media_main_g_WgcCap ? media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount() : 0u,
        media_main_g_WgcCap ? media_main_g_WgcCap->GetTexturePoolSlotCount() : 0u);
}
updateWgcIngressPressure(reason);
return trimmed;

}

uint32_t MediaEncoderSession::trimBufferedWgcForPoolPressure(const char* reason) {

if (!media_main_g_WgcCap) {
    updateWgcIngressPressure(reason);
    return 0u;
}
const uint32_t retainedCap = getWgcRetainedFrameCap();
const uint32_t reservedFreeSlots = media_main_g_WgcCap->GetSmoothnessReservedFreeSlotCount();
const uint32_t currentFreeSlots = media_main_g_WgcCap->GetPoolSlotFreeCurrentCount();
const uint32_t trimTarget = ce::capture_policy::GetWgcPoolPressureRetainedTrimTarget(
    currentFreeSlots, reservedFreeSlots, getWgcDelayReservoirTargetFrames(), retainedCap);
if (trimTarget == 0 || trimTarget >= retainedCap || bufferedWgcFrames.size() <= trimTarget) {
    updateWgcIngressPressure(reason);
    return 0u;
}

uint32_t trimmed = 0;
while (bufferedWgcFrames.size() > trimTarget) {
    QueuedFrame surplus = std::move(bufferedWgcFrames.back());
    bufferedWgcFrames.pop_back();
    ReleaseQueuedFrameTexture(surplus);
    ++trimmed;
}
if (trimmed > 0) {
    wgcRetainedCapTrimTotal += trimmed;
    wgcRetainedCapTrimWindow += trimmed;
    wgcPoolPressureTrimTotal += trimmed;
    wgcPoolPressureTrimWindow += trimmed;
    const DWORD nowTick = GetTickCount();
    if (trimmed >= 2 || nowTick - wgcPoolPressureTrimLastLogTick >= 1000) {
        LogInfo(
            "[WGC CFR] retained reservoir pressure trim: trimmedNewest=%u reason=%s retained=%zu "
            "target=%u cap=%u free=%u reservedFree=%u poolSlots=%u delayTarget=%u "
            "(preserved active-delay target; released surplus copy-pool leases, audio/PTS unchanged)",
            trimmed, reason ? reason : "pool-pressure", bufferedWgcFrames.size(), trimTarget, retainedCap,
            currentFreeSlots, reservedFreeSlots, media_main_g_WgcCap->GetTexturePoolSlotCount(),
            getWgcDelayReservoirTargetFrames());
        wgcPoolPressureTrimLastLogTick = nowTick;
    }
}
updateWgcIngressPressure(reason);
return trimmed;

}

void MediaEncoderSession::recordWgcDelayResidualSample(int64_t signedResidualUs, uint64_t& samples, uint64_t& absAccumUs, int64_t& signedAccumUs, uint32_t& absMaxUs, uint32_t& lateMaxUs, uint32_t& earlyMaxUs, std::array<uint32_t, 256>& histogram, uint64_t& windowSamples, uint64_t& windowAbsAccumUs, int64_t& windowSignedAccumUs, uint32_t& windowAbsMaxUs, uint32_t& windowLateMaxUs, std::array<uint32_t, 256>& windowHistogram) {

const uint32_t absResidualUs =
    SaturatingToUint32(static_cast<uint64_t>(signedResidualUs >= 0 ? signedResidualUs : -signedResidualUs));
++samples;
absAccumUs += absResidualUs;
signedAccumUs += signedResidualUs;
absMaxUs = std::max(absMaxUs, absResidualUs);
if (signedResidualUs >= 0) {
    lateMaxUs = std::max(lateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
} else {
    earlyMaxUs = std::max(earlyMaxUs, SaturatingToUint32(static_cast<uint64_t>(-signedResidualUs)));
}
const size_t histogramBin = std::min<size_t>(histogram.size() - 1, absResidualUs / 1000u);
++histogram[histogramBin];
++windowSamples;
windowAbsAccumUs += absResidualUs;
windowSignedAccumUs += signedResidualUs;
windowAbsMaxUs = std::max(windowAbsMaxUs, absResidualUs);
if (signedResidualUs >= 0) {
    windowLateMaxUs =
        std::max(windowLateMaxUs, SaturatingToUint32(static_cast<uint64_t>(signedResidualUs)));
}
++windowHistogram[histogramBin];

}

bool MediaEncoderSession::recordWgcDelayRealization(int64_t predictedSignedResidualUs, int64_t rawSignedResidualUs) {

if (!isWgcEffectiveContentDelayActive() || qpcFreq.QuadPart <= 0) {
    return false;
}
const int64_t requestedDelayUs = qpcToUs(getWgcEffectiveContentDelayQpc());
const int64_t realizedDelaySignedUs = requestedDelayUs - predictedSignedResidualUs;
const uint32_t realizedDelayUs =
    SaturatingToUint32(static_cast<uint64_t>(realizedDelaySignedUs > 0 ? realizedDelaySignedUs : 0));

wgcDelayRealizedAccumUs += realizedDelayUs;
wgcDelayRealizedMinUs = std::min(wgcDelayRealizedMinUs, realizedDelayUs);
wgcDelayRealizedMaxUs = std::max(wgcDelayRealizedMaxUs, realizedDelayUs);
recordWgcDelayResidualSample(predictedSignedResidualUs, wgcDelayResidualSamples, wgcDelayResidualAbsAccumUs,
                             wgcDelayResidualSignedAccumUs, wgcDelayResidualAbsMaxUs, wgcDelayResidualLateMaxUs,
                             wgcDelayResidualEarlyMaxUs, wgcDelayResidualAbsHistogram,
                             wgcDelayResidualWindowSamples, wgcDelayResidualWindowAbsAccumUs,
                             wgcDelayResidualWindowSignedAccumUs, wgcDelayResidualWindowAbsMaxUs,
                             wgcDelayResidualWindowLateMaxUs, wgcDelayResidualWindowAbsHistogram);
recordWgcDelayResidualSample(rawSignedResidualUs, wgcDelayRawResidualSamples, wgcDelayRawResidualAbsAccumUs,
                             wgcDelayRawResidualSignedAccumUs, wgcDelayRawResidualAbsMaxUs,
                             wgcDelayRawResidualLateMaxUs, wgcDelayRawResidualEarlyMaxUs,
                             wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualWindowSamples,
                             wgcDelayRawResidualWindowAbsAccumUs, wgcDelayRawResidualWindowSignedAccumUs,
                             wgcDelayRawResidualWindowAbsMaxUs, wgcDelayRawResidualWindowLateMaxUs,
                             wgcDelayRawResidualWindowAbsHistogram);
const int64_t rawMinusPredictedUs = rawSignedResidualUs - predictedSignedResidualUs;
const uint32_t rawMinusPredictedAbsUs = SaturatingToUint32(
    static_cast<uint64_t>(rawMinusPredictedUs >= 0 ? rawMinusPredictedUs : -rawMinusPredictedUs));
++wgcDelayRawMinusPredictedSamples;
wgcDelayRawMinusPredictedSignedAccumUs += rawMinusPredictedUs;
wgcDelayRawMinusPredictedAbsMaxUs = std::max(wgcDelayRawMinusPredictedAbsMaxUs, rawMinusPredictedAbsUs);
++wgcDelayRawMinusPredictedWindowSamples;
wgcDelayRawMinusPredictedWindowSignedAccumUs += rawMinusPredictedUs;
wgcDelayRawMinusPredictedWindowAbsMaxUs =
    std::max(wgcDelayRawMinusPredictedWindowAbsMaxUs, rawMinusPredictedAbsUs);
return true;

}

uint32_t MediaEncoderSession::wgcDelayResidualHistogramP95Us(const std::array<uint32_t, 256>& histogram, uint64_t samples) {

if (samples == 0) {
    return 0;
}
const uint64_t targetRank = (samples * 95ull + 99ull) / 100ull;
uint64_t cumulative = 0;
for (size_t i = 0; i < histogram.size(); ++i) {
    cumulative += histogram[i];
    if (cumulative >= targetRank) {
        return SaturatingToUint32(static_cast<uint64_t>(i) * 1000ull);
    }
}
return SaturatingToUint32(static_cast<uint64_t>(histogram.size() - 1) * 1000ull);

}

uint32_t MediaEncoderSession::wgcDelayResidualP95Us() {

return wgcDelayResidualHistogramP95Us(wgcDelayResidualAbsHistogram, wgcDelayResidualSamples);

}

uint32_t MediaEncoderSession::wgcDelayResidualWindowP95Us() {

return wgcDelayResidualHistogramP95Us(wgcDelayResidualWindowAbsHistogram, wgcDelayResidualWindowSamples);

}

uint32_t MediaEncoderSession::wgcDelayRawResidualP95Us() {

return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualAbsHistogram, wgcDelayRawResidualSamples);

}

uint32_t MediaEncoderSession::wgcDelayRawResidualWindowP95Us() {

return wgcDelayResidualHistogramP95Us(wgcDelayRawResidualWindowAbsHistogram, wgcDelayRawResidualWindowSamples);

}
