#pragma once

// clang-format off
#include <windows.h>
#include <timeapi.h>  // For timeBeginPeriod/timeEndPeriod
// clang-format on
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include "antilag2_limiter.h"
#include "fg_detection.h"
#include "fps_limiter_policy.h"
#include "hook_common.h"
#include "hook_context.h"
#include "ipc_client.h"
#include "reflex_limiter.h"
#include "xell_limiter.h"

// LimiterMode values matching the enum in config.h (duplicated here to avoid
// config.h dependency in the hook DLL which has no STL string support at load).
namespace LimiterModeValues {
constexpr uint32_t kBasic = 0;
constexpr uint32_t kFGFallback = 1;
constexpr uint32_t kNative = 2;  // NVIDIA Reflex
constexpr uint32_t kAuto = 3;
constexpr uint32_t kAntiLag2 = 4;  // AMD Anti-Lag 2
constexpr uint32_t kXeLL = 5;      // Intel XeLL
}  // namespace LimiterModeValues

// Shared FPS limiter - event-based synchronization with limiter process
// Call Apply() each frame before present
//
// Improvements:
// - Hybrid sleep/spin strategy for lower CPU usage
// - Frame drop tracking
// - Better timeout calculation
// - High-resolution waitable timer for sub-ms precision (Windows 10 1803+)
// - 1ms timer resolution via timeBeginPeriod
class FpsLimiter {
private:
    struct LocalCadenceResult {
        int64_t scheduledWaitUs = 0;
        int64_t actualWaitUs = 0;
        int64_t lateUs = 0;
        uint32_t frameCount = 0;
        uint32_t statsWaitedFrames = 0;
        uint32_t statsLateFrames = 0;
        uint32_t statsResetFrames = 0;
        uint32_t statsSkippedGridSlots = 0;
        int64_t statsAvgLateUs = 0;
        int64_t statsMaxLateUs = 0;
        bool emitStats = false;
        bool waited = false;
        bool resetCadence = false;
        double avgFps = 0;
        double instantFps = 0;
    };

    LocalCadenceResult RunLocalCadence(int effectiveTargetFps, bool preserveCaptureSyncPhase);

public:
    void SetIPCClient(IPCClient* ipc) {
        this->ipc = ipc;
    }

    // For testing: inject mock shared memory
    void SetSharedMemory(SharedMemoryLayout* shm) {
        this->dbgShm = shm;
    }

    // Get count of frames where limiter couldn't keep up
    uint32_t GetMissedFrames() const {
        return missedFrames;
    }
    void ResetMissedFrames() {
        missedFrames = 0;
    }
    bool IsEventsInitialized() const {
        std::lock_guard<std::mutex> lock(eventStateMutex_);
        return eventsInitialized;
    }
    // Get last actual wait time in microseconds (for perf logging)
    int64_t GetLastWaitUs() const {
        return lastActualWaitUs_;
    }
    // Returns true when the limiter is actively pacing frames (capture_sync or general).
    // Used by Present hooks to disable vsync (SyncInterval=0) so that the limiter
    // has full control over frame pacing — vsync's vblank wait absorbs our delay otherwise.
    bool IsActivelyLimiting() const {
        return isActivelyLimiting_.load(std::memory_order_relaxed);
    }

    // Check whether the general FPS limiter is configured (enabled + fps > 0)
    // using shared-memory values, without requiring the limiter to be actively
    // pacing yet.  Used during device creation where shared memory is available
    // but the limiter Apply() hasn't run.
    static bool IsGeneralConfigured(SharedMemoryLayout* shm) {
        if (!shm)
            return false;
        return shm->fpsLimiter.GetGeneralEnabled() && shm->fpsLimiter.GetGeneralFps() > 0;
    }

    // Ensure 1ms timer resolution is enabled
    void EnsureTimerResolution();

    // Smart wait until target QPC time
    // Returns true if we waited, false if we were already past target
    bool SmartWait(int64_t targetTick);

    // Direct trace log for debugging — bypasses all log infrastructure
    void TraceLog(const char* fmt, ...);

    void ApplyPostPresent();

    // Called each frame before present. DXGI/DX12 call sites can allow explicit
    // CE-owned Reflex pacing to defer its wait until after Present returns, so
    // the blocked time sits before the next frame's simulation/render work.
    void Apply(bool allowPostPresentReflexCadence = false);

    void Shutdown();

private:
    void RecordTimerOvershoot(int64_t overshootUs);

    void TryInitializeDx12NativeLimiters(uint32_t configuredMode);

    void ResetReflexNativePacingState();

