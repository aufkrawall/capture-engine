#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <windows.h>
#include <dbghelp.h>

// Freeze detection watchdog - monitors thread heartbeats and creates dumps on freeze
// Design principles:
// 1. Pre-load DbgHelp.dll to avoid loader lock during crash
// 2. Capture actual thread context on freeze for meaningful stack traces
// 3. Proper thread lifecycle with join timeout (no detached threads)
// 4. Dynamic timeout based on game engine detection (UE5, DLSS FG, etc.)
class FreezeWatchdog {
public:
    using FreezeCallback = std::function<void(const std::string &reason)>;

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

    // Set a custom callback for freeze handling (optional)
    void SetFreezeCallback(FreezeCallback callback);

    // Manual freeze check - returns true if frozen
    bool IsFrozen() const;

    // Get the monitored thread ID (set via SetMonitoredThread)
    DWORD GetMonitoredThreadId() const { return monitoredThreadId_.load(); }
    
    // Set the thread to monitor (call from the monitored thread)
    void SetMonitoredThread(DWORD threadId = GetCurrentThreadId()) {
        monitoredThreadId_.store(threadId);
    }

    // Check game engine features that affect timeout
    bool IsDLSSFGActive() const;
    bool IsUE5Active() const;
    
    // Get recommended timeout based on detected game engine
    double GetRecommendedTimeout() const;

private:
    void WatchdogThread();
    void CreateMinidumpWithThreadContext(const std::string &reason);
    void TerminateProcessSafely();
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
    bool dbgHelpInitialized_{false};
};

// Global watchdog instance for render thread monitoring
extern FreezeWatchdog g_RenderWatchdog;
