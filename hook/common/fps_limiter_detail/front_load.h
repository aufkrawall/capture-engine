#pragma once

// Front-loaded cadence placement: the half of FpsLimiter that decides how late
// in a period the game is released to build the frame the deadline presents.
// The deadline itself, the pre-present wait and the grid phase live in
// frame_pacing.h and are deliberately untouched here - everything in this unit
// is a latency control that degrades to the original back-edge placement.

#include "../fps_limiter.h"

inline void FpsLimiter::RecordFrameWork(int64_t workUs, int64_t intervalUs) {
    if (workUs < 0 || intervalUs <= 0 || workUs >= intervalUs) {
        // Outside the interval this is not frame work: the limiter was not the
        // thing that released the game, or the frame hitched. Feeding either in
        // would move the ceiling for reasons the budget cannot act on.
        return;
    }
    frameWorkUs_[frameWorkCursor_] = workUs;
    frameWorkCursor_ = (frameWorkCursor_ + 1) % frameWorkUs_.size();
    if (frameWorkSampleCount_ < frameWorkUs_.size()) {
        ++frameWorkSampleCount_;
    }

    int64_t ceilingUs = 0;
    for (size_t i = 0; i < frameWorkSampleCount_; ++i) {
        if (frameWorkUs_[i] > ceilingUs) {
            ceilingUs = frameWorkUs_[i];
        }
    }
    observedFrameWorkCeilingUs_ = ceilingUs;
    frameWorkBudgetUs_ = ce::fps_limiter_policy::ResolveFrameWorkBudgetUs(
        ceilingUs, adaptiveFineMarginUs_, intervalUs, frameWorkSampleCount_, kFrameWorkMinimumSamples);
}

inline int64_t FpsLimiter::CadenceIntervalTicks(int targetFps, int cadenceScale) const {
    if (qpcFrequency <= 0 || targetFps <= 0) {
        return 0;
    }
    const int64_t scale = cadenceScale > 0 ? cadenceScale : 1;
    if (qpcFrequency > INT64_MAX / scale) {
        return qpcFrequency / targetFps;
    }
    return (qpcFrequency * scale) / targetFps;
}

// Frame work for the release that has just ended, and the interval the budget
// is sized against. Called on the Apply() path with the cadence lock held,
// before the pre-present wait.
inline void FpsLimiter::NoteFrameWorkForFrontLoadedRelease(int64_t nowQpcTicks, int targetFps, int cadenceScale,
                                                           bool cadenceFirstFrame) {
    const int64_t intervalTicks = CadenceIntervalTicks(targetFps, cadenceScale);
    cadenceIntervalUs_ = intervalTicks > 0 && qpcFrequency > 0 ? (intervalTicks * 1000000) / qpcFrequency : 0;
    // How long the game took to build this frame after the limiter last
    // released it. Under the back-edge placement that release is the previous
    // Apply() return, under the front-loaded one it is the post-present
    // release; either way it is the span the budget has to cover.
    if (lastApplyReturnQpc != 0 && cadenceIntervalUs_ > 0 && !cadenceFirstFrame && qpcFrequency > 0) {
        RecordFrameWork(((nowQpcTicks - lastApplyReturnQpc) * 1000000) / qpcFrequency, cadenceIntervalUs_);
    }
    // An armed release the call site never ran belongs to the present that just
    // happened, not to this one.
    timerPostPresentPending_ = false;
}

// Hand the next deadline to ApplyPostPresent so the game is released just in
// time to build the frame that deadline presents, instead of finishing it
// immediately and ageing in the present hook.
inline void FpsLimiter::ArmFrontLoadedRelease(bool eligible, int effectiveTargetFps) {
    if (!eligible || localTargetTime_ == 0 || qpcFrequency <= 0 || frameWorkBudgetUs_ <= 0 ||
        frameWorkBudgetUs_ >= cadenceIntervalUs_) {
        return;
    }
    timerPostPresentPending_ = true;
    timerPostPresentTargetTime_ = localTargetTime_ - ((frameWorkBudgetUs_ * qpcFrequency) / 1000000);
    if (frontLoadedPacingLogged_) {
        return;
    }
    TraceLog(
        "Apply: LOCAL front-loaded release armed target=%d budgetUs=%lld workCeilingUs=%lld intervalUs=%lld "
        "samples=%zu",
        effectiveTargetFps, frameWorkBudgetUs_, observedFrameWorkCeilingUs_, cadenceIntervalUs_,
        frameWorkSampleCount_);
    HookLog(
        "FPS Limiter: front-loaded cadence release active (target=%d fps, budget=%lldus, measured frame work "
        "ceiling=%lldus, interval=%lldus) - the frame is built just before its deadline instead of ageing in "
        "the present hook",
        effectiveTargetFps, frameWorkBudgetUs_, observedFrameWorkCeilingUs_, cadenceIntervalUs_);
    frontLoadedPacingLogged_ = true;
}

// The release itself, run from ApplyPostPresent() with the cadence lock held.
// Returns true when it owned this post-present call.
inline bool FpsLimiter::RunFrontLoadedRelease() {
    if (!timerPostPresentPending_) {
        return false;
    }
    timerPostPresentPending_ = false;
    const int64_t releaseTarget = timerPostPresentTargetTime_;
    timerPostPresentTargetTime_ = 0;
    if (releaseTarget == 0) {
        return true;
    }
    LARGE_INTEGER releaseStart;
    LARGE_INTEGER releaseEnd;
    QueryPerformanceCounter(&releaseStart);
    SmartWait(releaseTarget);
    QueryPerformanceCounter(&releaseEnd);
    const int64_t releaseWaitUs =
        qpcFrequency > 0 ? ((releaseEnd.QuadPart - releaseStart.QuadPart) * 1000000) / qpcFrequency : 0;
    lastFrontLoadedReleaseWaitUs_ = releaseWaitUs;
    ++frontLoadedReleaseCount_;
    // The perf CSV's limiter column is the total time the limiter blocked this
    // frame's thread, which is now spent on both sides of Present.
    lastActualWaitUs_ += releaseWaitUs;
    lastApplyReturnQpc = releaseEnd.QuadPart;
    return true;
}

// Frame work measured under one configuration says nothing about the next one,
// and an armed release must never outlive the deadline it was sized against.
inline void FpsLimiter::ResetFrontLoadedPacingState() {
    timerPostPresentPending_ = false;
    timerPostPresentTargetTime_ = 0;
    frameWorkCursor_ = 0;
    frameWorkSampleCount_ = 0;
    observedFrameWorkCeilingUs_ = 0;
    frameWorkBudgetUs_ = 0;
    cadenceIntervalUs_ = 0;
    lastFrontLoadedReleaseWaitUs_ = 0;
    frontLoadedPacingLogged_ = false;
}
