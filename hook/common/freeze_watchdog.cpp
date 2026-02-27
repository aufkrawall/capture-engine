#include "freeze_watchdog.h"
#include <tlhelp32.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include "hook_common.h"

FreezeWatchdog g_RenderWatchdog;

static std::string GetLogsDirectory() {
    char dllPath[MAX_PATH];
    HMODULE hModule = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&GetLogsDirectory, &hModule)) {
        if (GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
            std::filesystem::path hookPath(dllPath);
            std::filesystem::path captureEngineDir = hookPath.parent_path();
            std::filesystem::path logsDir = captureEngineDir / "logs";
            return logsDir.string();
        }
    }

    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath)) {
        return std::string(tempPath);
    }

    return ".";
}

static uint64_t GetCurrentMicros() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

static bool IsProcessInForeground(DWORD processId) {
    HWND fgWindow = GetForegroundWindow();
    if (!fgWindow) {
        return false;
    }

    DWORD fgPid = 0;
    GetWindowThreadProcessId(fgWindow, &fgPid);
    return fgPid == processId;
}

FreezeWatchdog::FreezeWatchdog() : processId_(GetCurrentProcessId()) {
    char name[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, name, MAX_PATH);
    processName_ = std::filesystem::path(name).filename().string();
    InitializeDbgHelp();
}

FreezeWatchdog::~FreezeWatchdog() {
    Stop();
    if (hDbgHelp_) {
        FreeLibrary(hDbgHelp_);
        hDbgHelp_ = nullptr;
        pMiniDumpWriteDump_ = nullptr;
    }
}

bool FreezeWatchdog::InitializeDbgHelp() {
    std::call_once(dbgHelpInitOnce_, [this]() {
        hDbgHelp_ = LoadLibraryA("dbghelp.dll");
        if (!hDbgHelp_) {
            OutputDebugStringA("[FreezeWatchdog] Failed to load dbghelp.dll\n");
            dbgHelpInitialized_ = true;
            return;
        }

        pMiniDumpWriteDump_ =
            reinterpret_cast<decltype(MiniDumpWriteDump)*>(GetProcAddress(hDbgHelp_, "MiniDumpWriteDump"));

        if (!pMiniDumpWriteDump_) {
            OutputDebugStringA("[FreezeWatchdog] Failed to get MiniDumpWriteDump\n");
            FreeLibrary(hDbgHelp_);
            hDbgHelp_ = nullptr;
        } else {
            OutputDebugStringA("[FreezeWatchdog] DbgHelp initialized successfully\n");
        }

        dbgHelpInitialized_ = true;
    });
    return pMiniDumpWriteDump_ != nullptr;
}

bool FreezeWatchdog::Start(double timeoutSeconds) {
    if (running_.exchange(true)) {
        return false;
    }

    if (!dbgHelpInitialized_) {
        InitializeDbgHelp();
    }

    double finalTimeout = timeoutSeconds;

    if (IsUE5Active()) {
        finalTimeout = UE5_TIMEOUT;
        OutputDebugStringA("[FreezeWatchdog] UE5 detected, using extended timeout\n");
    } else if (IsDLSSFGActive()) {
        finalTimeout = DLSS_FG_TIMEOUT;
        OutputDebugStringA("[FreezeWatchdog] DLSS FG detected, using extended timeout\n");
    }

    timeoutSeconds_.store(finalTimeout);
    uint64_t now = GetCurrentMicros();
    lastHeartbeat_.store(now);
    startupTime_.store(now);

    char logMsg[256];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Starting: timeout=%.1fs, grace=%.1fs\n", finalTimeout,
             STARTUP_GRACE_PERIOD);
    OutputDebugStringA(logMsg);

    watchdogThread_ = std::thread(&FreezeWatchdog::WatchdogThread, this);

    return true;
}

void FreezeWatchdog::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (watchdogThread_.joinable()) {
        constexpr auto kJoinTimeout = std::chrono::milliseconds(2000);
        auto deadline = std::chrono::steady_clock::now() + kJoinTimeout;

        while (watchdogThread_.joinable() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (watchdogThread_.joinable()) {
            OutputDebugStringA("[FreezeWatchdog] Warning: Thread didn't exit in time\n");
            watchdogThread_.detach();
        }
    }
}

void FreezeWatchdog::Heartbeat() {
    lastHeartbeat_.store(GetCurrentMicros());
}

void FreezeWatchdog::SetFreezeCallback(FreezeCallback callback) {
    freezeCallback_ = std::move(callback);
}

