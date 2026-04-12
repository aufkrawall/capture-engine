#include "freeze_watchdog.h"
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include "dxgi_shared.h"
#include "fg_detection.h"
#include "hook_common.h"

FreezeWatchdog g_RenderWatchdog;

static std::string GetLogsDirectory() {
    char pathBuffer[MAX_PATH] = {};
    if (BuildLogFilePathForModuleAddress((const void*)&GetLogsDirectory, "freeze_watchdog.tmp", pathBuffer,
                                         sizeof(pathBuffer))) {
        return std::filesystem::path(pathBuffer).parent_path().string();
    }

    return ".\\logs";
}

static uint64_t GetCurrentMicros() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

static bool ShouldAllowImmediateDumpRequest(std::atomic<uint64_t>& lastDumpRequestMicros, uint64_t nowMicros,
                                           uint64_t dedupWindowMicros = 10'000'000) {
    uint64_t previousRequest = lastDumpRequestMicros.load(std::memory_order_acquire);
    while (true) {
        if (previousRequest != 0 && nowMicros > previousRequest && (nowMicros - previousRequest) < dedupWindowMicros) {
            return false;
        }
        if (lastDumpRequestMicros.compare_exchange_weak(previousRequest, nowMicros, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            return true;
        }
    }
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

namespace {
struct DialogInfo {
    HWND hwnd = nullptr;
    DWORD threadId = 0;
    bool visible = false;
    char title[256] = {};
};

struct DialogSearchContext {
    DWORD processId = 0;
    DialogInfo* info = nullptr;
};

BOOL CALLBACK FindProcessDialogWindowProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<DialogSearchContext*>(lParam);
    if (!context || !context->info) {
        return TRUE;
    }

    DWORD windowProcessId = 0;
    const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId != context->processId) {
        return TRUE;
    }

    char className[64] = {};
    if (!GetClassNameA(hwnd, className, static_cast<int>(sizeof(className)))) {
        return TRUE;
    }

    if (_stricmp(className, "#32770") != 0) {
        return TRUE;
    }

    context->info->hwnd = hwnd;
    context->info->threadId = windowThreadId;
    context->info->visible = IsWindowVisible(hwnd) != FALSE;
    GetWindowTextA(hwnd, context->info->title, static_cast<int>(sizeof(context->info->title)));
    return FALSE;
}

bool FindBlockingDialogWindow(DWORD processId, DialogInfo& info) {
    DialogSearchContext context = {};
    context.processId = processId;
    context.info = &info;
    EnumWindows(FindProcessDialogWindowProc, reinterpret_cast<LPARAM>(&context));
    return info.hwnd != nullptr;
}

std::string DescribeDialog(const DialogInfo& info) {
    std::string description = info.visible ? "visible dialog" : "hidden dialog";
    if (info.title[0] != '\0') {
        description += " '";
        description += info.title;
        description += "'";
    }
    return description;
}

std::string GetDialogIdentity(const DialogInfo& info) {
    if (info.title[0] != '\0') {
        return info.title;
    }
    return "#32770";
}
}  // namespace

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

    const bool ue5Active = IsUE5Active();
    const bool dlssFGActive = IsDLSSFGActive();

    if (ue5Active) {
        finalTimeout = UE5_TIMEOUT;
        OutputDebugStringA("[FreezeWatchdog] UE5 detected, using extended timeout\n");
    } else if (dlssFGActive) {
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
    HookLogImportant("FreezeWatchdog: Started (timeout=%.1fs, monitoredTid=%lu, ue5=%d, dlssFg=%d)", finalTimeout,
                     monitoredThreadId_.load(std::memory_order_acquire), ue5Active ? 1 : 0, dlssFGActive ? 1 : 0);

    watchdogThread_ = std::thread(&FreezeWatchdog::WatchdogThread, this);

    return true;
}

void FreezeWatchdog::Stop() {
    if (!running_.exchange(false)) {
        return;
    }

    if (watchdogThread_.joinable()) {
        watchdogThread_.join();
    }
}