    IPCClient* ipc = nullptr;
    SharedMemoryLayout* dbgShm = nullptr;  // Direct injection for testing
    HANDLE releaseEvent = NULL;
    HANDLE requestEvent = NULL;
    HANDLE highResTimer = NULL;  // High-resolution waitable timer (Win10 1803+)
    bool eventsInitialized = false;
    bool sessionIdPublished = false;
    bool timerResolutionSet = false;  // Whether timeBeginPeriod(1) was called
    bool highResTimerFailed = false;  // Fall back to polling if timer creation fails
    bool loggedInactive_ = false;     // Tracks whether the inactive log was already emitted
    bool loggedNoEvent_ = false;      // Tracks whether the no-event warning was already emitted
    bool loggedActive_ = false;       // Tracks whether the active-state log was already emitted
    int64_t qpcFrequency = 0;
    uint32_t missedFrames = 0;  // Track frames where limiter couldn't keep up
    // CRITICAL FIX: Per-instance log counters (was static, never reset)
    int timeoutLogCount_ = 0;
    int targetLogCount_ = 0;
    int targetHitLogCount_ = 0;
    int lastTargetFps_ = 0;
    bool lastUsedCaptureSync_ = false;
    uint32_t lastEffectiveMode_ = LimiterModeValues::kAuto;  // Track mode changes for logging
    int nativeApiRecheckCounter_ = 0;                        // Frame counter for periodic native API re-check
    bool reflexLimiterActive_ = false;                       // True when Reflex is handling pacing
    bool reflexDeviceProvided_ = false;                      // True once we've given device to ReflexLimiter
    bool reflexNativeSleepActive_ = false;                   // True while recent game Sleep calls are pacing natively
    bool reflexLoggedSuccess_ = false;                       // True once we've logged successful Reflex activation
    bool loggedNativeFallback_ = false;                      // Avoid spam when native mode falls back to timer
    bool reflexPostPresentCadencePending_ = false;           // True when explicit Reflex waits after Present returns
    int reflexPostPresentTargetFps_ = 0;                     // Target for pending post-present Reflex cadence
    bool reflexPostPresentCaptureSync_ = false;              // Pending cadence owns the capture grid phase
    bool reflexPostPresentPushOk_ = false;                   // Pre-present push state captured for diagnostics
    bool reflexPostPresentDeviceReady_ = false;              // Device state captured for diagnostics
    bool reflexPostPresentRecentGap_ = false;                // Present-gap state captured for diagnostics
    bool reflexPostPresentSkipSleep_ = false;    // Skip CE-owned Sleep in ApplyPostPresent (game owns Reflex)
    bool reflexPostPresentArmedLogged_ = false;  // Avoid spam when arming post-present cadence
    uint32_t reflexSleepBaselineCount_ =
        0;  // Sleep count at the last disruption; native handoff needs a fresh streak after it
    bool reflexRecentPresentGap_ = false;          // Edge detector for recent large Present gaps
    bool antilag2InitAttempted_ = false;           // Lazy init flag for Anti-Lag 2
    bool xellInitAttempted_ = false;               // Lazy init flag for XeLL
    int64_t lastApplyReturnQpc = 0;                // QPC tick when Apply() last returned from wait (dedup guard)
    int64_t localTargetTime_ = 0;                  // QPC target for local capture sync cadence
    int localIntervalFps_ = 0;                     // Denominator of the rational QPC cadence
    int64_t localIntervalRemainder_ = 0;           // Bresenham remainder; prevents integer-FPS drift
    uint32_t localFrameCount_ = 0;                 // Frame count for local capture sync stats
    int64_t localStatsIntervalStart_ = 0;          // QPC start of current stats interval
    uint32_t localStatsFrameCount_ = 0;            // Frame count within current stats interval
    uint32_t localStatsWaitedFrames_ = 0;          // Frames in current interval where local cadence waited
    uint32_t localStatsLateFrames_ = 0;            // Frames in current interval that arrived after the target
    uint32_t localStatsResetFrames_ = 0;           // Cadence resets caused by long gaps or slow frames
    uint32_t localStatsSkippedGridSlots_ = 0;      // Whole capture-grid slots skipped without changing phase
    int64_t localStatsLateUsSum_ = 0;              // Sum of late frame time in current interval
    int64_t localStatsMaxLateUs_ = 0;              // Worst late frame time in current interval
    int64_t lastActualWaitUs_ = 0;                 // Last Apply() actual wait time in μs
    std::atomic<bool> isActivelyLimiting_{false};  // True when limiter is actively pacing frames
    uint32_t applyActiveDedupCount_ = 0;
    uint32_t applyWaitCount_ = 0;
    uint32_t applySuccessCount_ = 0;
    int64_t lastApplyEntryQpc_ = 0;
    int64_t applyInterFrameSum_ = 0;
    uint32_t applyInterFrameCount_ = 0;
    int applyTraceCount_ = 0;
    uint32_t applyDedupCount_ = 0;
    int traceLogCount_ = 0;
    mutable std::mutex eventStateMutex_;
    mutable std::mutex timerStateMutex_;
    mutable std::mutex cadenceMutex_;
    std::array<int64_t, 64> timerOvershootUs_{};
    size_t timerOvershootCursor_ = 0;
    size_t timerOvershootSampleCount_ = 0;
    int64_t adaptiveFineMarginUs_ = 100;
    std::atomic<uint32_t> concurrentApplySkips_{0};
    static inline std::atomic<int> s_TimerResolutionRefCount{0};
};

// Member definitions live out of line to keep this header near the AGENTS.md
// size ceiling. They stay inline, so the per-frame path is unchanged. These
// must come after the class body above.
#include "fps_limiter_detail/frame_pacing.h"
#include "fps_limiter_detail/apply.h"
#include "fps_limiter_detail/lifecycle.h"

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
