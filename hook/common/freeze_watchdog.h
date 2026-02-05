#pragma once

#include <windows.h>
#include <atomic>
#include <thread>
#include <string>
#include <functional>

// Freeze detection watchdog - monitors thread heartbeats and creates dumps on freeze
class FreezeWatchdog {
public:
    // Callback type for custom freeze handling
    using FreezeCallback = std::function<void(const std::string& reason)>;
    
    FreezeWatchdog();
    ~FreezeWatchdog();
    
    // Start monitoring with specified timeout (seconds)
    bool Start(double timeoutSeconds = 5.0);
    void Stop();
    
    // Call this from the monitored thread to indicate it's alive
    void Heartbeat();
    
    // Set a custom callback for freeze handling (optional)
    void SetFreezeCallback(FreezeCallback callback);
    
    // Manual freeze check - returns true if frozen
    bool IsFrozen() const;
    
    // Check if this is a DLSS FG game (needs longer timeout)
    bool IsDLSSFGActive() const;
    
    // Adjust timeout based on game type
    void AdjustTimeoutForGameType();
    
private:
    void WatchdogThread();
    void CreateMinidump(const std::string& reason);
    void TerminateProcess();
    
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastHeartbeat_{0};  // Microseconds timestamp
    std::atomic<uint64_t> startupTime_{0};    // When watchdog started (microseconds)
    std::atomic<double> timeoutSeconds_{5.0};
    static constexpr double STARTUP_GRACE_PERIOD = 10.0;  // 10 seconds grace period
    std::thread watchdogThread_;
    FreezeCallback freezeCallback_;
    std::string processName_;
    DWORD processId_;
};

// Global watchdog instance for render thread monitoring
extern FreezeWatchdog g_RenderWatchdog;
