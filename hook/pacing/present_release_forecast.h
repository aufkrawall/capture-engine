#pragma once

// The instant a captured frame is actually presented, for captures taken BEFORE the FPS limiter's
// pre-present wait.
//
// The DXGI present detours copy the back buffer (ProcessFrame) and only then let the limiter hold
// the Present until its grid deadline. A capture stamped with its own clock reading therefore
// carries "previous deadline + this frame's build time", not the grid the limiter actually presents
// on: BioShock Infinite at a 120 fps capture-sync cap (session 20261019_164020) presented on an
// exact grid (phase sd 0.2 ms over 4490 frames) while its stamps sat 1.2-6.1 ms before the Present
// and moved ~4.5 ms whenever the game's load changed. Media's CFR phase lock chased those moves
// (three acquisitions, three rephases, two releases in 36 s), each costing a repeat+skip pair.
//
// The limiter already knows the deadline its next pre-present wait releases on when the capture
// runs, so it publishes that as a forecast and the capture takes it as the frame's timestamp. Each
// later Apply() checks the claimed deadline against the release it really made.

#include <atomic>
#include <cstdint>

namespace ce::present_release_forecast {

// Open ScopedPreLimiterCapture scopes on this thread.
inline thread_local int t_preLimiterCaptureScopes = 0;

inline bool InPreLimiterCaptureScope() {
    return t_preLimiterCaptureScopes > 0;
}

// Opened by a present site whose captures run BEFORE its FpsLimiter::Apply() and whose Present
// follows that Apply()'s wait (the DXGI detours), and closed once Apply() returned. Inside it a
// capture takes the limiter's pending present release as its timestamp and Apply() publishes the
// next one. Sites that wait after Present (D3D9, D3D8, OpenGL, DirectDraw) or capture after the
// wait (Vulkan) never open it: their capture clock already is the present instant.
class ScopedPreLimiterCapture {
public:
    ScopedPreLimiterCapture() {
        ++t_preLimiterCaptureScopes;
    }
    ~ScopedPreLimiterCapture() {
        Close();
    }
    void Close() {
        if (open_) {
            open_ = false;
            --t_preLimiterCaptureScopes;
        }
    }
    ScopedPreLimiterCapture(const ScopedPreLimiterCapture&) = delete;
    ScopedPreLimiterCapture& operator=(const ScopedPreLimiterCapture&) = delete;

private:
    bool open_ = true;
};

enum class StampSource : uint8_t {
    kCaptureClock = 0,    // no forecast: the capture's own clock reading
    kForecastRelease,     // the limiter's pending pre-present deadline
    kDeadlinePassed,      // the forecast deadline is already behind the capture: the Present goes out now
    kForecastOutOfRange,  // further ahead than any cadence step can put it: stale, ignored
};

inline const char* StampSourceName(StampSource source) {
    switch (source) {
        case StampSource::kForecastRelease:
            return "present-release";
        case StampSource::kDeadlinePassed:
            return "deadline-passed";
        case StampSource::kForecastOutOfRange:
            return "forecast-out-of-range";
        case StampSource::kCaptureClock:
        default:
            return "capture-clock";
    }
}

struct Forecast {
    int64_t releaseQpc = 0;   // deadline the next pre-present wait releases the Present on
    int64_t intervalQpc = 0;  // grid interval that produced it
};

struct Stamp {
    int64_t qpc = 0;
    StampSource source = StampSource::kCaptureClock;
};

// A capture always follows the release that set the forecast. On time, the next deadline is one
// interval after that release; capture-sync late recovery skips whole slots until the deadline is
// at least half an interval ahead (at most 1.5 intervals); general late recovery and a cadence's
// first frame place it one or half an interval out. Two intervals cover all of them plus the
// rational grid's remainder ticks without admitting a deadline from a different cadence.
constexpr int64_t MaxForecastLeadQpc(int64_t intervalQpc) {
    return intervalQpc * 2;
}

inline Stamp ResolveSourceFrameStamp(int64_t captureQpc, const Forecast& forecast) {
    if (forecast.releaseQpc == 0 || forecast.intervalQpc <= 0) {
        return {captureQpc, StampSource::kCaptureClock};
    }
    const int64_t leadQpc = forecast.releaseQpc - captureQpc;
    if (leadQpc <= 0) {
        // The limiter finds the deadline passed and presents immediately, which is the capture's
        // own moment give or take the CE work in between.
        return {captureQpc, StampSource::kDeadlinePassed};
    }
    if (leadQpc > MaxForecastLeadQpc(forecast.intervalQpc)) {
        return {captureQpc, StampSource::kForecastOutOfRange};
    }
    return {forecast.releaseQpc, StampSource::kForecastRelease};
}

// The wait's own wake error is tens of microseconds, so a release within a quarter interval of the
// claimed deadline is that slot. Beyond it the cadence re-based between the capture and the wait
// (a target, mode or frame-generation change) and the frame carried a stale slot.
constexpr bool ClaimMatchesRelease(int64_t claimedQpc, int64_t releaseQpc, int64_t intervalQpc) {
    const int64_t errorQpc = releaseQpc >= claimedQpc ? releaseQpc - claimedQpc : claimedQpc - releaseQpc;
    return intervalQpc > 0 && errorQpc <= intervalQpc / 4;
}

// One stats window of stamping and claim outcomes, reported with the limiter's cadence stats.
struct Window {
    uint32_t forecastStamps = 0;
    uint32_t captureClockStamps = 0;
    uint32_t deadlinePassed = 0;
    uint32_t outOfRange = 0;
    uint32_t claimsMatched = 0;
    uint32_t claimsMissed = 0;      // released on a different slot than the stamp claimed
    uint32_t claimsUnreleased = 0;  // the following Apply() made no grid release at all
    int64_t leadQpcSum = 0;         // claimed deadline minus capture clock, forecast stamps only
    int64_t leadQpcMax = 0;
    int64_t claimErrorQpcMax = 0;  // largest |release - claim|
};

// Forecast publication plus claim bookkeeping. Publish/ClearOwnedBy/SettleClaim run on the presenting
// thread inside Apply(); StampCapture runs on the capturing thread, which a forecast only serves
// when it is the same thread. The forecast is a seqlock so a reader never pairs one publication's
// deadline with another's interval or owner.
class Forecaster {
public:
    void Publish(int64_t releaseQpc, int64_t intervalQpc, uint32_t ownerThreadId) {
        seq_.fetch_add(1, std::memory_order_acq_rel);
        releaseQpc_.store(releaseQpc, std::memory_order_relaxed);
        intervalQpc_.store(intervalQpc, std::memory_order_relaxed);
        ownerThreadId_.store(ownerThreadId, std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_release);
    }

