#include "freeze_watchdog.h"
#include <dbghelp.h>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <filesystem>

// Global watchdog instance
FreezeWatchdog g_RenderWatchdog;

// Helper to get the capture engine logs directory
static std::string GetLogsDirectory() {
    // Try to find the hook DLL path
    char dllPath[MAX_PATH];
    HMODULE hModule = nullptr;
    
    // Get the module handle for this DLL (freeze_watchdog is in the hook DLL)
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | 
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&GetLogsDirectory, &hModule)) {
        if (GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
            // Path is like: ...\installed\captureengine\capture_hook_x64.dll
            std::filesystem::path hookPath(dllPath);
            std::filesystem::path captureEngineDir = hookPath.parent_path();
            std::filesystem::path logsDir = captureEngineDir / "logs";
            return logsDir.string();
        }
    }
    
    // Fallback: use temp directory
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath)) {
        return std::string(tempPath);
    }
    
    // Last resort: current directory
    return ".";
}

// Helper to get current timestamp in microseconds
static uint64_t GetCurrentMicros() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
}

FreezeWatchdog::FreezeWatchdog() : processId_(GetCurrentProcessId()) {
    // Get process name
    char name[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, name, MAX_PATH);
    processName_ = std::filesystem::path(name).filename().string();
}

FreezeWatchdog::~FreezeWatchdog() {
    Stop();
}

bool FreezeWatchdog::Start(double timeoutSeconds) {
    if (running_.exchange(true)) {
        return false;  // Already running
    }
    
    timeoutSeconds_.store(timeoutSeconds);
    uint64_t now = GetCurrentMicros();
    lastHeartbeat_.store(now);
    startupTime_.store(now);
    
    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), 
        "[FreezeWatchdog] Starting with timeout=%.1f seconds (grace period=%.1f seconds)\n", 
        timeoutSeconds, STARTUP_GRACE_PERIOD);
    OutputDebugStringA(logMsg);
    
    watchdogThread_ = std::thread(&FreezeWatchdog::WatchdogThread, this);
    
    // Detach the thread so it can run independently
    // This is critical - the watchdog must survive process termination attempts
    watchdogThread_.detach();
    
    OutputDebugStringA("[FreezeWatchdog] Watchdog thread detached and running\n");
    
    return true;
}

void FreezeWatchdog::Stop() {
    running_.store(false);
    // Thread is detached, no need to join
}

void FreezeWatchdog::Heartbeat() {
    lastHeartbeat_.store(GetCurrentMicros());
}

void FreezeWatchdog::SetFreezeCallback(FreezeCallback callback) {
    freezeCallback_ = callback;
}

bool FreezeWatchdog::IsFrozen() const {
    uint64_t last = lastHeartbeat_.load();
    uint64_t now = GetCurrentMicros();
    double elapsed = (now - last) / 1'000'000.0;  // Convert to seconds
    
    // Check if we're still in the startup grace period
    double sinceStartup = (now - startupTime_.load()) / 1'000'000.0;
    if (sinceStartup < STARTUP_GRACE_PERIOD) {
        // Still in grace period, don't report as frozen
        // Just update the heartbeat to give more time after grace period ends
        return false;
    }
    
    return elapsed > timeoutSeconds_.load();
}

