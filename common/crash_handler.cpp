// Crash-handler state, dump-directory management, symbol archiving, WER
// registration and tracing. The dump worker and the exception filters that
// consume all of this live in crash_dump_writer.cpp.

#include "crash_handler.h"
#include <direct.h>
#include <errno.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include "crash_dump_policy.h"
#include "logging.h"
#include "secure_dll_loading.h"

#include "crash_handler_internal.h"

std::mutex g_DumpDirMutex;
static char g_ProcessName[256] = "unknown";
HMODULE g_hDbgHelp = NULL;
std::atomic<bool> g_DumpAttemptInProgress{false};
std::atomic<bool> g_DumpSuccessfullyWritten{false};
std::atomic<bool> g_ForceUnhandledDump{false};
static std::atomic<bool> g_CrashTraceActive{false};
static std::atomic<CrashExecutionFaultHandler> g_ExecutionFaultHandler{nullptr};
static std::atomic<bool (*)(const char*)> g_ExternalCrashDumpCapture{nullptr};
static std::atomic<bool (*)()> g_ForeignOverlayLoadedQuery{nullptr};
static std::mutex g_TraceCrashMutex;
std::atomic<int> g_VEHCallCount{0};
std::atomic<int> g_RPCDisconnectedExceptionCount{0};
std::atomic<int> g_RPCServerUnavailableExceptionCount{0};
std::atomic<int> g_ENoInterfaceExceptionCount{0};
static std::mutex g_SymbolArchiveMutex;
MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

void TraceCrash(const char* msg);

std::string& CrashDumpDirectoryStorage() {
    // Function-local construction can report allocation failure to the first
    // caller instead of terminating during namespace-scope initialization.
    static std::string dumpDir = ".\\logs";
    return dumpDir;
}

LONG DispatchCrashExecutionFaultHandler(EXCEPTION_POINTERS* pExceptionPointers) {
    if (!pExceptionPointers || !pExceptionPointers->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto* record = pExceptionPointers->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const ULONG_PTR accessType = record->ExceptionInformation[0];
    const ULONG_PTR faultAddr = record->ExceptionInformation[1];
    if (accessType != 8) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CrashExecutionFaultHandler handler = g_ExecutionFaultHandler.load(std::memory_order_acquire);
    if (!handler) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const LONG result = handler(pExceptionPointers, accessType, faultAddr);
    return result == EXCEPTION_CONTINUE_EXECUTION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

namespace {

std::filesystem::path GetCurrentCrashHandlerModulePath() {
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&SetCrashDumpDirectory), &hModule) ||
        !hModule) {
        return {};
    }

    char modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(hModule, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }

    return std::filesystem::path(modulePath);
}

std::filesystem::path BuildCrashSymbolArchiveDir(const std::string& dumpDir) {
    return std::filesystem::path(dumpDir) / ce::crash_dump_policy::kSymbolArchiveDirName /
           ce::crash_dump_policy::kCaptureEngineArchiveDirName;
}

void WriteCrashSymbolArchiveManifestIfMissing(const std::filesystem::path& archiveDir,
                                              const std::filesystem::path& sourceDir,
                                              const std::filesystem::path& modulePath) {
    const std::filesystem::path manifestPath = archiveDir / ce::crash_dump_policy::kSymbolArchiveManifestFileName;
    if (std::filesystem::exists(manifestPath)) {
        return;
    }

    FILE* manifest = fopen(manifestPath.string().c_str(), "w");
    if (!manifest) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(manifest,
            "archived_at=%04u-%02u-%02u %02u:%02u:%02u.%03u\nprocess_name=%s\npid=%lu\nmodule_path=%s\n"
            "source_dir=%s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_ProcessName,
            GetCurrentProcessId(), modulePath.string().c_str(), sourceDir.string().c_str());
    fclose(manifest);
}