bool FreezeWatchdog::IsFrozen() const {
    uint64_t last = lastHeartbeat_.load();
    uint64_t now = GetCurrentMicros();
    double elapsed = (now - last) / 1'000'000.0;

    double sinceStartup = (now - startupTime_.load()) / 1'000'000.0;
    if (sinceStartup < STARTUP_GRACE_PERIOD) {
        return false;
    }

    // Vulkan / DXVK paths can pause or bypass the DXGI/D3D heartbeat patterns
    // used by this watchdog. Skip freeze assertions in those cases.
    if (GetModuleHandleW(L"vulkan-1.dll") || GetModuleHandleW(L"winevulkan.dll")) {
        return false;
    }

    // Alt+Tab/minimized games can legitimately stop presenting for a while.
    // Only treat missing heartbeats as a freeze while the game is foreground.
    if (!IsProcessInForeground(processId_)) {
        return false;
    }

    return elapsed > timeoutSeconds_.load();
}

bool FreezeWatchdog::IsDLSSFGActive() const {
    if (GetModuleHandleW(L"nvngx_dlssg.dll")) {
        return true;
    }
    return false;
}

bool FreezeWatchdog::IsUE5Active() const {
    // Check for common UE5 DLLs
    if (GetModuleHandleW(L"ue5.dll") || GetModuleHandleW(L"UnrealEditor.dll")) {
        return true;
    }

    // Check for UE5 engine patterns in loaded modules
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me;
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnapshot, &me)) {
            do {
                std::wstring moduleName = me.szModule;
                std::transform(moduleName.begin(), moduleName.end(), moduleName.begin(), ::towlower);
                if (moduleName.find(L"unreal") != std::wstring::npos || moduleName.find(L"ue4") != std::wstring::npos ||
                    moduleName.find(L"ue5") != std::wstring::npos) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            } while (Module32NextW(hSnapshot, &me));
        }
        CloseHandle(hSnapshot);
    }

    return false;
}

double FreezeWatchdog::GetRecommendedTimeout() const {
    if (IsUE5Active())
        return UE5_TIMEOUT;
    if (IsDLSSFGActive())
        return DLSS_FG_TIMEOUT;
    return DEFAULT_TIMEOUT;
}

bool FreezeWatchdog::CaptureThreadContext(DWORD threadId, CONTEXT& ctx) {
    HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, threadId);
    if (!hThread) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[FreezeWatchdog] Failed to open thread %lu, error=%lu\n", threadId, GetLastError());
        OutputDebugStringA(msg);
        return false;
    }

    DWORD suspendResult = SuspendThread(hThread);
    if (suspendResult == (DWORD)-1) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[FreezeWatchdog] Failed to suspend thread %lu\n", threadId);
        OutputDebugStringA(msg);
        CloseHandle(hThread);
        return false;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;

    BOOL gotContext = GetThreadContext(hThread, &ctx);
    ResumeThread(hThread);
    CloseHandle(hThread);

    if (!gotContext) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[FreezeWatchdog] Failed to get context for thread %lu\n", threadId);
        OutputDebugStringA(msg);
        return false;
    }

    OutputDebugStringA("[FreezeWatchdog] Successfully captured thread context\n");
    return true;
}

void FreezeWatchdog::CreateFreezeExceptionRecord(EXCEPTION_RECORD& record, const std::string& reason) {
    memset(&record, 0, sizeof(record));
    record.ExceptionCode = 0xE0000001;  // Custom freeze detection code
    record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = nullptr;

    // Store first 14 chars of reason in exception information for debugging
    uintptr_t reasonInfo[EXCEPTION_MAXIMUM_PARAMETERS] = {0};
    size_t copyLen = std::min(reason.size(), sizeof(reasonInfo) - 1);
    memcpy(reasonInfo, reason.c_str(), copyLen);

    record.NumberParameters = 1;
    record.ExceptionInformation[0] = reasonInfo[0];
}

