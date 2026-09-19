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
    // The reservation above the ceiling is the timer margin plus whatever the
    // overrun controller has learned this game needs.
    frameWorkBudgetUs_ = ce::fps_limiter_policy::ResolveFrameWorkBudgetUs(
        ceilingUs, adaptiveFineMarginUs_ + frontLoadHeadroomUs_ + frontLoadGpuHeadroomUs_, intervalUs,
        frameWorkSampleCount_, kFrameWorkMinimumSamples);
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
    const int64_t frameWorkOverrideUs = frameWorkOverrideUs_.load(std::memory_order_relaxed);
    if (frameWorkOverrideUs > 0 && cadenceIntervalUs_ > 0 && !cadenceFirstFrame) {
        // A stated frame work, which only a test sets - see
        // SetObservedFrameWorkOverrideUs. It deliberately does not require
        // lastApplyReturnQpc, because the point is not to consult the clock.
        RecordFrameWork(frameWorkOverrideUs, cadenceIntervalUs_);
    } else if (lastApplyReturnQpc != 0 && cadenceIntervalUs_ > 0 && !cadenceFirstFrame && qpcFrequency > 0) {
        RecordFrameWork(((nowQpcTicks - lastApplyReturnQpc) * 1000000) / qpcFrequency, cadenceIntervalUs_);
    }
    // An armed release the call site never ran belongs to the present that just
    // happened, not to this one.
    timerPostPresentPending_ = false;
}

// Hand the next deadline to ApplyPostPresent so the game is released just in
// time to build the frame that deadline presents, instead of finishing it
// immediately and ageing in the present hook.
// A present that missed its deadline while the previous frame was released on
// a budget is the only evidence that the budget was too small. Anything else -
// a hitch, a back-edge frame, a frame the release never ran for - says nothing
// about it and must not move the reservation.
inline void FpsLimiter::NoteFrontLoadedLateness(int64_t lateUs) {
    if (!frontLoadedReleaseRan_) {
        return;
    }
    frontLoadedReleaseRan_ = false;
    const int64_t grown =
        ce::fps_limiter_policy::GrowFrontLoadHeadroomUs(frontLoadHeadroomUs_, lateUs, cadenceIntervalUs_);
    if (grown != frontLoadHeadroomUs_) {
        frontLoadHeadroomUs_ = grown;
        frontLoadCleanFrames_ = 0;
        ++frontLoadOverrunCount_;
        TraceLog("Apply: LOCAL front-load overrun lateUs=%lld headroomUs=%lld ceilingUs=%lld overruns=%u", lateUs,
                 frontLoadHeadroomUs_, observedFrameWorkCeilingUs_, frontLoadOverrunCount_);
        return;
    }
    if (lateUs > 0) {
        // A hitch: neither evidence for nor against the reservation.
        return;
    }
    if (++frontLoadCleanFrames_ >= frameWorkUs_.size()) {
        frontLoadCleanFrames_ = 0;
        frontLoadHeadroomUs_ = ce::fps_limiter_policy::DecayFrontLoadHeadroomUs(frontLoadHeadroomUs_);
    }
}

inline FpsLimiter::PresentToDisplaySnapshot FpsLimiter::SnapshotPresentToDisplay() const {
    PresentToDisplaySnapshot snapshot;
    std::array<int64_t, 64> samples{};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(presentToDisplayMutex_);
        count = presentToDisplaySampleCount_;
        samples = presentToDisplayUs_;
        snapshot.floorUs = presentToDisplayFloorUs_;
        snapshot.floorSeeded = presentToDisplayFloorSeeded_;
    }
    snapshot.samples = count;
    if (count == 0) {
        return snapshot;
    }
    // Median, so one vertical-blank hiccup cannot move the reservation.
    std::sort(samples.begin(), samples.begin() + count);
    snapshot.recentUs = samples[count / 2];
    return snapshot;
}