void FreezeWatchdog::Heartbeat() {
    const DWORD heartbeatTid = GetCurrentThreadId();
    const DWORD monitoredTid = monitoredThreadId_.load(std::memory_order_acquire);
    if (monitoredTid != 0 && monitoredTid != heartbeatTid) {
        static std::atomic<int> s_threadSwitchLogCount{0};
        if (s_threadSwitchLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("FreezeWatchdog: Helper heartbeat from tid=%lu while monitoring tid=%lu", heartbeatTid,
                    monitoredTid);
        }
    }
    lastHeartbeat_.store(GetCurrentMicros(), std::memory_order_release);
}

void FreezeWatchdog::HeartbeatFromHelperThread() {
    lastHeartbeat_.store(GetCurrentMicros(), std::memory_order_release);
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
    // used by this watchdog. Skip freeze assertions in those cases — BUT only
    // when D3D12 is NOT also loaded (UE5 loads vulkan-1.dll even for DX12).
    if ((GetModuleHandleW(L"vulkan-1.dll") || GetModuleHandleW(L"winevulkan.dll")) && !GetModuleHandleW(L"d3d12.dll")) {
        return false;
    }

    bool inPresentCall = DXGIShared::g_SharedState.presentInFlightDepth.load(std::memory_order_acquire) > 0;

    // Alt+Tab/minimized games can legitimately stop presenting for a while.
    // Suppress background freezes only when no Present is in flight. If a
    // Present call is already in progress and heartbeats stop, it's likely a
    // real driver/render hang and should still produce a dump.
    // When forceMonitor_ is set (device removed), always check — the GPU
    // driver may be stuck in a kernel call even though the window is unfocused.
    if (!IsProcessInForeground(processId_) && !forceMonitor_.load(std::memory_order_relaxed)) {
        if (!inPresentCall) {
            return false;
        }
        double inFlightTimeout = timeoutSeconds_.load();
        if (inFlightTimeout > 15.0) {
            inFlightTimeout = 15.0;
        }
        return elapsed > inFlightTimeout;
    }

    return elapsed > timeoutSeconds_.load();
}

