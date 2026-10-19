#pragma once

// Out-of-line FpsLimiter members for the game-thread proxy Present site
// (PresentSite::kRuntimeInputPresent). Included by fps_limiter.h after the
// class body. See ce::runtime_input_site_policy for why it owns the cadence.

#include "hook/pacing/fps_limiter.h"

// True when this Apply() must pass without pacing: the game's own Present into
// the frame-generation runtime owns the cadence and this is one of the
// runtime's outputs (or any other non-input site) while that ownership holds.
//
// `callerPairsPostPresent` (Apply's allowPostPresentReflexCadence): the call
// site makes the matching ApplyPostPresent() call. Only such a pass may leave
// t_FpsLimiterPassedApplyPending for it, and every Apply() clears the mark
// first. A pass whose caller never calls ApplyPostPresent() (the third-party
// overlay bypass, Vulkan acquire-time gating) used to leave the mark set, and
// the thread's next real release - after ownership lapsed, or the game's own
// release around a nested present - was then taken for a runtime output's and
// skipped.
inline bool FpsLimiter::PassBecauseRuntimeInputOwnsPacing(ce::fps_limiter_policy::PresentSite site,
                                                          bool callerPairsPostPresent) {
    using ce::fps_limiter_policy::PresentSite;
    using namespace ce::runtime_input_site_policy;
    t_FpsLimiterPassedApplyPending = false;
    if (site == PresentSite::kRuntimeInputPresent) {
        const uint32_t previous = outputsSinceRuntimeInput_.exchange(0, std::memory_order_acq_rel);
        if (!RuntimeInputOwnsPacing(previous)) {
            runtimeInputOwnershipStarts_.fetch_add(1, std::memory_order_relaxed);
            HookLog(
                "FPS Limiter: the game's Present into the frame-generation runtime owns pacing; runtime outputs "
                "pass (starts=%u passed=%llu unpaired=%llu)",
                runtimeInputOwnershipStarts_.load(std::memory_order_relaxed),
                static_cast<unsigned long long>(runtimeInputPassCount_.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(runtimeInputUnpairedPassCount_.load(std::memory_order_relaxed)));
        }
        return false;
    }
    runtimeOutputStreamIsFinal_.store(site == PresentSite::kRuntimeOutputPresent, std::memory_order_release);
    if (outputsSinceRuntimeInput_.load(std::memory_order_acquire) == kNoRuntimeInputSeen) {
        return false;
    }
    const uint32_t seen = outputsSinceRuntimeInput_.fetch_add(1, std::memory_order_acq_rel);
    if (RuntimeInputOwnsPacing(seen)) {
        runtimeInputPassCount_.fetch_add(1, std::memory_order_relaxed);
        if (callerPairsPostPresent) {
            t_FpsLimiterPassedApplyPending = true;
        } else {
            runtimeInputUnpairedPassCount_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    // Several whole output groups without one proxy Present: the proxy no
    // longer carries the game's frames. Stop counting (unless a proxy Present
    // re-armed ownership in the meantime) and pace this output stream again.
    uint32_t expected = seen + 1;
    if (seen != kNoRuntimeInputSeen &&
        outputsSinceRuntimeInput_.compare_exchange_strong(expected, kNoRuntimeInputSeen, std::memory_order_acq_rel)) {
        runtimeInputOwnershipLapses_.fetch_add(1, std::memory_order_relaxed);
        HookLog(
            "FPS Limiter: %u presents without the game's Present into the frame-generation runtime; this site "
            "(%u) paces again (lapses=%u passed=%llu unpaired=%llu)",
            seen, static_cast<unsigned>(site), runtimeInputOwnershipLapses_.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(runtimeInputPassCount_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(runtimeInputUnpairedPassCount_.load(std::memory_order_relaxed)));
    }
    return false;
}

// Whether inject capture records the final presented output for this Apply().
// The proxy site sees only application frames itself, so it compares capture
// sync against the source the runtime's output sites would have used.
inline bool FpsLimiter::ResolveInjectFinalOutputForSite(ce::fps_limiter_policy::PresentSite site) const {
    const bool published = injectFinalOutputCaptureAvailable_.load(std::memory_order_acquire);
    if (site == ce::fps_limiter_policy::PresentSite::kRuntimeInputPresent) {
        return ce::runtime_input_site_policy::RuntimeInputCaptureSourceIsFinalOutput(
            published, runtimeOutputStreamIsFinal_.load(std::memory_order_acquire));
    }
    return ce::fps_limiter_policy::IsInjectCaptureFinalOutputForSite(published, site);
}
