#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ce::freeze_watchdog_policy {

inline bool ShouldDeferImmediateDumpToWatchdogThread(DWORD callerThreadId, DWORD targetThreadId) {
    return targetThreadId != 0 && callerThreadId == targetThreadId;
}

inline bool ShouldCaptureWatchdogDump(bool dumpAlreadyCaptured) {
    return !dumpAlreadyCaptured;
}

inline bool ShouldSuppressFreezeCheckForBackgroundProcess(bool processForeground, bool forceMonitor,
                                                          bool presentInFlight, bool runtimePresentationMonitor) {
    return !processForeground && !forceMonitor && !presentInFlight && !runtimePresentationMonitor;
}

// "Render thread frozen" is only a defensible claim once CE has actually seen
// the render loop it monitors. The watchdog is armed from a hook-install worker
// thread, and its heartbeat is fed exclusively from CE's D3D/DXGI present
// paths, so a game that renders through an API CE does not present for (Vulkan
// via the CE layer, OpenGL, a foreign present chain that owns the entry) never
// beats it - the elapsed time then measures "CE never saw a present", not a
// hang. Strange Brigade Vulkan `20260818_190149` is that false positive: the
// game ran at 144 FPS (perf CSV frame 3232 at t=153871 ms) while the watchdog
// dumped at t=153875 ms, and the in-process dump is what actually froze it.
// Presenting evidence that does not need a heartbeat still counts: a Present
// stuck inside CE's own hook, a removed device, or an FG runtime that owns
// presentation each prove a D3D render loop exists.
inline bool ShouldAssertRenderThreadFreeze(bool renderLoopHeartbeatObserved, bool presentInFlight, bool forceMonitor,
                                           bool runtimePresentationMonitor) {
    return renderLoopHeartbeatObserved || presentInFlight || forceMonitor || runtimePresentationMonitor;
}

// Which thread a freeze dump must capture when nothing has claimed the
// monitored render thread. A present that is still in flight is a thread stuck
// inside CE's own hook, so its stack is the freeze - DOOM Eternal
// `20260819_020933` dumped with targetTid=0 and !analyze -hang named the idle
// main thread while the real deadlock sat in CE's flip-queue pacing wait on the
// Vulkan runtime's presenter thread.
inline DWORD ResolveFreezeDumpTargetThread(DWORD resolvedThreadId, bool presentInFlight,
                                           DWORD presentHookThreadId) {
    if (resolvedThreadId != 0) {
        return resolvedThreadId;
    }
    return presentInFlight ? presentHookThreadId : 0;
}

// Liveness published by a present path that cannot call Heartbeat() itself -
// currently the Vulkan layer's `runtimeState.vulkanPresentTick`, which lives in
// a separate DLL. A tick that is never published, or one from the future, is
// not liveness.
inline bool IsObservedPresentRecent(uint64_t lastPresentTickMs, uint64_t nowTickMs, uint64_t maxAgeMs) {
    return lastPresentTickMs != 0 && nowTickMs >= lastPresentTickMs && (nowTickMs - lastPresentTickMs) <= maxAgeMs;
}

// Which observed render loop still counts as evidence right now. A D3D present
// path proved itself from inside this module and that proof cannot expire. The
// Vulkan layer's proof can: it is another DLL, and when it detaches or drops
// its IPC connection it simply stops publishing, which is indistinguishable
// from a frozen Vulkan render loop. Withdrawing that evidence with the layer is
// what keeps a dormant layer from turning a healthy game into a freeze claim -
// the exact shape of the bug this gate exists to prevent.
inline bool HasLiveRenderLoopEvidence(bool d3dPresentObserved, bool vulkanLayerPresentObserved,
                                      bool vulkanLayerStillActive) {
    return d3dPresentObserved || (vulkanLayerPresentObserved && vulkanLayerStillActive);
}

}  // namespace ce::freeze_watchdog_policy

// Freeze detection watchdog - monitors thread heartbeats and creates dumps on freeze
// Design principles:
// 1. Pre-load DbgHelp.dll to avoid loader lock during crash
// 2. Capture actual thread context on freeze for meaningful stack traces
// 3. Proper thread lifecycle with join timeout (no detached threads)
// 4. Dynamic timeout based on game engine detection (UE5, DLSS FG, etc.)
class FreezeWatchdog {
public:
    using FreezeCallback = std::function<void(const std::string& reason)>;
    using PreferredThreadProvider = DWORD (*)();

    FreezeWatchdog() noexcept;
    ~FreezeWatchdog();

    // Non-copyable, non-movable
    FreezeWatchdog(const FreezeWatchdog&) = delete;
    FreezeWatchdog& operator=(const FreezeWatchdog&) = delete;

    // Start monitoring with specified timeout (seconds)
    // Returns true if started, false if already running
    bool Start(double timeoutSeconds = 5.0);

    // Stop monitoring (waits for thread to exit with timeout)
    void Stop();

    // Call this from the monitored thread to indicate it's alive
    void Heartbeat();

    // Call this from helper threads that prove the render pipeline is still
    // making forward progress without changing which thread the watchdog should
    // capture when a hang is detected.
    void HeartbeatFromHelperThread();

