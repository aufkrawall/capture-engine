// Freeze-dump writing for FreezeWatchdog: the thread-context capture, the
// synthetic freeze exception record, and the dump route itself.
//
// Kept apart from the detection half in freeze_watchdog.cpp because the two
// answer different questions - "is this actually frozen?" versus "how do we
// record it without freezing the game ourselves?" - and the file-size ceiling
// wants both under it.

#include "freeze_watchdog.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>

#include "crash_dump_policy.h"
#include "crash_handler.h"
#include "hook_common.h"

// Defined in dx12_hook.cpp. Emits DRED breadcrumbs + page-fault info if the D3D12
// device is removed/hung, so a device-hung freeze dump is accompanied by the exact
// faulting GPU op. No-op for non-DX12 or healthy-device freezes.
extern void DX12_DumpDredIfDeviceRemoved(const char* reason);

// Defined in dx12_hook.cpp. Reads CE's overlay GPU breadcrumb markers and reports the last GPU op CE's
// overlay command list reached — works even for a pure hang (no device removal). Pinpoints whether a
// native-FSR ffxQuery wedge is CE's GPU work stalling the queue or a fence/CPU deadlock after CE's list.
extern void DX12_LogOverlayGpuBreadcrumbs(const char* reason);

static std::string GetLogsDirectory() {
    std::string cached = GetCrashDumpDirectory();
    if (!cached.empty())
        return cached;

    char pathBuffer[MAX_PATH] = {};
    if (BuildLogFilePathForModuleAddress((const void*)&GetLogsDirectory, "freeze_watchdog.tmp", pathBuffer,
                                         sizeof(pathBuffer))) {
        return std::filesystem::path(pathBuffer).parent_path().string();
    }

    return ".\\logs";
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

void FreezeWatchdog::CreateMinidumpWithThreadContext(const std::string& reason, DWORD preferredThreadId) {
    OutputDebugStringA("[FreezeWatchdog] Creating minidump with thread context...\n");

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

    std::stringstream fileNameStream;
    fileNameStream << processName_ << "_FREEZE_" << std::put_time(&local_tm, "%Y-%m-%d_%H-%M-%S") << "_" << std::setw(3)
                   << std::setfill('0') << millisecondPart << ".dmp";
    const std::string dumpFileName = fileNameStream.str();
    const std::filesystem::path dumpPathFs = std::filesystem::path(logsDir) / dumpFileName;
    const std::filesystem::path tempDumpPathFs =
        std::filesystem::path(logsDir) / ce::crash_dump_policy::BuildInProgressDumpFileName(dumpFileName.c_str());
    std::string dumpPath = dumpPathFs.string();
    std::string tempDumpPath = tempDumpPathFs.string();

    char logMsg[512];
    snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] Dump path: %s\n", dumpPath.c_str());
    OutputDebugStringA(logMsg);

    // If this freeze is a removed/hung D3D12 device, record DRED breadcrumbs +
    // page-fault info into the hook log alongside the dump.
    DX12_DumpDredIfDeviceRemoved("freeze watchdog dump");
    // Also read CE's overlay GPU breadcrumbs (works for a pure hang too) so a native-FSR ffxQuery wedge is
    // attributable to CE's overlay GPU op vs a fence/CPU deadlock after CE's list completed.
    DX12_LogOverlayGpuBreadcrumbs("freeze watchdog dump");

    const DWORD dumpTargetTid = preferredThreadId ? preferredThreadId : monitoredThreadId_.load();

    // An in-process dump suspends every thread while dbghelp reads the version
    // resource of every loaded module. Through a foreign overlay's loader and
    // version hooks that walk takes minutes (session 20260817_052857: 61.6 s
    // for a single MiniDumpNormal with the Steam overlay loaded), so a freeze
    // dump written this way freezes the game far harder and longer than
    // whatever it was meant to diagnose — and it does so even when the freeze
    // claim itself was wrong. The external helper writes the same dump from
    // outside without holding this process's threads, so prefer it, exactly
    // like the crash worker does.
    const bool foreignOverlayLoaded = IsForeignOverlayLoadedForCrashDump();
    if (ce::crash_dump_policy::ShouldPreferExternalCrashDumpHelper(foreignOverlayLoaded,
                                                                   HasExternalCrashDumpCapture())) {
        HookLogImportant(
            "FreezeWatchdog: Foreign overlay loaded — capturing the freeze dump with the external helper "
            "(hint=%s targetTid=%lu)",
            dumpFileName.c_str(), dumpTargetTid);
        if (CaptureCrashDumpWithExternalHelper(dumpFileName.c_str())) {
            HookLogImportant("FreezeWatchdog: External helper captured the freeze dump (hint=%s targetTid=%lu)",
                             dumpFileName.c_str(), dumpTargetTid);
            return;
        }
        HookLogImportant("FreezeWatchdog: External helper freeze dump failed (hint=%s)", dumpFileName.c_str());
    }

    if (!ce::crash_dump_policy::ShouldUseInProcessMiniDumpFallbackAfterExternalHelperFailure(foreignOverlayLoaded)) {
        HookLogImportant(
            "FreezeWatchdog: Skipping the in-process freeze dump — dbghelp module enumeration can hang against "
            "foreign overlay hooks and would suspend every thread meanwhile");
        return;
    }

    if (!pMiniDumpWriteDump_) {
        OutputDebugStringA("[FreezeWatchdog] MiniDumpWriteDump not available\n");
        HookLogImportant("FreezeWatchdog: MiniDumpWriteDump unavailable — no freeze dump written");
        return;
    }

    HookLogImportant("FreezeWatchdog: Writing dump to %s via in-progress path %s", dumpPath.c_str(),
                     tempDumpPath.c_str());

    DeleteFileA(tempDumpPath.c_str());
    HANDLE hFile = CreateFileA(tempDumpPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);

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

    const DWORD monitoredTid = dumpTargetTid;
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

    struct DumpAttempt {
        MINIDUMP_TYPE type;
        const char* label;
    };

    const DumpAttempt attempts[] = {
        {ce::crash_dump_policy::kRichFreezeDumpType, "rich-primary"},
        {ce::crash_dump_policy::kCompatibilityFreezeDumpType, "compat-primary"},
        {ce::crash_dump_policy::kMinimalDumpType, "fallback-normal"},
    };

    BOOL success = FALSE;
    DWORD err = ERROR_SUCCESS;
    for (size_t i = 0; i < std::size(attempts); ++i) {
        HookLogImportant("FreezeWatchdog: Dump attempt %zu/%zu (%s)", i + 1, std::size(attempts), attempts[i].label);
        success = pMiniDumpWriteDump_(hProcess, processId, hFile, attempts[i].type,
                                      mei.ExceptionPointers ? &mei : nullptr, nullptr, nullptr);
        if (success) {
            break;
        }

        err = GetLastError();
        HookLogImportant("FreezeWatchdog: Dump attempt failed (%s, error=%lu)", attempts[i].label, err);
        if (i + 1 < std::size(attempts)) {
            SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(hFile);
        }
    }

    CloseHandle(hFile);

    LARGE_INTEGER dumpSize = {};
    const bool hasNonEmptyDump =
        success && GetFileAttributesA(tempDumpPath.c_str()) != INVALID_FILE_ATTRIBUTES && [&]() {
            HANDLE sizeFile = CreateFileA(tempDumpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
            if (sizeFile == INVALID_HANDLE_VALUE) {
                return false;
            }
            const bool ok = GetFileSizeEx(sizeFile, &dumpSize) && dumpSize.QuadPart > 0;
            CloseHandle(sizeFile);
            return ok;
        }();

    if (hasNonEmptyDump && MoveFileExA(tempDumpPath.c_str(), dumpPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] SUCCESS: Dump created: %s\n", dumpPath.c_str());
        OutputDebugStringA(logMsg);
        HookLogImportant("FreezeWatchdog: Dump created at %s", dumpPath.c_str());
    } else {
        if (success && hasNonEmptyDump) {
            err = GetLastError();
            if (CopyFileA(tempDumpPath.c_str(), dumpPath.c_str(), FALSE)) {
                DeleteFileA(tempDumpPath.c_str());
                snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] SUCCESS: Dump copied after move failure: %s\n",
                         dumpPath.c_str());
                OutputDebugStringA(logMsg);
                HookLogImportant("FreezeWatchdog: Dump created at %s via CopyFile fallback", dumpPath.c_str());
                return;
            }
            HookLogImportant(
                "FreezeWatchdog: Failed to promote non-empty dump to %s (moveErr=%lu copyErr=%lu); preserving %s",
                dumpPath.c_str(), err, GetLastError(), tempDumpPath.c_str());
            return;
        }
        DeleteFileA(tempDumpPath.c_str());
        snprintf(logMsg, sizeof(logMsg), "[FreezeWatchdog] FAILED to write dump, error=%lu\n", err);
        OutputDebugStringA(logMsg);
        HookLogImportant("FreezeWatchdog: Dump creation failed at %s (error=%lu)", dumpPath.c_str(), err);
    }
}