// Walk the reservation to the point where the frame's GPU work finishes by the
// deadline. Growth is immediate and by the whole measured excess, because every
// microsecond the GPU runs past the deadline is a microsecond of the game's own
// variance landing on the screen timeline; the walk back in is a bounded probe.
inline void FpsLimiter::UpdateFrontLoadGpuHeadroom() {
    if (++frontLoadWindowFrames_ < kFrontLoadGpuWindowFrames) {
        return;
    }
    frontLoadWindowFrames_ = 0;

    const PresentToDisplaySnapshot p2d = SnapshotPresentToDisplay();
    if (!p2d.floorSeeded || p2d.samples == 0) {
        return;
    }
    const int64_t excessUs =
        ce::fps_limiter_policy::ResolveFrontLoadGpuExcessUs(p2d.recentUs, p2d.floorUs, adaptiveFineMarginUs_);
    if (excessUs > 0) {
        const int64_t grown = frontLoadGpuHeadroomUs_ + excessUs;
        frontLoadGpuHeadroomUs_ = cadenceIntervalUs_ > 0 && grown > cadenceIntervalUs_ ? cadenceIntervalUs_ : grown;
        TraceLog(
            "Apply: LOCAL front-load gpu reservation grew excessUs=%lld p2dUs=%lld floorUs=%lld gpuHeadroomUs=%lld",
            excessUs, p2d.recentUs, p2d.floorUs, frontLoadGpuHeadroomUs_);
        return;
    }
    frontLoadGpuHeadroomUs_ =
        ce::fps_limiter_policy::DecayFrontLoadGpuHeadroomUs(frontLoadGpuHeadroomUs_, adaptiveFineMarginUs_);
}

inline void FpsLimiter::ArmFrontLoadedRelease(bool eligible, int effectiveTargetFps) {
    // Without displayed-transition evidence there is no way to tell whether
    // releasing the game later pushes its GPU work past the deadline, and a
    // budget below the frame's whole CPU+GPU time buys no latency at all - so
    // the back edge stays the default rather than a guess.
    const PresentToDisplaySnapshot p2d = SnapshotPresentToDisplay();
    const bool gpuEvidenceUsable = ce::fps_limiter_policy::HasUsableGpuCompletionEvidence(
        p2d.samples, kPresentToDisplayMinimumSamples, p2d.floorSeeded);
    if (!eligible || !gpuEvidenceUsable || localTargetTime_ == 0 || qpcFrequency <= 0 ||
        frameWorkBudgetUs_ <= 0 || frameWorkBudgetUs_ >= cadenceIntervalUs_) {
        if (!gpuEvidenceUsable && eligible && !frontLoadedPacingLogged_ && !loggedMissingGpuEvidence_) {
            HookLog(
                "FPS Limiter: holding the back-edge cadence placement - no displayed-transition evidence yet "
                "(present-to-display samples=%zu, floor=%s). Front-loading needs it to know the frame's GPU work "
                "still finishes by the deadline.",
                p2d.samples, p2d.floorSeeded ? "seeded" : "unseeded");
            loggedMissingGpuEvidence_ = true;
        }
        frontLoadedPlacementActive_.store(false, std::memory_order_release);
        return;
    }
    frontLoadedPlacementActive_.store(true, std::memory_order_release);
    timerPostPresentPending_ = true;
    timerPostPresentTargetTime_ = localTargetTime_ - ((frameWorkBudgetUs_ * qpcFrequency) / 1000000);
    frontLoadedReleaseRan_ = false;
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
    // The next Apply()'s lateness, if any, is attributable to this budget.
    frontLoadedReleaseRan_ = true;
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
    frontLoadHeadroomUs_ = 0;
    frontLoadGpuHeadroomUs_ = 0;
    frontLoadWindowFrames_ = 0;
    frontLoadCleanFrames_ = 0;
    frontLoadedReleaseRan_ = false;
    frontLoadedPacingLogged_ = false;
    loggedMissingGpuEvidence_ = false;
    frontLoadedPlacementActive_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(presentToDisplayMutex_);
        presentToDisplayCursor_ = 0;
        presentToDisplaySampleCount_ = 0;
        presentToDisplayFloorUs_ = -1;
        presentToDisplayFloorSeeded_ = false;
    }
}