void ArchiveInstalledCrashArtifactsForDumpDirectory(const std::string& dumpDir) {
    if (dumpDir.empty()) {
        return;
    }

    const std::filesystem::path modulePath = GetCurrentCrashHandlerModulePath();
    const std::filesystem::path sourceDir = modulePath.parent_path();
    if (sourceDir.empty() || !std::filesystem::exists(sourceDir)) {
        return;
    }

    const std::filesystem::path archiveDir = BuildCrashSymbolArchiveDir(dumpDir);
    std::error_code ec;
    std::lock_guard<std::mutex> lock(g_SymbolArchiveMutex);
    std::filesystem::create_directories(archiveDir, ec);
    if (ec) {
        return;
    }

    WriteCrashSymbolArchiveManifestIfMissing(archiveDir, sourceDir, modulePath);

    for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        const std::string fileName = entry.path().filename().string();
        if (!ce::crash_dump_policy::ShouldArchiveInstalledCrashArtifactFileName(fileName.c_str())) {
            continue;
        }

        const std::filesystem::path destinationPath = archiveDir / entry.path().filename();
        if (std::filesystem::exists(destinationPath)) {
            continue;
        }

        std::filesystem::copy_file(entry.path(), destinationPath, std::filesystem::copy_options::none, ec);
        if (ec) {
            ec.clear();
        }
    }

    // Also copy PDB files directly to the symbols/ root directory so that cdb
    // can find them with a symbol path like "srv*;...\symbols" without needing
    // a per-module subdirectory.  The captureengine/ subdirectory above already
    // preserves the full set of artifacts for manual inspection.
    const std::filesystem::path symbolsRoot = archiveDir.parent_path();
    for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        const std::string fileName = entry.path().filename().string();
        if (!ce::crash_dump_policy::EndsWithAsciiInsensitive(fileName.c_str(), ".pdb")) {
            continue;
        }
        if (ce::crash_dump_policy::ContainsAsciiInsensitive(fileName.c_str(), ".old.")) {
            continue;
        }

        const std::filesystem::path dest = symbolsRoot / entry.path().filename();
        if (std::filesystem::exists(dest)) {
            continue;
        }

        std::filesystem::copy_file(entry.path(), dest, std::filesystem::copy_options::none, ec);
        if (ec) {
            ec.clear();
        }
    }
}

void DeleteStaleEmptyInProgressDumpArtifactsForDirectory(const std::string& dumpDir) {
    if (dumpDir.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(dumpDir, ec) || ec) {
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dumpDir, ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }

        const std::string fileName = entry.path().filename().string();
        const auto fileSize = entry.file_size(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!ce::crash_dump_policy::IsStaleEmptyInProgressDumpArtifact(fileName.c_str(),
                                                                       static_cast<uint64_t>(fileSize))) {
            continue;
        }

        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
            LogInfo("CrashHandler: Removed stale empty in-progress dump artifact %s", fileName.c_str());
        } else {
            ec.clear();
        }
    }
}

}  // namespace

