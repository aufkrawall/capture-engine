#include "media_main_internal.h"
#include "media_main_encoder_session.h"

void MediaEncoderSession::LoopWgcTarget() {
        scheduledOutputQpc = scheduledSampleQpc;
        if (!config.video.useVFR && recordingOutputLive && activeScreenGrab) {
            // Wake deadlines may rebase after expensive work, but source selection,
            // cursor sampling, and submission stay on the immutable CFR grid. Extra
            // held slots repay debt without duplicate QPC or postponing the next wake.
            scheduledOutputQpc = ce::capture_policy::GetNextCfrOutputQpc(
                liveStartQpc.QuadPart, liveTicksOutput, targetIntervalTicks, scheduledSampleQpc);
        }

        popped = false;
        wgcTelemetryTickArmed = false;
        wgcBufferedAtTickStart = 0;
        wgcFreshAvailableAtTickStart = false;
        wgcReserveAvailableAtTickStart = false;
        wgcSelectionDelayAppliedThisTick = false;
        wgcProactiveOverloadRepeatThisTick = false;
        wgcDelayRealizationRecordedThisTick = false;

}

void MediaEncoderSession::inspectBufferedWgcCoverageForTarget(int64_t selectionTargetQpc, bool activeDelaySelection, uint32_t requiredReserveFrames, bool* hasFrameForTick, bool* hasReserveFrame) {

if (hasFrameForTick) {
    *hasFrameForTick = false;
}
if (hasReserveFrame) {
    *hasReserveFrame = false;
}
if (bufferedWgcFrames.empty()) {
    return;
}

size_t idx = 0;
if (selectionTargetQpc > 0) {
    while ((idx + 1) < bufferedWgcFrames.size()) {
        const QueuedFrame& current = bufferedWgcFrames[idx];
        const QueuedFrame& next = bufferedWgcFrames[idx + 1];
        const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
        const bool nextAlreadyCoversTarget = next.timestamp > 0 && next.timestamp <= selectionTargetQpc;
        if (!sameTimestamp && !nextAlreadyCoversTarget) {
            break;
        }
        ++idx;
    }
}

if (idx >= bufferedWgcFrames.size()) {
    return;
}

const QueuedFrame& candidate = bufferedWgcFrames[idx];
const int64_t candidateSelectionTimestamp = GetFrameSelectionTimestamp(candidate);
const bool canUseCandidateNow =
    selectionTargetQpc <= 0 || candidateSelectionTimestamp <= 0 ||
    !(activeDelaySelection ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                 candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)
                           : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                 candidateSelectionTimestamp, selectionTargetQpc, targetIntervalTicks)) ||
    !media_main_g_HasLastFrame || media_main_g_LastFrame.isInjectMode;
if (hasFrameForTick) {
    *hasFrameForTick = canUseCandidateNow;
}
if (hasReserveFrame) {
    const uint32_t reserveFrames = static_cast<uint32_t>(
        std::min<size_t>(bufferedWgcFrames.size() - idx, static_cast<size_t>(UINT32_MAX)));
    *hasReserveFrame = canUseCandidateNow && reserveFrames >= std::max<uint32_t>(1u, requiredReserveFrames);
}

}

int64_t MediaEncoderSession::computeWgcSelectionTargetForTick(int64_t scheduledQpcForTick, int64_t selectionGridTickForTick, bool applyLiveDelay) {

const int64_t fallbackTargetQpc =
    ComputeIdealOutputQpc(encoderGridStartQpc, selectionGridTickForTick, targetIntervalTicks);
// Uniform playout keeps its fixed delay through recovery; the legacy
// reservoir may yield it. Keep target and application on one helper.
const int64_t effectiveContentDelayQpc = getWgcEffectiveContentDelayQpc();
const bool uniformCadenceActiveDelay = effectiveContentDelayQpc > 0 && config.wgcActiveDelayUniformCadence;
return ce::capture_policy::GetWgcActiveDelaySelectionTargetQpc(
    scheduledQpcForTick, fallbackTargetQpc, targetIntervalTicks, recordingOutputLive, applyLiveDelay,
    wgcLiveRecoveryModeActive, uniformCadenceActiveDelay, effectiveContentDelayQpc);

}

int64_t MediaEncoderSession::computeWgcSelectionTargetQpc(bool applyLiveDelay) {

return computeWgcSelectionTargetForTick(scheduledOutputQpc, selectionGridTick, applyLiveDelay);

}

int64_t MediaEncoderSession::computeLiveWgcSelectionTargetQpc() {
return computeWgcSelectionTargetQpc(false); 
}

int64_t MediaEncoderSession::computeDelayedWgcSelectionTargetQpc() {
return computeWgcSelectionTargetQpc(true); 
}

int64_t MediaEncoderSession::clampWgcSelectionTargetQpc(int64_t selectionTargetQpc, int64_t liveNowQpc) {

const bool encoderBottlenecked = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
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

}

int64_t MediaEncoderSession::computeLiveTimelineElapsedUs(int64_t scheduledQpcForTick) {

if (liveStartQpc.QuadPart <= 0 || qpcFreq.QuadPart <= 0) {
    return -1;
}
const int64_t deltaQpc = scheduledQpcForTick - liveStartQpc.QuadPart;
if (deltaQpc < 0) {
    return -1;
}
return (deltaQpc * 1000000) / qpcFreq.QuadPart;

}