void FreezeWatchdog::WatchdogThread() {
    const auto checkInterval = std::chrono::milliseconds(500);
    uint64_t lastLogTime = 0;

    OutputDebugStringA("[FreezeWatchdog] Watchdog thread started\n");

    while (running_.load(std::memory_order_acquire) && !HookIsShuttingDown()) {
        std::this_thread::sleep_for(checkInterval);

        if (!running_.load(std::memory_order_acquire)) {
            OutputDebugStringA("[FreezeWatchdog] Watchdog stopping\n");
            break;
        }

        uint64_t now = GetCurrentMicros();
        if (now - lastLogTime > 10'000'000) {
            lastLogTime = now;
            uint64_t lastBeat = lastHeartbeat_.load();
            double elapsed = (now - lastBeat) / 1'000'000.0;
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Status: elapsed=%.1fs, timeout=%.1fs\n", elapsed,
                     timeoutSeconds_.load());
            OutputDebugStringA(logMsg);
        }

        if (IsFrozen()) {
            double elapsed = (GetCurrentMicros() - lastHeartbeat_.load()) / 1'000'000.0;
            std::string reason = "Render thread frozen for " + std::to_string(static_cast<int>(elapsed)) + " seconds";

            OutputDebugStringA("[FreezeWatchdog] FREEZE DETECTED!\n");
            OutputDebugStringA(reason.c_str());
            OutputDebugStringA("\n");

            if (freezeCallback_) {
                freezeCallback_(reason);
            }

            CreateMinidumpWithThreadContext(reason);

            // Do NOT terminate the process: the game may recover on its own,
            // and forcefully killing it loses unsaved data and prevents the
            // game's own crash-handling from running.  The minidump and the
            // callback (which can notify the host) are sufficient action.
            OutputDebugStringA("[FreezeWatchdog] Dump created. Not terminating - watchdog stopping.\n");
            return;
        }
    }

    OutputDebugStringA("[FreezeWatchdog] Watchdog thread exiting\n");
}

void FreezeWatchdog::CreateMinidumpWithThreadContext(const std::string& reason) {
    OutputDebugStringA("[FreezeWatchdog] Creating minidump with thread context...\n");

    if (!pMiniDumpWriteDump_) {
        OutputDebugStringA("[FreezeWatchdog] MiniDumpWriteDump not available\n");
        return;
    }

    std::string logsDir = GetLogsDirectory();
    std::filesystem::path logPath(logsDir);
    if (!std::filesystem::exists(logPath)) {
        std::filesystem::create_directories(logPath);
    }

    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);

    std::stringstream ss;
    ss << logsDir << "\\" << processName_ << "_FREEZE_" << std::put_time(&local_tm, "%Y-%m-%d_%H-%M-%S") << ".dmp";
    std::string dumpPath = ss.str();

    char logMsg[512];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Dump path: %s\n", dumpPath.c_str());
    OutputDebugStringA(logMsg);

    HANDLE hFile =
        CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Failed to create dump file, error=%lu\n", err);
        OutputDebugStringA(logMsg);

        char tempPath[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tempPath)) {
            dumpPath = std::string(tempPath) + "\\" + processName_ + "_FREEZE.dmp";
            hFile =
                CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE hProcess = GetCurrentProcess();
    DWORD processId = GetCurrentProcessId();

    // Try to capture context from monitored thread
    CONTEXT threadCtx = {};
    EXCEPTION_RECORD exceptionRecord = {};
    EXCEPTION_POINTERS exceptionPointers = {&exceptionRecord, &threadCtx};

    DWORD monitoredTid = monitoredThreadId_.load();
    bool hasContext = false;

    if (monitoredTid != 0) {
        hasContext = CaptureThreadContext(monitoredTid, threadCtx);
        if (hasContext) {
            CreateFreezeExceptionRecord(exceptionRecord, reason);
        }
    }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = monitoredTid ? monitoredTid : GetCurrentThreadId();
    mei.ExceptionPointers = hasContext ? &exceptionPointers : nullptr;
    mei.ClientPointers = FALSE;

    // Include thread info and handle data for debugging freezes
    MINIDUMP_TYPE dumpType =
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                   MiniDumpWithIndirectlyReferencedMemory);

    BOOL success = pMiniDumpWriteDump_(hProcess, processId, hFile, dumpType, mei.ExceptionPointers ? &mei : nullptr,
                                       nullptr, nullptr);

    CloseHandle(hFile);

    if (success) {
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] SUCCESS: Dump created: %s\n", dumpPath.c_str());
        OutputDebugStringA(logMsg);
    } else {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] FAILED to write dump, error=%lu\n", err);
        OutputDebugStringA(logMsg);
    }
}

void FreezeWatchdog::TerminateProcessSafely() {
    OutputDebugStringA("[FreezeWatchdog] Terminating process due to freeze\n");

    ::TerminateProcess(GetCurrentProcess(), 0xDEAD);
    __fastfail(0xDEAD);
}
