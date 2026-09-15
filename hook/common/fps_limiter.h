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
#include "fg_detection.h"
#include "fps_limiter_policy.h"
#include "hook_common.h"
#include "hook_context.h"
#include "ipc_client.h"
#include "reflex_limiter.h"

// LimiterMode values matching the enum in config.h (duplicated here to avoid
// config.h dependency in the hook DLL which has no STL string support at load).
namespace LimiterModeValues {
constexpr uint32_t kBasic = 0;
constexpr uint32_t kFGFallback = 1;
constexpr uint32_t kNative = 2;  // NVIDIA Reflex
constexpr uint32_t kAuto = 3;
}  // namespace LimiterModeValues

// Optional API-native pacing supplied by a presentation backend that cannot
// use the D3D NvAPI device contract (currently Vulkan). The callback owns its
// API objects; FpsLimiter owns only mode/target selection and post-Present
// placement.
struct NativeFpsPacingBackend {
    void* context = nullptr;
    bool (*isAvailable)(void* context) = nullptr;
    bool (*isGameActive)(void* context) = nullptr;
    bool (*setTargetFps)(void* context, int fps) = nullptr;
    bool (*sleep)(void* context, int64_t* waitUs) = nullptr;
    void (*clear)(void* context) = nullptr;
    const char* name = nullptr;
};

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
        // Real-boundary output-group admission deltas since the previous
        // emission of the 120-frame stats window.
        uint32_t statsBoundaryCallbacks = 0;
        uint32_t statsPacedGroups = 0;
        uint32_t statsGeneratedPasses = 0;
        uint32_t statsGroupResets = 0;
        uint32_t statsConcurrentSkips = 0;
        bool emitStats = false;
        bool waited = false;
        bool resetCadence = false;
        double avgFps = 0;
        double instantFps = 0;
    };

    LocalCadenceResult RunLocalCadence(int targetFps, int cadenceScale, bool preserveCaptureSyncPhase);

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
    // Output-group admission diagnostics for tests and rate-limited stats.
    uint32_t GetBoundaryCallbackCount() const {
        return boundaryCallbackCount_.load(std::memory_order_relaxed);
    }
    uint32_t GetPacedGroupCount() const {
        return pacedGroupCount_.load(std::memory_order_relaxed);
    }
    uint32_t GetGeneratedSlotPassCount() const {
        return generatedSlotPassCount_.load(std::memory_order_relaxed);
    }
    uint32_t GetGroupAdmissionResetCount() const {
        return groupAdmissionResetCount_.load(std::memory_order_relaxed);
    }
    uint32_t GetConcurrentApplySkipCount() const {
        return concurrentApplySkips_.load(std::memory_order_relaxed);
    }
    // Discards the pending output-group ordinal so the next real-boundary
    // callback owns a fresh cadence slot. Used when the pacing boundary itself
    // moves (Vulkan moves limiter work between vkQueuePresentKHR and
    // vkAcquireNextImageKHR when async present is detected) and on IPC/session
    // resets; configuration-driven transitions reset inside Apply().
    void ResetOutputGroupAdmission() {
        std::lock_guard<std::mutex> admissionLock(admissionMutex_);
        if (groupAdmission_.Reset()) {
            groupAdmissionResetCount_.fetch_add(1, std::memory_order_relaxed);
        }
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
    // Close and forget the cached fps_limiter_trace.log path so the next
    // TraceLog call re-resolves it against the current host session.
    void ResetTraceLogPath();

    void ApplyPostPresent();
    void CancelPostPresentPacing() {
        std::lock_guard<std::mutex> lock(cadenceMutex_);
        reflexPostPresentCadencePending_ = false;
        externalNativePostPresentPending_ = false;
        // A present that never completed releases nothing: the next frame's
        // pre-present wait still owns the deadline, so dropping the armed
        // release only costs this frame its front-loaded placement.
        timerPostPresentPending_ = false;
    }

    void SetNativePacingBackend(const NativeFpsPacingBackend& backend) {
        std::lock_guard<std::mutex> lock(cadenceMutex_);
        nativePacingBackend_ = backend;
    }

    // API hooks publish whether their active inject route contains final
    // presented outputs (including generated frames) rather than only base
    // application frames. This is process-local state, so it does not extend
    // the shared-memory ABI and cannot be delayed by media startup handshakes.
    // Displayed-transition feedback: how long a present waited before it was
    // scanned out. Published by whichever thread consumes display timing, so it
    // must never touch the cadence state. While the placement is at the back
    // edge the frame's GPU work is known to have finished before the present,
    // which is the only state in which this measures the irreducible flip
    // latency rather than a frame CE released too late.
    void ObservePresentToDisplay(int64_t presentToDisplayUs) {
        if (presentToDisplayUs < 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(presentToDisplayMutex_);
        presentToDisplayUs_[presentToDisplayCursor_] = presentToDisplayUs;
        presentToDisplayCursor_ = (presentToDisplayCursor_ + 1) % presentToDisplayUs_.size();
        if (presentToDisplaySampleCount_ < presentToDisplayUs_.size()) {
            ++presentToDisplaySampleCount_;
        }
        if (frontLoadedPlacementActive_.load(std::memory_order_acquire)) {
            return;
        }
        if (!presentToDisplayFloorSeeded_ || presentToDisplayUs < presentToDisplayFloorUs_) {
            presentToDisplayFloorUs_ = presentToDisplayUs;
            presentToDisplayFloorSeeded_ = true;
        }
    }

    void SetInjectFinalOutputCaptureAvailable(bool available) {
        injectFinalOutputCaptureAvailable_.store(available, std::memory_order_release);
    }

    // The display's maximum refresh, in the OUTPUT domain, while CE owes the
    // rendered-rate ceiling it took over by standing down from its Vulkan
    // present-mode override on a metered frame generator; 0 when it owes none.
    // Published from the swapchain-creation path, which is the one place that
    // knows both facts; see
    // ce::vulkan_present_metering_policy::ResolveVblankCeilingOutputFps.
    void SetDisplayVblankCeilingFps(int fps) {
        displayVblankCeilingFps_.store(fps > 0 ? fps : 0, std::memory_order_release);
    }

    int GetDisplayVblankCeilingFps() const {
        return displayVblankCeilingFps_.load(std::memory_order_acquire);
    }

    // Called each frame before present. DXGI/DX12 call sites can allow explicit
    // CE-owned Reflex pacing to defer its wait until after Present returns, so
    // the blocked time sits before the next frame's simulation/render work.
    //
    // `site` is the call site's structural contract about its own entries; see
    // ce::fps_limiter_policy::PresentSite. It decides two things:
    //
    // kFinalOutputBoundary (native-Vulkan vkQueuePresentKHR /
    // vkAcquireNextImageKHR) uses deterministic multiplier-sized output-group
    // admission while frame generation is active: exactly one callback per
    // group owns a cadence slot and waits on the exact rational group grid
    // (interval = QPC_frequency * multiplier / configured output target),
    // while the remaining multiplier-1 callbacks are the generated outputs of
    // that already admitted group and pass through a lock-free fast path. The
    // classification is an ordinal, never a time window, so bursts of rapid
    // callbacks cannot be confused with generated spillover the way the legacy
    // 2ms dedup allowed (that escape let Portal RTX run ~146 fps against a 130
    // cap).
    //
    // Every site that cannot deliver a second entry for one logical frame
    // (kFinalOutputBoundary, and kUniqueApplicationPresent with FG off) is
    // gated on the grid: the duplicate-present window is skipped entirely and
    // blocking cadence-lock serialization preserves the Strange Brigade
    // multi-present grid: exactly one present per target interval, evenly
    // spaced. Only kDuplicateProne sites (DXVK Present+PresentEx and the
    // D3D/OpenGL wrappers) keep the dedup fast paths, because their second
    // call is genuinely the same logical frame.
    void Apply(bool allowPostPresentReflexCadence = false,
               ce::fps_limiter_policy::PresentSite site =
                   ce::fps_limiter_policy::PresentSite::kDuplicateProne);

    void Shutdown();

    // Front-loaded cadence placement state, for diagnostics and regression
    // coverage. See fps_limiter_detail/front_load.h.
    struct FrontLoadedPacingState {
        bool armed = false;
        int64_t budgetUs = 0;
        int64_t workCeilingUs = 0;
        int64_t intervalUs = 0;
        int64_t lastReleaseWaitUs = 0;
        int64_t headroomUs = 0;
        int64_t gpuHeadroomUs = 0;
        int64_t presentToDisplayUs = 0;
        int64_t presentToDisplayFloorUs = -1;
        uint32_t releases = 0;
        uint32_t overruns = 0;
        size_t workSamples = 0;
    };

    FrontLoadedPacingState GetFrontLoadedPacingState() const {
        std::lock_guard<std::mutex> lock(cadenceMutex_);
        FrontLoadedPacingState state;
        state.armed = timerPostPresentPending_;
        state.budgetUs = frameWorkBudgetUs_;
        state.workCeilingUs = observedFrameWorkCeilingUs_;
        state.intervalUs = cadenceIntervalUs_;
        state.lastReleaseWaitUs = lastFrontLoadedReleaseWaitUs_;
        state.headroomUs = frontLoadHeadroomUs_;
        state.gpuHeadroomUs = frontLoadGpuHeadroomUs_;
        state.releases = frontLoadedReleaseCount_;
        state.overruns = frontLoadOverrunCount_;
        state.workSamples = frameWorkSampleCount_;
        return state;
    }

private:
    void RecordTimerOvershoot(int64_t overshootUs);

    // Time the game needed to build and submit a frame after the limiter last
    // released it, used to size the front-loaded release. Samples outside the
    // cadence interval are a hitch or a placement change, not frame work.
    void RecordFrameWork(int64_t workUs, int64_t intervalUs);

    // Rational cadence interval of the currently configured grid, in QPC ticks.
    int64_t CadenceIntervalTicks(int targetFps, int cadenceScale) const;

    // Front-loaded cadence placement; see fps_limiter_detail/front_load.h.
    void NoteFrameWorkForFrontLoadedRelease(int64_t nowQpcTicks, int targetFps, int cadenceScale,
                                            bool cadenceFirstFrame);
    void ArmFrontLoadedRelease(bool eligible, int effectiveTargetFps);
    // Learns the extra reservation from presents that actually missed their
    // deadline while the placement was front-loaded.
    void NoteFrontLoadedLateness(int64_t lateUs);
    // Walks the reservation to the point where the frame's GPU work finishes by
    // the deadline; see fps_limiter_detail/front_load.h.
    void UpdateFrontLoadGpuHeadroom();
    // Snapshot of the present-to-display ring, taken without holding its lock
    // across anything.
    struct PresentToDisplaySnapshot {
        int64_t recentUs = 0;
        int64_t floorUs = -1;
        size_t samples = 0;
        bool floorSeeded = false;
    };
    PresentToDisplaySnapshot SnapshotPresentToDisplay() const;
    bool RunFrontLoadedRelease();
    void ResetFrontLoadedPacingState();

    // 120-frame cadence report; see fps_limiter_detail/cadence_diagnostics.h.
    void EmitLocalCadenceStats(const LocalCadenceResult& cadence, int effectiveTargetFps);

    void ResetReflexNativePacingState();

    bool TryHandleReflexNativeWarmup(bool requested, bool driverTargetAccepted,
                                     uint32_t gameSleepCount, uint32_t freshSleepCount,
                                     bool recentPresentGap);

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
    bool lastFGActive_ = false;                              // Include FG activation in cadence transitions
    int lastFGMultiplier_ = 1;                              // Re-arm immediately when MFG factor changes
    bool lastFGRuntimeSignaledActive_ = false;               // Nominal API state can precede actual generation
    int lastFGRuntimeSignaledMultiplier_ = 1;                // Requested/published factor while production is pending
    int lastNativeDriverTargetFps_ = 0;                      // Interval handed to a driver-owned low-latency cap
    int lastCaptureOutputEquivalentFps_ = 0;                 // Capture constraint expressed as displayed FPS
    int lastGeneralConstraintFps_ = 0;                       // Concurrent configured displayed-rate constraint
    bool lastCaptureSourceFinalOutput_ = false;              // Route-domain transition diagnostic
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
    NativeFpsPacingBackend nativePacingBackend_{};
    bool externalNativePostPresentPending_ = false;
    int externalNativeTargetFps_ = 0;
    bool externalNativeLoggedSuccess_ = false;
    uint32_t reflexSleepBaselineCount_ =
        0;  // Sleep count at the last disruption; native handoff needs a fresh streak after it
    uint32_t reflexLastEvaluatedGameSleepCount_ =
        0;  // Progress detector prevents fallback from overlapping newly resumed game Sleep
    bool reflexRecentPresentGap_ = false;          // Edge detector for recent large Present gaps
    int64_t lastApplyReturnQpc = 0;                // QPC tick when Apply() last returned from wait (dedup guard)
    int64_t localTargetTime_ = 0;                  // QPC target for local capture sync cadence
    int localIntervalFps_ = 0;                     // Configured output target of the rational QPC cadence
    int localIntervalScale_ = 1;                   // Cadence scale (FG multiplier) of the rational QPC cadence
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
    std::atomic<bool> injectFinalOutputCaptureAvailable_{false};
    std::atomic<int> displayVblankCeilingFps_{0};
    uint32_t applyActiveDedupCount_ = 0;
    int64_t lastApplyEntryQpc_ = 0;
    int64_t applyInterFrameSum_ = 0;
    uint32_t applyInterFrameCount_ = 0;
    int applyTraceCount_ = 0;
    uint32_t applyDedupCount_ = 0;
    uint32_t strictGridContendedWaits_ = 0;  // Strict-grid presents that had to block on the cadence lock
    // Output-group admission state and diagnostics. The ordinal is only
    // mutated under admissionMutex_, which is never held across a wait, so a
    // generated-slot pass can always classify while a group owner is waiting.
    mutable std::mutex admissionMutex_;
    ce::fps_limiter_policy::OutputGroupAdmission groupAdmission_;
    uint32_t lastAdmissionKey_ = 0;                      // Admission epoch (admissionMutex_-guarded)
    std::atomic<uint32_t> boundaryCallbackCount_{0};     // Total real-boundary Apply() entries
    std::atomic<uint32_t> pacedGroupCount_{0};           // Total pace_group admissions
    std::atomic<uint32_t> generatedSlotPassCount_{0};    // Total generated-slot fast-path passes
    std::atomic<uint32_t> groupAdmissionResetCount_{0};  // Resets that discarded a partial group
    uint32_t statsSnapshotBoundaryCallbacks_ = 0;        // 120-frame stats windows (cadence-mutex-only writes)
    uint32_t statsSnapshotPacedGroups_ = 0;
    uint32_t statsSnapshotGeneratedPasses_ = 0;
    uint32_t statsSnapshotGroupResets_ = 0;
    uint32_t statsSnapshotConcurrentSkips_ = 0;
    int lastCadenceTargetFps_ = 0;                       // Transition key: configured output cadence target
    int lastCadenceScale_ = 1;                           // Transition key: cadence scale (FG multiplier)
    int traceLogCount_ = 0;
    mutable std::mutex eventStateMutex_;
    mutable std::mutex timerStateMutex_;
    mutable std::mutex cadenceMutex_;
    std::array<int64_t, 64> timerOvershootUs_{};
    size_t timerOvershootCursor_ = 0;
    size_t timerOvershootSampleCount_ = 0;
    int64_t adaptiveFineMarginUs_ = 100;
    // Front-loaded cadence placement. The grid deadline and the pre-present
    // wait are untouched by all of this: the release only decides how late in
    // the period the game starts building the frame that deadline presents.
    static constexpr size_t kFrameWorkMinimumSamples = 16;
    std::array<int64_t, 64> frameWorkUs_{};
    size_t frameWorkCursor_ = 0;
    size_t frameWorkSampleCount_ = 0;
    int64_t observedFrameWorkCeilingUs_ = 0;
    int64_t frameWorkBudgetUs_ = 0;
    int64_t cadenceIntervalUs_ = 0;
    int64_t frontLoadHeadroomUs_ = 0;
    int64_t frontLoadGpuHeadroomUs_ = 0;
    uint32_t frontLoadWindowFrames_ = 0;
    static constexpr size_t kPresentToDisplayMinimumSamples = 16;
    static constexpr uint32_t kFrontLoadGpuWindowFrames = 64;
    // Written by whichever thread consumes display timing, read once per window
    // by the present thread. The lock is only ever held for O(1) ring work and
    // never across a wait.
    mutable std::mutex presentToDisplayMutex_;
    std::array<int64_t, 64> presentToDisplayUs_{};
    size_t presentToDisplayCursor_ = 0;
    size_t presentToDisplaySampleCount_ = 0;
    int64_t presentToDisplayFloorUs_ = -1;
    bool presentToDisplayFloorSeeded_ = false;
    std::atomic<bool> frontLoadedPlacementActive_{false};
    uint32_t frontLoadCleanFrames_ = 0;
    uint32_t frontLoadOverrunCount_ = 0;
    bool frontLoadedReleaseRan_ = false;
    bool timerPostPresentPending_ = false;
    int64_t timerPostPresentTargetTime_ = 0;
    int64_t lastFrontLoadedReleaseWaitUs_ = 0;
    uint32_t frontLoadedReleaseCount_ = 0;
    bool frontLoadedPacingLogged_ = false;
    bool loggedMissingGpuEvidence_ = false;
    std::atomic<uint32_t> concurrentApplySkips_{0};
    static inline std::atomic<int> s_TimerResolutionRefCount{0};
};

// Member definitions live out of line to keep this header near the AGENTS.md
// size ceiling. They stay inline, so the per-frame path is unchanged. These
// must come after the class body above.
#include "fps_limiter_detail/cadence_diagnostics.h"
#include "fps_limiter_detail/front_load.h"
#include "fps_limiter_detail/frame_pacing.h"
#include "fps_limiter_detail/apply.h"
#include "fps_limiter_detail/lifecycle.h"

// Global FPS limiter instance
inline FpsLimiter g_SharedFpsLimiter;
