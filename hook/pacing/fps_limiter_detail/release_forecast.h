#pragma once

// Pre-limiter capture stamping. FpsLimiter publishes the deadline its next pre-present wait releases
// the Present on, and a capture taken before that wait carries it as the frame's timestamp. Policy,
// rationale and evidence: hook/pacing/present_release_forecast.h.

#include "hook/pacing/fps_limiter.h"
#include "common/logging/log_meter.h"

#include <cstdio>

// One Apply() call's forecast duty. Every Apply() settles the claim the capture before it made, then
// either publishes the next forecast (the timer cadence released a pre-present wait inside a
// ScopedPreLimiterCapture) or withdraws this thread's forecast, so no later capture can take a
// deadline the limiter no longer holds.
class FpsLimiter::ReleaseForecastScope {
public:
    explicit ReleaseForecastScope(FpsLimiter& limiter)
        : limiter_(limiter), threadId_(static_cast<uint32_t>(GetCurrentThreadId())) {}
    ~ReleaseForecastScope() {
        if (!settled_) {
            limiter_.releaseForecast_.SettleClaim(0, threadId_);
            limiter_.releaseForecast_.ClearOwnedBy(threadId_);
        }
    }
    ReleaseForecastScope(const ReleaseForecastScope&) = delete;
    ReleaseForecastScope& operator=(const ReleaseForecastScope&) = delete;

    // After the timer cadence's pre-present wait. `releaseQpc` is the release this frame got,
    // `nextReleaseQpc` the deadline armed for the next one; `eligible` means one application
    // Present per grid slot with frame generation off.
    void SettleAndPublish(int64_t releaseQpc, int64_t nextReleaseQpc, int64_t intervalQpc, bool eligible) {
        settled_ = true;
        limiter_.releaseForecast_.SettleClaim(releaseQpc, threadId_);
        if (eligible && nextReleaseQpc != 0 && intervalQpc > 0 &&
            ce::present_release_forecast::InPreLimiterCaptureScope()) {
            limiter_.releaseForecast_.Publish(nextReleaseQpc, intervalQpc, threadId_);
        } else {
            limiter_.releaseForecast_.ClearOwnedBy(threadId_);
        }
    }

private:
    FpsLimiter& limiter_;
    const uint32_t threadId_;
    bool settled_ = false;
};

inline int64_t FpsLimiter::StampPreLimiterCapture(int64_t captureQpc) {
    if (!ce::present_release_forecast::InPreLimiterCaptureScope()) {
        return captureQpc;
    }
    using ce::present_release_forecast::StampSource;
    const ce::present_release_forecast::Stamp stamp =
        releaseForecast_.StampCapture(captureQpc, static_cast<uint32_t>(GetCurrentThreadId()));
    // One line when captures move between forecast-backed stamps and the capture clock (limiter
    // start or stop, a cadence the forecast does not cover such as frame generation), never per
    // frame. A frame that finds its deadline passed is still on the forecast's grid.
    static ce::log_meter::ChangeGate s_sourceGate;
    const bool forecastBacked =
        stamp.source == StampSource::kForecastRelease || stamp.source == StampSource::kDeadlinePassed;
    if (const auto verdict = s_sourceGate.Observe(forecastBacked ? 1u : 0u)) {
        const uint32_t change = releaseForecastSourceChanges_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (change <= 16 || change % 64 == 0) {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            const long long leadUs =
                frequency.QuadPart > 0 ? (stamp.qpc - captureQpc) * 1000000 / frequency.QuadPart : 0;
            HookLog("FPS Limiter: pre-limiter captures stamped by %s (leadUs=%lld, change #%u)%s",
                    ce::present_release_forecast::StampSourceName(stamp.source), leadUs, change,
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return stamp.qpc;
}

namespace fps_limiter_detail {

// The stamping report for the cadence stats line, " stamp(...)", or empty when no pre-limiter
// capture asked during the window. release/clock/passed/far count the stamp sources; claims are
// matched/missed/unreleased against the release the following Apply() made.
inline void FormatReleaseForecastWindow(const ce::present_release_forecast::Window& window, int64_t qpcFrequency,
                                        char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    out[0] = '\0';
    const uint32_t stamps =
        window.forecastStamps + window.captureClockStamps + window.deadlinePassed + window.outOfRange;
    if (stamps == 0 && window.claimsUnreleased == 0) {
        return;
    }
    const auto toUs = [qpcFrequency](int64_t ticks) -> long long {
        return qpcFrequency > 0 ? ticks * 1000000 / qpcFrequency : 0;
    };
    snprintf(out, outSize,
             " stamp(release=%u clock=%u passed=%u far=%u claims=%u/%u/%u leadMeanUs=%lld leadMaxUs=%lld "
             "claimErrMaxUs=%lld)",
             window.forecastStamps, window.captureClockStamps, window.deadlinePassed, window.outOfRange,
             window.claimsMatched, window.claimsMissed, window.claimsUnreleased,
             window.forecastStamps != 0 ? toUs(window.leadQpcSum / static_cast<int64_t>(window.forecastStamps)) : 0,
             toUs(window.leadQpcMax), toUs(window.claimErrorQpcMax));
}

}  // namespace fps_limiter_detail