    // Set a custom callback for freeze handling (optional)
    void SetFreezeCallback(FreezeCallback callback);

    // Manual freeze check - returns true if frozen
    bool IsFrozen() const;

    // True once any present path CE observes has beaten at least once, i.e. the
    // render loop this watchdog claims to monitor demonstrably exists.
    bool HasObservedRenderLoop() const {
        return renderLoopObserved_.load(std::memory_order_acquire);
    }

    // Get the monitored thread ID (set via SetMonitoredThread)
    DWORD GetMonitoredThreadId() const {
        return monitoredThreadId_.load();
    }

    // Set the thread to monitor (call from the monitored thread). An explicit,
    // provenance-checked claim outranks the Vulkan layer's published present
    // thread, so the cross-API poll stops re-claiming it afterwards.
    void SetMonitoredThread(DWORD threadId = GetCurrentThreadId()) {
        monitoredThreadId_.store(threadId);
        monitoredThreadFromVulkanLayer_.store(false, std::memory_order_release);
    }

    // Check game engine features that affect timeout
    bool IsDLSSFGActive() const;
    bool IsUE5Active() const;

    // Get recommended timeout based on detected game engine
    double GetRecommendedTimeout() const;

    // Force freeze monitoring even when the process is in the background.
    // Set when the GPU device is removed so the watchdog can detect a stuck
    // driver call (e.g. ECL blocking in NtGdiDdDDICreateAllocation).
    void SetForceMonitor(bool force) {
        forceMonitor_.store(force, std::memory_order_release);
    }

    // Force freeze monitoring while a frame-generation runtime owns
    // presentation. These presenter threads can freeze without ordinary game
    // Present focus heuristics firing, especially around FSR swapchain
    // replacement or startup.
    void SetRuntimePresentationMonitor(bool force) {
        runtimePresentationMonitor_.store(force, std::memory_order_release);
    }

    void SetPreferredThreadProvider(PreferredThreadProvider provider) {
        preferredThreadProvider_.store(provider, std::memory_order_release);
    }

    void RequestImmediateDump(const std::string& reason, DWORD preferredThreadId = 0);

private:
    enum class RenderLoopSource { D3DPresent, VulkanLayerPresent };
    void NoteRenderLoopObserved(RenderLoopSource source);
    bool HasLiveRenderLoopEvidence() const;
    void PollCrossApiPresentLiveness();
    void WatchdogThread();
    void CreateMinidumpWithThreadContext(const std::string& reason, DWORD preferredThreadId = 0);
    bool InitializeDbgHelp();

    // Capture context from frozen thread for meaningful dump
    bool CaptureThreadContext(DWORD threadId, CONTEXT& ctx);

    // Create a synthetic exception record for freeze dump
    void CreateFreezeExceptionRecord(EXCEPTION_RECORD& record, const std::string& reason);

    std::atomic<bool> running_{false};
    std::atomic<bool> renderLoopObserved_{false};
    std::atomic<bool> d3dRenderLoopObserved_{false};
    std::atomic<bool> vulkanLayerRenderLoopObserved_{false};
    std::atomic<bool> monitoredThreadFromVulkanLayer_{false};
    // IsFrozen() is const and rate-limits its own diagnostic.
    mutable std::atomic<bool> loggedMissingRenderLoop_{false};
    std::atomic<uint64_t> lastHeartbeat_{0};
    std::atomic<uint64_t> startupTime_{0};
    std::atomic<double> timeoutSeconds_{5.0};
    std::atomic<DWORD> monitoredThreadId_{0};
    std::atomic<bool> forceMonitor_{false};
    std::atomic<bool> runtimePresentationMonitor_{false};
    std::atomic<uint64_t> lastDumpRequestMicros_{0};
    std::atomic<bool> dumpCapturedForCurrentRun_{false};
    std::atomic<PreferredThreadProvider> preferredThreadProvider_{nullptr};
    std::atomic<bool> pendingImmediateDump_{false};
    std::mutex pendingImmediateDumpMutex_;
    std::string pendingImmediateDumpReason_;
    DWORD pendingImmediateDumpTargetTid_{0};

    // Presents published by the CE Vulkan layer count as liveness while they
    // are at most this old. Matches the hook-install Vulkan-ownership window
    // and spans several watchdog polls.
    static constexpr uint64_t kCrossApiPresentMaxAgeMs = 2000;

    static constexpr double STARTUP_GRACE_PERIOD = 10.0;
    static constexpr double DEFAULT_TIMEOUT = 30.0;
    static constexpr double UE5_TIMEOUT = 120.0;
    static constexpr double DLSS_FG_TIMEOUT = 120.0;

    std::thread watchdogThread_;
    FreezeCallback freezeCallback_;
    std::string processName_;
    DWORD processId_;

    // Pre-loaded DbgHelp resources
    HMODULE hDbgHelp_{nullptr};
    decltype(MiniDumpWriteDump)* pMiniDumpWriteDump_{nullptr};
    std::once_flag dbgHelpInitOnce_;
    bool dbgHelpInitialized_{false};
};

// Global watchdog instance for render thread monitoring
extern FreezeWatchdog g_RenderWatchdog;