    // Another present thread's Apply() leaves this thread's forecast alone: its own captures are
    // never served by it, and the owner's next Apply() replaces or clears it.
    void ClearOwnedBy(uint32_t threadId) {
        if (threadId != 0 && ownerThreadId_.load(std::memory_order_relaxed) == threadId) {
            Publish(0, 0, 0);
        }
    }

    Forecast Read(uint32_t threadId) const {
        const uint32_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            return {};
        }
        Forecast forecast{releaseQpc_.load(std::memory_order_relaxed), intervalQpc_.load(std::memory_order_relaxed)};
        const uint32_t owner = ownerThreadId_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != before || owner == 0 || owner != threadId) {
            return {};
        }
        return forecast;
    }

    Stamp StampCapture(int64_t captureQpc, uint32_t threadId) {
        const Forecast forecast = Read(threadId);
        const Stamp stamp = ResolveSourceFrameStamp(captureQpc, forecast);
        switch (stamp.source) {
            case StampSource::kForecastRelease: {
                const int64_t leadQpc = stamp.qpc - captureQpc;
                forecastStamps_.fetch_add(1, std::memory_order_relaxed);
                leadQpcSum_.fetch_add(leadQpc, std::memory_order_relaxed);
                StoreMax(leadQpcMax_, leadQpc);
                claimThreadId_.store(threadId, std::memory_order_relaxed);
                claimIntervalQpc_.store(forecast.intervalQpc, std::memory_order_relaxed);
                claimQpc_.store(stamp.qpc, std::memory_order_release);
                break;
            }
            case StampSource::kDeadlinePassed:
                deadlinePassed_.fetch_add(1, std::memory_order_relaxed);
                break;
            case StampSource::kForecastOutOfRange:
                outOfRange_.fetch_add(1, std::memory_order_relaxed);
                break;
            case StampSource::kCaptureClock:
            default:
                captureClockStamps_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
        return stamp;
    }

    // `releaseQpc` is the clock right after this thread's pre-present wait, 0 when this Apply()
    // released nothing on a grid. The claim is judged against the slot it claimed, so a cadence
    // that re-based in between counts as missed. A claim another thread made stays for that
    // thread's Apply().
    void SettleClaim(int64_t releaseQpc, uint32_t threadId) {
        if (claimQpc_.load(std::memory_order_acquire) == 0 ||
            claimThreadId_.load(std::memory_order_relaxed) != threadId) {
            return;
        }
        const int64_t claimedQpc = claimQpc_.exchange(0, std::memory_order_acq_rel);
        if (claimedQpc == 0) {
            return;
        }
        if (releaseQpc == 0) {
            claimsUnreleased_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const int64_t errorQpc = releaseQpc >= claimedQpc ? releaseQpc - claimedQpc : claimedQpc - releaseQpc;
        StoreMax(claimErrorQpcMax_, errorQpc);
        if (ClaimMatchesRelease(claimedQpc, releaseQpc, claimIntervalQpc_.load(std::memory_order_relaxed))) {
            claimsMatched_.fetch_add(1, std::memory_order_relaxed);
        } else {
            claimsMissed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Window TakeWindow() {
        Window window;
        window.forecastStamps = forecastStamps_.exchange(0, std::memory_order_relaxed);
        window.captureClockStamps = captureClockStamps_.exchange(0, std::memory_order_relaxed);
        window.deadlinePassed = deadlinePassed_.exchange(0, std::memory_order_relaxed);
        window.outOfRange = outOfRange_.exchange(0, std::memory_order_relaxed);
        window.claimsMatched = claimsMatched_.exchange(0, std::memory_order_relaxed);
        window.claimsMissed = claimsMissed_.exchange(0, std::memory_order_relaxed);
        window.claimsUnreleased = claimsUnreleased_.exchange(0, std::memory_order_relaxed);
        window.leadQpcSum = leadQpcSum_.exchange(0, std::memory_order_relaxed);
        window.leadQpcMax = leadQpcMax_.exchange(0, std::memory_order_relaxed);
        window.claimErrorQpcMax = claimErrorQpcMax_.exchange(0, std::memory_order_relaxed);
        return window;
    }

private:
    static void StoreMax(std::atomic<int64_t>& target, int64_t value) {
        int64_t current = target.load(std::memory_order_relaxed);
        while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }

    std::atomic<uint32_t> seq_{0};
    std::atomic<int64_t> releaseQpc_{0};
    std::atomic<int64_t> intervalQpc_{0};
    std::atomic<uint32_t> ownerThreadId_{0};
    std::atomic<int64_t> claimQpc_{0};
    std::atomic<uint32_t> claimThreadId_{0};
    std::atomic<int64_t> claimIntervalQpc_{0};
    std::atomic<uint32_t> forecastStamps_{0};
    std::atomic<uint32_t> captureClockStamps_{0};
    std::atomic<uint32_t> deadlinePassed_{0};
    std::atomic<uint32_t> outOfRange_{0};
    std::atomic<uint32_t> claimsMatched_{0};
    std::atomic<uint32_t> claimsMissed_{0};
    std::atomic<uint32_t> claimsUnreleased_{0};
    std::atomic<int64_t> leadQpcSum_{0};
    std::atomic<int64_t> leadQpcMax_{0};
    std::atomic<int64_t> claimErrorQpcMax_{0};
};

}  // namespace ce::present_release_forecast