bool PromoteInProgressDumpFile(const char* tempDumpPath, const char* dumpPath, const char* traceContext,
                               bool* preservedTempDump) {
    if (preservedTempDump) {
        *preservedTempDump = false;
    }
    if (!tempDumpPath || !dumpPath) {
        return false;
    }

    if (MoveFileExA(tempDumpPath, dumpPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }

    const DWORD moveErr = GetLastError();
    char msg[512];
    snprintf(msg, sizeof(msg), "%s: MoveFileEx failed while promoting in-progress dump (err=%lu)",
             traceContext && traceContext[0] ? traceContext : "CrashHandler", moveErr);
    TraceCrash(msg);

    if (CopyFileA(tempDumpPath, dumpPath, FALSE)) {
        DeleteFileA(tempDumpPath);
        snprintf(msg, sizeof(msg), "%s: Promoted in-progress dump via CopyFile fallback",
                 traceContext && traceContext[0] ? traceContext : "CrashHandler");
        TraceCrash(msg);
        return true;
    }

    const DWORD copyErr = GetLastError();
    snprintf(msg, sizeof(msg),
             "%s: CopyFile fallback failed while promoting in-progress dump (moveErr=%lu copyErr=%lu); preserving %s",
             traceContext && traceContext[0] ? traceContext : "CrashHandler", moveErr, copyErr, tempDumpPath);
    TraceCrash(msg);
    if (preservedTempDump) {
        *preservedTempDump = true;
    }
    return false;
}

bool WriteSupplementalCrashDump(const char* fileNameHint, HANDLE hProcess, DWORD processId,
                                MINIDUMP_TYPE preferredDumpType, PMINIDUMP_EXCEPTION_INFORMATION exceptionParam,
                                PMINIDUMP_USER_STREAM_INFORMATION userStreamParam,
                                PMINIDUMP_CALLBACK_INFORMATION callbackParam) {
    if (!g_pMiniDumpWriteDump) {
        return false;
    }

    std::string dumpDir;
    {
        std::lock_guard<std::mutex> dirLock(g_DumpDirMutex);
        dumpDir = CrashDumpDirectoryStorage();
    }
    if (dumpDir.empty()) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    if (ec) {
        return false;
    }

    const std::string dumpFileName =
        ce::crash_dump_policy::BuildSupplementalCrashDumpFileNameFromExternalSource(fileNameHint);
    const std::string tempDumpFileName = ce::crash_dump_policy::BuildInProgressDumpFileName(dumpFileName.c_str());
    const std::filesystem::path dumpPath = std::filesystem::path(dumpDir) / dumpFileName;
    const std::filesystem::path tempDumpPath = std::filesystem::path(dumpDir) / tempDumpFileName;

    DeleteFileA(tempDumpPath.string().c_str());
    HANDLE hFile = CreateFileA(tempDumpPath.string().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    struct DumpAttempt {
        MINIDUMP_TYPE type;
        PMINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
    };

    const DumpAttempt attempts[] = {
        {preferredDumpType, exceptionParam},
        {ce::crash_dump_policy::kCompatibilityCrashDumpType, exceptionParam},
        {ce::crash_dump_policy::kMinimalDumpType, exceptionParam},
        {ce::crash_dump_policy::kMinimalDumpType, nullptr},
    };

    BOOL success = FALSE;
    for (size_t i = 0; i < std::size(attempts); ++i) {
        success = g_pMiniDumpWriteDump(hProcess, processId, hFile, attempts[i].type, attempts[i].exceptionInfo,
                                       userStreamParam, callbackParam);
        if (success) {
            FlushFileBuffers(hFile);
            break;
        }

        if (i + 1 < std::size(attempts)) {
            SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
            SetEndOfFile(hFile);
        }
    }

    LARGE_INTEGER dumpSize = {};
    const bool hasNonEmptyDump = success && GetFileSizeEx(hFile, &dumpSize) && dumpSize.QuadPart > 0;
    CloseHandle(hFile);
    if (!hasNonEmptyDump) {
        DeleteFileA(tempDumpPath.string().c_str());
        return false;
    }

    bool preservedTempDump = false;
    return PromoteInProgressDumpFile(tempDumpPath.string().c_str(), dumpPath.string().c_str(), "SupplementalCrashDump",
                                     &preservedTempDump) ||
           preservedTempDump;
}

int IncrementExceptionCount(std::atomic<int>& counter) {
    return counter.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void ActivateCrashTrace() {
    g_CrashTraceActive.store(true, std::memory_order_release);
}

// Register this process with WER (Windows Error Reporting) so crash dumps are
// generated even for __fastfail() which bypasses VEH and UEF handlers.
// This is critical for catching /GS stack buffer overrun (0xC0000409) crashes.
void RegisterWithWER() {
    // Prevent Windows Error Reporting dialog from appearing
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);

    // Enable WER crash dumps - this catches __fastfail and other exceptions
    // that bypass our VEH handler
    HMODULE hWer = GetModuleHandleW(L"wer.dll");
    if (!hWer)
        hWer = ce::security::LoadSystemLibrary(L"wer.dll");
    if (hWer) {
        typedef HRESULT(WINAPI * PFN_WerSetFlags)(DWORD);
        auto pfnWerSetFlags = (PFN_WerSetFlags)GetProcAddress(hWer, "WerSetFlags");
        if (pfnWerSetFlags) {
            pfnWerSetFlags(0x00000003);  // WER_FAULT_REPORTING_NO_UI | WER_FAULT_REPORTING_QUEUE
        }
    }

    // Also register our dump directory with WER so dumps go to our logs folder.
    // These APIs live in wer.dll; looking them up on kernel32 leaves the
    // programmatic WER fallback path inert.
    if (hWer) {
        // WerRegisterFile - registers a file with WER to include in crash reports
        typedef HRESULT(WINAPI * PFN_WerRegisterFile)(PCWSTR, DWORD, DWORD);
        auto pfnWerRegisterFile = (PFN_WerRegisterFile)GetProcAddress(hWer, "WerRegisterFile");
        if (pfnWerRegisterFile) {
            // Register our dump directory as a file to include in WER reports
            wchar_t dumpDirW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, CrashDumpDirectoryStorage().c_str(), -1, dumpDirW, MAX_PATH);
            pfnWerRegisterFile(dumpDirW, 1 /*WER_FILE_ANOTHER*/, 0);
        }

        // Enable WER local dumps programmatically (creates dumps in %LOCALAPPDATA%\CrashDumps)
        // This is a fallback in case our VEH/UEF crash handlers don't catch the exception
        typedef HRESULT(WINAPI * PFN_WerAddNamedDumpStore)(PCWSTR, PCWSTR);
        auto pfnWerAddNamedDumpStore = (PFN_WerAddNamedDumpStore)GetProcAddress(hWer, "WerAddNamedDumpStore");
        if (pfnWerAddNamedDumpStore) {
            wchar_t dumpDirW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, CrashDumpDirectoryStorage().c_str(), -1, dumpDirW, MAX_PATH);
            pfnWerAddNamedDumpStore(L"CaptureEngine", dumpDirW);
        }
    }

    // Also set WER registry keys for local dump generation as last resort
    // This catches __fastfail crashes that bypass VEH and UEF
    HKEY hKey = NULL;
    wchar_t procPath[MAX_PATH];
    GetModuleFileNameW(NULL, procPath, MAX_PATH);
    std::wstring procName(procPath);
    size_t lastSlash = procName.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos)
        procName = procName.substr(lastSlash + 1);

    // Per-application WER dump settings
    std::wstring regPath = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\" + procName;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) ==
        ERROR_SUCCESS) {
        DWORD dumpType = 2;  // MiniDumpWithFullMemory
        DWORD dumpCount = 10;
        wchar_t dumpDirW[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, CrashDumpDirectoryStorage().c_str(), -1, dumpDirW, MAX_PATH);
        RegSetValueExW(hKey, L"DumpType", 0, REG_DWORD, (BYTE*)&dumpType, sizeof(dumpType));
        RegSetValueExW(hKey, L"DumpCount", 0, REG_DWORD, (BYTE*)&dumpCount, sizeof(dumpCount));
        RegSetValueExW(hKey, L"DumpFolder", 0, REG_EXPAND_SZ, (BYTE*)dumpDirW,
                       (DWORD)((wcslen(dumpDirW) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }

    // Also set global WER settings for ALL apps (covers subprocesses, thread pool crashes)
    regPath = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) ==
        ERROR_SUCCESS) {
        DWORD dumpType = 2;
        DWORD dumpCount = 10;
        wchar_t dumpDirW2[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, CrashDumpDirectoryStorage().c_str(), -1, dumpDirW2, MAX_PATH);
        RegSetValueExW(hKey, L"DumpType", 0, REG_DWORD, (BYTE*)&dumpType, sizeof(dumpType));
        RegSetValueExW(hKey, L"DumpCount", 0, REG_DWORD, (BYTE*)&dumpCount, sizeof(dumpCount));
        RegSetValueExW(hKey, L"DumpFolder", 0, REG_EXPAND_SZ, (BYTE*)dumpDirW2,
                       (DWORD)((wcslen(dumpDirW2) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

void SetCrashDumpDirectory(const std::string& dir, bool archiveInstalledSymbols) {
    {
        std::lock_guard<std::mutex> lock(g_DumpDirMutex);
        CrashDumpDirectoryStorage() = dir;
    }
    DeleteStaleEmptyInProgressDumpArtifactsForDirectory(dir);
    if (archiveInstalledSymbols) {
        ArchiveInstalledCrashArtifactsForDumpDirectory(dir);
    }
}

std::string GetCrashDumpDirectory() {
    std::lock_guard<std::mutex> lock(g_DumpDirMutex);
    return CrashDumpDirectoryStorage();
}

void SetCrashProcessName(const char* name) {
    if (name) {
        strncpy(g_ProcessName, name, sizeof(g_ProcessName) - 1);
        g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
    }
}

void RegisterCrashExecutionFaultHandler(CrashExecutionFaultHandler handler) {
    g_ExecutionFaultHandler.store(handler, std::memory_order_release);
}

void RegisterCrashDumpEnvironmentHooks(const CrashDumpEnvironmentHooks& hooks) {
    g_ExternalCrashDumpCapture.store(hooks.captureWithExternalHelper, std::memory_order_release);
    g_ForeignOverlayLoadedQuery.store(hooks.foreignOverlayLoaded, std::memory_order_release);
}

bool HasExternalCrashDumpCapture() {
    return g_ExternalCrashDumpCapture.load(std::memory_order_acquire) != nullptr;
}

bool CaptureCrashDumpWithExternalHelper(const char* dumpFileNameHint) {
    auto capture = g_ExternalCrashDumpCapture.load(std::memory_order_acquire);
    return capture && dumpFileNameHint && dumpFileNameHint[0] && capture(dumpFileNameHint);
}

bool IsForeignOverlayLoadedForCrashDump() {
    auto query = g_ForeignOverlayLoadedQuery.load(std::memory_order_acquire);
    return query && query();
}

LONG DispatchCrashExecutionFaultHandlerForTesting(EXCEPTION_POINTERS* pExceptionPointers) {
    return DispatchCrashExecutionFaultHandler(pExceptionPointers);
}

// Trace function for debugging the crash handler itself
// Thread-safe: uses mutex to prevent concurrent file corruption
void TraceCrash(const char* msg) {
    if (!msg || !g_CrashTraceActive.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_TraceCrashMutex);
    std::string dumpDir;
    {
        std::lock_guard<std::mutex> dirLock(g_DumpDirMutex);
        dumpDir = CrashDumpDirectoryStorage();
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\crash.log", dumpDir.c_str());
    FILE* f = fopen(path, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d][%s][%lu] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                g_ProcessName, GetCurrentThreadId(), msg);
        fclose(f);
    }
}
