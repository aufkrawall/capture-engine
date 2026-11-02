#pragma once

#include <algorithm>
#include <cstdint>

namespace ce::dx9_pacing_policy {

struct PacingDecision {
    bool shouldPace = false;
    bool needsFullscreenFallback = false;
    bool skipDueToLimiter = false;
};

// Evaluates whether DX9 post-present software pacing is needed.
//
// In fullscreen exclusive mode, when the driver successfully applies the requested
// presentation interval (liveInterval == vsyncPresentInterval), hardware handles VSync
// and needsFullscreenFallback is false, avoiding redundant software sleeps.
// When an FPS limiter is actively pacing frames and no fullscreen fallback is required,
// post-present software pacing is skipped to avoid double-pacing judder.
inline PacingDecision EvaluatePacing(
    bool vsyncShouldOverride,
    int vsyncPresentInterval,
    bool stagingCaptureActiveAndRecording,
    bool nonSystemRuntimeLoaded,
    bool windowed,
    uint32_t liveInterval,
    int64_t presentUs,
    bool fpsLimiterActivelyLimiting) {

    PacingDecision decision;
    if (!vsyncShouldOverride || vsyncPresentInterval <= 0) {
        return decision;
    }
    if (stagingCaptureActiveAndRecording) {
        return decision;
    }
    if (nonSystemRuntimeLoaded) {
        return decision;
    }

    decision.needsFullscreenFallback =
        !windowed && vsyncPresentInterval > 0 && liveInterval != static_cast<uint32_t>(vsyncPresentInterval);

    if (fpsLimiterActivelyLimiting && !decision.needsFullscreenFallback) {
        decision.skipDueToLimiter = true;
        return decision;
    }

    decision.shouldPace = (windowed && (presentUs < 3000)) || decision.needsFullscreenFallback;
    return decision;
}

// Computes hook overhead by excluding intentional waits:
// - fpsLimitUs: intentional pacing delay from FpsLimiter
// - presentCallUs: time blocked inside the driver's Present call (e.g. hardware VSync wait)
// - prerenderWaitUs: intentional delay from CPU prerender limit query
inline int64_t CalculatePresentOverheadUs(
    int64_t totalUs,
    int64_t fpsLimitUs,
    int64_t presentCallUs,
    int64_t prerenderWaitUs) {
    return std::max<int64_t>(0, totalUs - fpsLimitUs - presentCallUs - prerenderWaitUs);
}

}  // namespace ce::dx9_pacing_policy