bool FreezeWatchdog::IsDLSSFGActive() const {
    return g_FGCompat.IsDLSSFGApiActive() || g_FGCompat.GetActiveFGType() == FGCompatibility::FGType::DLSS_FG;
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

void FreezeWatchdog::RequestImmediateDump(const std::string& reason, DWORD preferredThreadId) {
    if (!running_.load(std::memory_order_acquire) || reason.empty()) {
        return;
    }

    const uint64_t nowMicros = GetCurrentMicros();
    if (!ShouldAllowImmediateDumpRequest(lastDumpRequestMicros_, nowMicros)) {
        HookLog("FreezeWatchdog: Suppressing duplicate immediate dump request for '%s'", reason.c_str());
        return;
    }

    DWORD targetTid = preferredThreadId;
    if (targetTid == 0) {
        if (FreezeWatchdog::PreferredThreadProvider provider =
                preferredThreadProvider_.load(std::memory_order_acquire)) {
            const DWORD preferredRenderTid = provider();
            if (preferredRenderTid != 0) {
                targetTid = preferredRenderTid;
            }
        }
    }
    if (targetTid == 0) {
        targetTid = monitoredThreadId_.load(std::memory_order_acquire);
    }

    HookLogImportant("FreezeWatchdog: Immediate dump requested (%s, targetTid=%lu)", reason.c_str(), targetTid);
    if (freezeCallback_) {
        freezeCallback_(reason);
    }

    if (ce::freeze_watchdog_policy::ShouldDeferImmediateDumpToWatchdogThread(GetCurrentThreadId(), targetTid)) {
        {
            std::lock_guard<std::mutex> lock(pendingImmediateDumpMutex_);
            pendingImmediateDumpReason_ = reason;
            pendingImmediateDumpTargetTid_ = targetTid;
        }
        pendingImmediateDump_.store(true, std::memory_order_release);
        HookLogImportant(
            "FreezeWatchdog: Deferring immediate dump to watchdog thread because caller matches targetTid=%lu",
            targetTid);
        return;
    }

    CreateMinidumpWithThreadContext(reason, targetTid);
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
    constexpr double kDialogDumpDelaySeconds = 5.0;
    constexpr double kDialogStartupWindowSeconds = 90.0;
    constexpr uint64_t kDialogDumpDedupWindowMicros = 10'000'000;
    uint64_t lastLogTime = 0;
    uint64_t dialogSeenSince = 0;
    DWORD dialogThreadId = 0;
    std::string dialogDescription;
    std::string dialogIdentity;
    bool dialogDumpWritten = false;
    bool freezeDumpSuppressionLogged = false;
    uint64_t lastDialogDumpTime = 0;
    std::string lastDialogDumpIdentity;

    OutputDebugStringA("[FreezeWatchdog] Watchdog thread started\n");

    while (running_.load(std::memory_order_acquire) && !HookIsShuttingDown()) {
        std::this_thread::sleep_for(checkInterval);

        if (!running_.load(std::memory_order_acquire)) {
            OutputDebugStringA("[FreezeWatchdog] Watchdog stopping\n");
            break;
        }

        if (pendingImmediateDump_.exchange(false, std::memory_order_acq_rel)) {
            std::string pendingReason;
            DWORD pendingTargetTid = 0;
            {
                std::lock_guard<std::mutex> lock(pendingImmediateDumpMutex_);
                pendingReason = pendingImmediateDumpReason_;
                pendingTargetTid = pendingImmediateDumpTargetTid_;
                pendingImmediateDumpReason_.clear();
                pendingImmediateDumpTargetTid_ = 0;
            }

            if (!pendingReason.empty()) {
                HookLogImportant(
                    "FreezeWatchdog: Processing deferred immediate dump request (%s, targetTid=%lu)",
                    pendingReason.c_str(), pendingTargetTid);
                CreateMinidumpWithThreadContext(pendingReason, pendingTargetTid);
                continue;
            }
        }

        uint64_t now = GetCurrentMicros();
        uint64_t lastBeat = lastHeartbeat_.load(std::memory_order_acquire);
        double elapsed = (now - lastBeat) / 1'000'000.0;
        double sinceStartup = (now - startupTime_.load(std::memory_order_acquire)) / 1'000'000.0;
        DialogInfo dialogInfo = {};
        bool hasDialog = FindBlockingDialogWindow(processId_, dialogInfo);
        if (hasDialog) {
            std::string currentDialogDescription = DescribeDialog(dialogInfo);
            std::string currentDialogIdentity = GetDialogIdentity(dialogInfo);
            if (dialogSeenSince == 0 || dialogIdentity != currentDialogIdentity) {
                dialogSeenSince = now;
                dialogThreadId = dialogInfo.threadId;
                dialogDescription = currentDialogDescription;
                dialogIdentity = currentDialogIdentity;
                dialogDumpWritten = false;
                freezeDumpSuppressionLogged = false;
                HookLogImportant("FreezeWatchdog: Detected %s (hwnd=%p tid=%lu)", dialogDescription.c_str(),
                                 dialogInfo.hwnd, dialogThreadId);
            } else if (dialogThreadId != dialogInfo.threadId || dialogDescription != currentDialogDescription) {
                HookLog("FreezeWatchdog: Dialog state changed to %s (hwnd=%p tid=%lu)",
                        currentDialogDescription.c_str(), dialogInfo.hwnd, dialogInfo.threadId);
                dialogThreadId = dialogInfo.threadId;
                dialogDescription = currentDialogDescription;
            }
        } else if (dialogSeenSince != 0) {
            HookLog("FreezeWatchdog: Blocking dialog cleared");
            dialogSeenSince = 0;
            dialogThreadId = 0;
            dialogDescription.clear();
            dialogIdentity.clear();
            dialogDumpWritten = false;
            freezeDumpSuppressionLogged = false;
        }

        if (now - lastLogTime > 10'000'000) {
            lastLogTime = now;
            char logMsg[256];
            snprintf(logMsg, sizeof(logMsg),
                     "[FreezeWatchdog] Status: elapsed=%.1fs, timeout=%.1fs, monitoredTid=%lu, dialogTid=%lu\n",
                     elapsed, timeoutSeconds_.load(), monitoredThreadId_.load(std::memory_order_acquire),
                     dialogThreadId);
            OutputDebugStringA(logMsg);
            HookLog("FreezeWatchdog: Status elapsed=%.1fs timeout=%.1fs monitoredTid=%lu dialogTid=%lu", elapsed,
                    timeoutSeconds_.load(), monitoredThreadId_.load(std::memory_order_acquire), dialogThreadId);
        }

        if (dialogSeenSince != 0 && !dialogDumpWritten) {
            double dialogElapsed = (now - dialogSeenSince) / 1'000'000.0;
            const bool isErrGfxStateDialog = dialogDescription.find("ERR_GFX_STATE") != std::string::npos;
            const double requiredDialogDumpDelay = isErrGfxStateDialog ? 0.0 : kDialogDumpDelaySeconds;
            const bool withinDialogDumpWindow = sinceStartup <= kDialogStartupWindowSeconds || isErrGfxStateDialog;
            if (dialogElapsed >= requiredDialogDumpDelay && withinDialogDumpWindow) {
                if (!dialogIdentity.empty() && dialogIdentity == lastDialogDumpIdentity &&
                    (now - lastDialogDumpTime) < kDialogDumpDedupWindowMicros) {
                    HookLogImportant(
                        "FreezeWatchdog: Suppressing duplicate dialog dump for %s because a dump was already captured "
                        "%.1fs ago",
                        dialogDescription.c_str(), (now - lastDialogDumpTime) / 1'000'000.0);
                    dialogDumpWritten = true;
                    continue;
                }

                std::string reason = dialogDescription.empty() ? "Blocking dialog detected"
                                                               : ("Blocking " + dialogDescription + " detected");
                if (isErrGfxStateDialog) {
                    HookLogImportant("FreezeWatchdog: Critical dialog %s detected - capturing dump immediately",
                                     dialogDescription.c_str());
                } else {
                    HookLogImportant("FreezeWatchdog: Persistent dialog detected after %.1fs - capturing dump",
                                     dialogElapsed);
                }
                RequestImmediateDump(reason, dialogThreadId);
                dialogDumpWritten = true;
                lastDialogDumpTime = now;
                lastDialogDumpIdentity = dialogIdentity;
            }
        }

        if (IsFrozen()) {
            if (dialogSeenSince != 0 && dialogDumpWritten) {
                if (!freezeDumpSuppressionLogged) {
                    HookLogImportant(
                        "FreezeWatchdog: Suppressing redundant freeze dump because a dialog dump was already captured "
                        "for %s",
                        dialogDescription.c_str());
                    freezeDumpSuppressionLogged = true;
                }
                continue;
            }
            freezeDumpSuppressionLogged = false;

            std::string reason = "Render thread frozen for " + std::to_string(static_cast<int>(elapsed)) + " seconds";

            OutputDebugStringA("[FreezeWatchdog] FREEZE DETECTED!\n");
            OutputDebugStringA(reason.c_str());
            OutputDebugStringA("\n");
            HookLogImportant("FreezeWatchdog: Freeze detected (%s)", reason.c_str());

            RequestImmediateDump(reason);

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

void FreezeWatchdog::CreateMinidumpWithThreadContext(const std::string& reason, DWORD preferredThreadId) {
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
    const auto totalMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto millisecondPart = static_cast<int>(totalMilliseconds % 1000);
    std::tm local_tm;
    localtime_s(&local_tm, &time_t_now);

    std::stringstream ss;
    ss << logsDir << "\\" << processName_ << "_FREEZE_" << std::put_time(&local_tm, "%Y-%m-%d_%H-%M-%S") << "_"
       << std::setw(3) << std::setfill('0') << millisecondPart << ".dmp";
    std::string dumpPath = ss.str();

    char logMsg[512];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Dump path: %s\n", dumpPath.c_str());
    OutputDebugStringA(logMsg);
    HookLogImportant("FreezeWatchdog: Writing dump to %s", dumpPath.c_str());

    HANDLE hFile =
        CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Failed to create dump file, error=%lu\n", err);
        OutputDebugStringA(logMsg);
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

    DWORD monitoredTid = preferredThreadId ? preferredThreadId : monitoredThreadId_.load();
    HookLogImportant("FreezeWatchdog: Capturing dump for reason '%s' (targetTid=%lu)", reason.c_str(), monitoredTid);
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
        HookLogImportant("FreezeWatchdog: Dump created at %s", dumpPath.c_str());
    } else {
        DWORD err = GetLastError();
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] FAILED to write dump, error=%lu\n", err);
        OutputDebugStringA(logMsg);
        HookLogImportant("FreezeWatchdog: Dump creation failed at %s (error=%lu)", dumpPath.c_str(), err);
    }
}
