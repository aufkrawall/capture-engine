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

    FreezeWatchdog();
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

    // Get the monitored thread ID (set via SetMonitoredThread)
    DWORD GetMonitoredThreadId() const {
        return monitoredThreadId_.load();
    }

    // Set the thread to monitor (call from the monitored thread)
    void SetMonitoredThread(DWORD threadId = GetCurrentThreadId()) {
        monitoredThreadId_.store(threadId);
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
    void WatchdogThread();
    void CreateMinidumpWithThreadContext(const std::string& reason, DWORD preferredThreadId = 0);
    bool InitializeDbgHelp();

    // Capture context from frozen thread for meaningful dump
    bool CaptureThreadContext(DWORD threadId, CONTEXT& ctx);

    // Create a synthetic exception record for freeze dump
    void CreateFreezeExceptionRecord(EXCEPTION_RECORD& record, const std::string& reason);

    std::atomic<bool> running_{false};
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
