#include "media_main_internal.h"
#include "media_main_encoder_session.h"
#include "common/capture/time_grid.h"

void MediaEncoderSession::LoopWgcTarget() {
        scheduledOutputQpc = scheduledSampleQpc;
        if (!config.video.useVFR && recordingOutputLive && activeScreenGrab) {
            // Wake deadlines may rebase after expensive work, but source selection,
            // cursor sampling, and submission stay on the immutable CFR grid. Extra
            // held slots repay debt without duplicate QPC or postponing the next wake.
            scheduledOutputQpc = ce::capture_policy::GetNextCfrOutputQpc(
                liveStartQpc.QuadPart, liveTicksOutput, qpcFreq.QuadPart, config.video.fps, scheduledSampleQpc);
        }

        popped = false;
        wgcTelemetryTickArmed = false;
        wgcBufferedAtTickStart = 0;
        wgcFreshAvailableAtTickStart = false;
        wgcReserveAvailableAtTickStart = false;
        wgcSelectionDelayAppliedThisTick = false;
        wgcProactiveOverloadRepeatThisTick = false;
        injectProactiveOverloadRepeatThisTick = false;
        wgcDelayRealizationRecordedThisTick = false;

}

void MediaEncoderSession::inspectBufferedWgcCoverageForTarget(int64_t targetQpc, bool activeDelaySelection, uint32_t requiredReserveFrames, bool* hasFrameForTick, bool* hasReserveFrame) {

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
if (targetQpc > 0) {
    while ((idx + 1) < bufferedWgcFrames.size()) {
        const QueuedFrame& current = bufferedWgcFrames[idx];
        const QueuedFrame& next = bufferedWgcFrames[idx + 1];
        const bool sameTimestamp = current.timestamp > 0 && current.timestamp == next.timestamp;
        const bool nextAlreadyCoversTarget = next.timestamp > 0 && next.timestamp <= targetQpc;
        if (!sameTimestamp && !nextAlreadyCoversTarget) {
            break;
        }
        ++idx;
    }
}

if (idx >= bufferedWgcFrames.size()) {
    return;
}

const QueuedFrame& coverageCandidateFrame = bufferedWgcFrames[idx];
const int64_t coverageTimestamp = GetFrameSelectionTimestamp(coverageCandidateFrame);
const bool canUseCandidateNow =
    targetQpc <= 0 || coverageTimestamp <= 0 ||
    !(activeDelaySelection ? ce::capture_policy::IsWgcFrameTooNewForActiveDelaySlot(
                                 coverageTimestamp, targetQpc, targetIntervalTicks)
                           : ce::capture_policy::IsWgcFrameTooNewForCfrSlot(
                                 coverageTimestamp, targetQpc, targetIntervalTicks)) ||
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
    ComputeIdealOutputQpcOnRationalGrid(encoderGridStartQpc, selectionGridTickForTick, qpcFreq.QuadPart,
                                        config.video.fps);
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

int64_t MediaEncoderSession::clampWgcSelectionTargetQpc(int64_t targetQpc, int64_t observedNowQpc) {

const bool encoderBottlenecked = media_main_g_IsEncoderBottlenecked.load(std::memory_order_relaxed);
const int64_t clampedSelectionTargetQpc = ce::capture_policy::ClampWgcSelectionTargetToLiveQpc(
    targetQpc, observedNowQpc, targetIntervalTicks, qpcFreq.QuadPart, wgcLowSourceModeActive,
    wgcLiveRecoveryModeActive, outputShortfallTicks, encoderBottlenecked,
    ce::capture_policy::kCfrShortfallCatchupThresholdTicks, isWgcEncoderLimitedSmoothnessMode(),
    getWgcEffectiveContentDelayQpc());
if (clampedSelectionTargetQpc > targetQpc) {
    const uint64_t clampDeltaUs = static_cast<uint64_t>(clampedSelectionTargetQpc - targetQpc) *
                                  1000000ull / static_cast<uint64_t>(qpcFreq.QuadPart);
    ++wgcSelectionTargetClampCount;
    wgcSelectionTargetClampMaxUs = std::max(wgcSelectionTargetClampMaxUs, SaturatingToUint32(clampDeltaUs));
}
return clampedSelectionTargetQpc;

}

int64_t MediaEncoderSession::computeLiveTimelineElapsedUs(int64_t scheduledQpcForTick) {

return ce::time::ScheduledElapsed(ce::time::QpcTicks{liveStartQpc.QuadPart}, ce::time::QpcTicks{scheduledQpcForTick},
                                  ce::time::QpcFrequency{qpcFreq.QuadPart}).count();

}
