#pragma once

// The game thread's release under frame generation at the proxy Present site
// (PresentSite::kRuntimeInputPresent): it waits until the runtime presented the
// frame's generated outputs, bounded by the frame's work budget before its next
// deadline. See ce::runtime_input_site_policy::ShouldGateReleaseOnGeneratedOutputs.
// Included by fps_limiter.h after the class body.

#include "hook/pacing/fps_limiter.h"

// A runtime output reached the screen path (its DXGI Present returned). Called
// from ApplyPostPresent() on the thread whose Apply() passed; never takes the
// cadence lock, so it cannot wait behind the game thread's release.
inline void FpsLimiter::NoteRuntimeOutputPresented() {
    if (!ce::runtime_input_site_policy::RuntimeInputOwnsPacing(
            outputsSinceRuntimeInput_.load(std::memory_order_acquire))) {
        return;
    }
    runtimeOutputsPresented_.fetch_add(1, std::memory_order_acq_rel);
    if (HANDLE event = runtimeOutputEvent_.load(std::memory_order_acquire)) {
        SetEvent(event);
    }
}

// Apply() at the proxy site, cadence lock held, after the pre-present wait: the
// frame is about to be handed to the runtime, so outputs counted from here on
// are this frame's.
inline void FpsLimiter::ArmGeneratedOutputGatedRelease(ce::fps_limiter_policy::PresentSite site, bool eligible,
                                                       bool frameGenerationActive, int multiplier,
                                                       int effectiveTargetFps) {
    gatedReleaseOutputsNeeded_ = 0;
    if (site != ce::fps_limiter_policy::PresentSite::kRuntimeInputPresent) {
        return;
    }
    runtimeOutputsPresented_.store(0, std::memory_order_release);
    if (!eligible || timerPostPresentPending_ || localTargetTime_ == 0 || qpcFrequency <= 0 ||
        cadenceIntervalUs_ <= 0) {
        return;
    }
    // CPU work ceiling plus the timer margin and what real overruns taught. The
    // present-to-display GPU reservation is left out: under frame generation
    // that series is the runtime's outputs, which this release exists to clear.
    const int64_t budgetUs = ce::fps_limiter_policy::ResolveFrameWorkBudgetUs(
        observedFrameWorkCeilingUs_, adaptiveFineMarginUs_ + frontLoadHeadroomUs_, cadenceIntervalUs_,
        frameWorkSampleCount_, kFrameWorkMinimumSamples);
    if (budgetUs <= 0 || budgetUs >= cadenceIntervalUs_) {
        return;  // not measurable or no room: the back edge
    }
    gatedReleaseOutputsNeeded_ =
        ce::runtime_input_site_policy::GeneratedOutputsPerFrame(frameGenerationActive, multiplier);
    timerPostPresentPending_ = true;
    timerPostPresentTargetTime_ = localTargetTime_ - ((budgetUs * qpcFrequency) / 1000000);
    frontLoadedReleaseRan_ = false;
    gatedReleaseBudgetUs_ = budgetUs;
    if (!gatedReleaseLogged_) {
        gatedReleaseLogged_ = true;
        HookLog(
            "FPS Limiter: frame-generation release waits for the frame's %u generated output(s), at the latest "
            "%lldus before the deadline (target=%d fps, work ceiling=%lldus, interval=%lldus) - the game's next "
            "frame no longer runs under the runtime's generated Present",
            gatedReleaseOutputsNeeded_, budgetUs, effectiveTargetFps, observedFrameWorkCeilingUs_,
            cadenceIntervalUs_);
    }
}

// Waits until `needed` outputs were presented or `boundTick`, whichever comes
// first; true when the outputs released it. The high-resolution timer is armed
// one fine margin before the bound and waited on together with the output
// event, and the last margin is SmartWait's, so the bound keeps its precision.
inline bool FpsLimiter::WaitForGeneratedOutputs(uint32_t needed, int64_t boundTick) {
    const auto presented = [&] { return runtimeOutputsPresented_.load(std::memory_order_acquire) >= needed; };
    if (presented()) {
        return true;
    }
    if (clock_.waitUntil) {
        // Test clock: no other thread can present while it stands still.
        SmartWait(boundTick);
        return presented();
    }
    HANDLE event = runtimeOutputEvent_.load(std::memory_order_acquire);
    if (!event) {
        HANDLE created = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE expected = nullptr;
        if (created && !runtimeOutputEvent_.compare_exchange_strong(expected, created, std::memory_order_acq_rel)) {
            CloseHandle(created);
            created = expected;
        }
        event = created;
    }
    const int64_t fineMarginTicks = (adaptiveFineMarginUs_ * qpcFrequency) / 1000000;
    while (!presented()) {
        const int64_t coarseTicks = boundTick - fineMarginTicks - ReadClock();
        const int64_t coarseUs = (coarseTicks * 1000000) / qpcFrequency;
        if (!event || coarseUs <= ce::fps_limiter_policy::kSmartWaitSchedulerTickUs) {
            break;
        }
        std::lock_guard<std::mutex> lock(timerStateMutex_);
        if (!EnsureHighResTimerLocked()) {
            WaitForSingleObject(event, static_cast<DWORD>(coarseUs / 1000));
            continue;
        }
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -coarseUs * 10;
        if (!SetWaitableTimer(highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
            break;
        }
        const HANDLE handles[2] = {event, highResTimer};
        const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>((coarseUs + 999) / 1000 + 2));
        CancelWaitableTimer(highResTimer);
        if (woke != WAIT_OBJECT_0) {
            break;  // the timer: the coarse part of the bound is over
        }
    }
    if (presented()) {
        return true;
    }
    SmartWait(boundTick);
    return presented();
}

inline void FpsLimiter::NoteGatedRelease(bool byOutputs, int64_t waitUs) {
    ++gatedReleaseWindow_.releases;
    gatedReleaseWindow_.byOutputs += byOutputs ? 1 : 0;
    gatedReleaseWindow_.waitUsSum += waitUs;
    gatedReleaseWindow_.waitUsMax = (std::max)(gatedReleaseWindow_.waitUsMax, waitUs);
    if (gatedReleaseWindow_.releases < 600) {
        return;
    }
    HookLog(
        "FPS Limiter: frame-generation release (%u frames): byOutputs=%u byBound=%u waitMeanUs=%lld waitMaxUs=%lld "
        "budgetUs=%lld",
        gatedReleaseWindow_.releases, gatedReleaseWindow_.byOutputs,
        gatedReleaseWindow_.releases - gatedReleaseWindow_.byOutputs,
        gatedReleaseWindow_.waitUsSum / gatedReleaseWindow_.releases, gatedReleaseWindow_.waitUsMax,
        gatedReleaseBudgetUs_);
    gatedReleaseWindow_ = {};
}