void FreezeWatchdog::WatchdogThread() {
    // Check interval - how often we check for freeze
    const auto checkInterval = std::chrono::milliseconds(500);
    int checkCount = 0;
    
    OutputDebugStringA("[FreezeWatchdog] Watchdog thread started\n");
    
    while (running_.load()) {
        std::this_thread::sleep_for(checkInterval);
        
        if (!running_.load()) {
            OutputDebugStringA("[FreezeWatchdog] Watchdog stopping (running=false)\n");
            break;
        }
        
        // Log first few checks to confirm watchdog is running
        if (++checkCount <= 5) {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), 
                "[FreezeWatchdog] Check #%d, lastHeartbeat=%llu\n", 
                checkCount, lastHeartbeat_.load());
            OutputDebugStringA(logMsg);
        }
        
        if (IsFrozen()) {
            // Freeze detected!
            double elapsed = (GetCurrentMicros() - lastHeartbeat_.load()) / 1'000'000.0;
            std::string reason = "Render thread frozen for " + 
                std::to_string(static_cast<int>(elapsed)) + " seconds";
            
            OutputDebugStringA("[FreezeWatchdog] FREEZE DETECTED!\n");
            OutputDebugStringA(reason.c_str());
            OutputDebugStringA("\n");
            
            // Call custom callback if set
            if (freezeCallback_) {
                freezeCallback_(reason);
            }
            
            // Create crash dump
            CreateMinidump(reason);
            
            OutputDebugStringA("[FreezeWatchdog] Dump created, terminating process...\n");
            
            // Terminate process
            TerminateProcess();
            return;  // Exit watchdog thread (should never reach here)
        }
    }
    
    OutputDebugStringA("[FreezeWatchdog] Watchdog thread exiting\n");
}

void FreezeWatchdog::CreateMinidump(const std::string& reason) {
    OutputDebugStringA("[FreezeWatchdog] Creating minidump...\n");
    
    // Get logs directory (relative to hook DLL location)
    std::string logsDir = GetLogsDirectory();
    
    // Generate dump filename with timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);
    
    std::stringstream ss;
    ss << logsDir << "\\"
       << processName_ << "_FREEZE_"
       << std::put_time(&local_tm, "%Y-%m-%d_%H-%M-%S")
       << ".dmp";
    
    std::string dumpPath = ss.str();
    
    char logMsg[512];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Dump path: %s\n", dumpPath.c_str());
    OutputDebugStringA(logMsg);
    
    // Create the dump
    HANDLE hFile = CreateFileA(
        dumpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Failed to create dump in logs dir, error=%lu\n", err);
        OutputDebugStringA(logMsg);
        
        // Fallback: try temp directory
        char tempPath[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tempPath)) {
            dumpPath = std::string(tempPath) + "\\" + processName_ + "_FREEZE.dmp";
            
            snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Trying temp dir: %s\n", dumpPath.c_str());
            OutputDebugStringA(logMsg);
            
            hFile = CreateFileA(
                dumpPath.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );
        }
    }
    
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Failed to create dump file, error=%lu\n", err);
        OutputDebugStringA(logMsg);
        return;  // Can't create dump
    }
    
    // Write minidump with full memory
    HANDLE hProcess = GetCurrentProcess();
    DWORD processId = GetCurrentProcessId();
    
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = nullptr;  // No exception - it's a freeze
    mei.ClientPointers = FALSE;
    
    // Write the dump
    BOOL success = MiniDumpWriteDump(
        hProcess,
        processId,
        hFile,
        MiniDumpWithFullMemory,  // Full memory dump for debugging
        &mei,
        nullptr,
        nullptr
    );
    
    CloseHandle(hFile);
    
    if (success) {
        char logMsg[512];
        snprintf(logMsg, sizeof(logMsg), 
            "[FreezeWatchdog] SUCCESS: Crash dump created: %s\n",
            dumpPath.c_str());
        OutputDebugStringA(logMsg);
    } else {
        DWORD err = GetLastError();
        char logMsg[256];
        snprintf(logMsg, sizeof(logMsg), 
            "[FreezeWatchdog] FAILED to write dump, error=%lu\n", err);
        OutputDebugStringA(logMsg);
    }
}

void FreezeWatchdog::TerminateProcess() {
    OutputDebugStringA("[FreezeWatchdog] Terminating process due to freeze\n");
    
    // CRITICAL: Use TerminateProcess directly, NOT ExitProcess
    // ExitProcess runs DLL cleanup which can deadlock with our mutex
    ::TerminateProcess(GetCurrentProcess(), 0xDEAD);
    
    // If we're still here, something is very wrong
    // Last resort: try to crash the process
    __fastfail(0xDEAD);
}
