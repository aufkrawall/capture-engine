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
#include "crash_symbol_store.h"
#include "log_privacy.h"
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
static std::atomic<CrashPreDumpCallback> g_PreDumpCallback{nullptr};
static std::atomic<bool (*)(const char*, bool, const ExternalDumpException*)> g_ExternalCrashDumpCapture{nullptr};
static std::atomic<bool (*)()> g_ForeignOverlayLoadedQuery{nullptr};
static std::mutex g_TraceCrashMutex;
// TraceCrash runs from a vectored exception handler, which Windows can re-enter
// on the same thread - a fault raised while the handler is already running
// dispatches straight back into it. A plain std::mutex is not recursive, so the
// second entry blocks on a lock the same thread holds and the handler never
// returns. Gothic II session 20260916_011148 froze exactly there: the render
// thread overflowed its stack, CE's filter ran, and TraceCrash deadlocked in
// pthread_mutex_lock with crash.log still empty - the stack-overflow dump that
// should have named the recursion was never written. Ownership is tracked
// explicitly so a re-entrant call writes without the lock instead of waiting
// for itself.
static std::atomic<DWORD> g_TraceCrashOwnerThread{0};
std::atomic<DWORD> g_DumpDirMutexOwnerThread{0};
std::atomic<int> g_VEHCallCount{0};
static std::mutex g_SymbolArchiveMutex;
static std::filesystem::path g_SymbolStoreDir;  // guarded by g_SymbolArchiveMutex
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

        ce::crash_symbols::PlaceArtifact(entry.path(), archiveDir / entry.path().filename(), g_SymbolStoreDir);
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

        ce::crash_symbols::PlaceArtifact(entry.path(), symbolsRoot / entry.path().filename(), g_SymbolStoreDir);
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
        ExceptionSafeLock dirLock(g_DumpDirMutex, g_DumpDirMutexOwnerThread);
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

void ActivateCrashTrace() {
    g_CrashTraceActive.store(true, std::memory_order_release);
}

// Keep this process visible to WER (Windows Error Reporting). WER is the only
// mechanism that still records a __fastfail termination (0xC0000409), because
// that path is dispatched with FirstChance = FALSE and therefore reaches no
// vectored handler, no SEH frame and no unhandled-exception filter - see
// ce::crash_dump_policy::IsInProcessHandlerBypassingExitCode.
void RegisterWithWER() {
    // Hard errors (an unloadable image, a missing removable volume) must fail
    // rather than park a modal box on a game's render thread.
    //
    // SEM_NOGPFAULTERRORBOX is deliberately NOT set. It makes the default
    // UnhandledExceptionFilter terminate the process without invoking WER at
    // all, which is the opposite of what this function exists for; the report
    // UI is suppressed through WER_FAULT_REPORTING_NO_UI below instead. This
    // is the same rule the build itself follows (tools/build/build_common.py:
    // "crash reporting must keep producing the dumps this project debugs
    // from"), applied to the runtime.
    //
    // The error mode is process state this module shares with its host (the
    // game, when this is the injected hook), so CE adds its two bits instead of
    // replacing whatever the host chose.
    SetErrorMode(GetErrorMode() | SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);

    // Enable WER crash dumps - this catches __fastfail and other exceptions
    // that bypass our VEH handler
    HMODULE hWer = GetModuleHandleW(L"wer.dll");
    if (!hWer)
        hWer = ce::security::LoadSystemLibrary(L"wer.dll");
    if (hWer) {
        typedef HRESULT(WINAPI * PFN_WerSetFlags)(DWORD);
        typedef HRESULT(WINAPI * PFN_WerGetFlags)(HANDLE, PDWORD);
        auto pfnWerSetFlags = (PFN_WerSetFlags)GetProcAddress(hWer, "WerSetFlags");
        auto pfnWerGetFlags = (PFN_WerGetFlags)GetProcAddress(hWer, "WerGetFlags");
        if (pfnWerSetFlags) {
            // Like the error mode, the WER flags belong to the host process as
            // well; keep the ones it already set.
            DWORD existingWerFlags = 0;
            if (!pfnWerGetFlags || FAILED(pfnWerGetFlags(GetCurrentProcess(), &existingWerFlags))) {
                existingWerFlags = 0;
            }
            // WER_FAULT_REPORTING_NO_UI (0x20) is what actually keeps WerFault
            // from putting a dialog on screen, and it has to be set explicitly:
            // this call used to pass 0x3 while claiming to pass NO_UI, but 0x3
            // is NOHEAP | QUEUE. That mattered from the moment
            // SEM_NOGPFAULTERRORBOX came out of SetErrorMode above - this hook
            // DLL is loaded into the game, so the gap meant a crashing game
            // could show a fault dialog CE used to suppress.
            //
            // QUEUE keeps the report out of the interactive submit flow; NO_UI
            // suppresses the dialog. Neither suppresses the dump - that is
            // WerFault's LocalDumps work, which is the whole point of staying
            // visible to WER (see ce::wer_dump_adoption).
            constexpr DWORD kWerFaultReportingFlagNoHeap = 0x00000001;
            constexpr DWORD kWerFaultReportingFlagQueue = 0x00000002;
            constexpr DWORD kWerFaultReportingNoUi = 0x00000020;
            pfnWerSetFlags(existingWerFlags | kWerFaultReportingFlagNoHeap | kWerFaultReportingFlagQueue |
                           kWerFaultReportingNoUi);
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

    // CE used to also write the LocalDumps values under HKEY_CURRENT_USER here,
    // described as the "last resort" for exactly the __fastfail case. WER reads
    // LocalDumps from HKEY_LOCAL_MACHINE only, so those writes never had any
    // effect; session 20260919_154534 shows the HKCU key for witcher3.exe
    // naming the CE session directory while WER wrote to the default store
    // instead. All they produced was one stale subkey per game, each carrying
    // the user's own paths. CE now claims the dump WER really wrote
    // (ce::wer_dump_adoption) and purges the leftovers once from the
    // controller, and this process makes no machine-wide registry change at
    // all.
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

void SetCrashSymbolStoreRoot(const std::string& logsRoot) {
    std::lock_guard<std::mutex> lock(g_SymbolArchiveMutex);
    g_SymbolStoreDir =
        logsRoot.empty() ? std::filesystem::path() : ce::crash_symbols::StoreDirForLogsRoot(logsRoot);
}

void PruneCrashSymbolStore() {
    std::lock_guard<std::mutex> lock(g_SymbolArchiveMutex);
    if (g_SymbolStoreDir.empty())
        return;
    const size_t removed = ce::crash_symbols::PruneUnreferencedStoreFiles(g_SymbolStoreDir);
    if (removed > 0)
        LogInfo("CrashHandler: Pruned %zu symbol-store file(s) no retained session references", removed);
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

void RegisterCrashPreDumpCallback(CrashPreDumpCallback callback) {
    g_PreDumpCallback.store(callback, std::memory_order_release);
}

void NotifyCrashPreDump() {
    const CrashPreDumpCallback callback = g_PreDumpCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    TraceCrash("Running pre-dump release callback");
    callback();
}

void RegisterCrashDumpEnvironmentHooks(const CrashDumpEnvironmentHooks& hooks) {
    g_ExternalCrashDumpCapture.store(hooks.captureWithExternalHelper, std::memory_order_release);
    g_ForeignOverlayLoadedQuery.store(hooks.foreignOverlayLoaded, std::memory_order_release);
}

bool HasExternalCrashDumpCapture() {
    return g_ExternalCrashDumpCapture.load(std::memory_order_acquire) != nullptr;
}

bool CaptureCrashDumpWithExternalHelper(const char* dumpFileNameHint, bool stackOnly,
                                        const ExternalDumpException* exception) {
    auto capture = g_ExternalCrashDumpCapture.load(std::memory_order_acquire);
    return capture && dumpFileNameHint && dumpFileNameHint[0] && capture(dumpFileNameHint, stackOnly, exception);
}

bool IsForeignOverlayLoadedForCrashDump() {
    auto query = g_ForeignOverlayLoadedQuery.load(std::memory_order_acquire);
    return query && query();
}

LONG DispatchCrashExecutionFaultHandlerForTesting(EXCEPTION_POINTERS* pExceptionPointers) {
    return DispatchCrashExecutionFaultHandler(pExceptionPointers);
}

void NotifyCrashPreDumpForTesting() {
    NotifyCrashPreDump();
}

// Trace function for debugging the crash handler itself
// Thread-safe: uses mutex to prevent concurrent file corruption
void TraceCrash(const char* msg) {
    if (!msg || !g_CrashTraceActive.load(std::memory_order_acquire)) {
        return;
    }
    // crash.log is shared in support workflows, so it follows the same log
    // privacy contract as every other funnel (log_privacy.h): the Windows
    // account component of a user-profile path must be masked before the
    // message reaches the file. The redaction runs on a fixed stack buffer and
    // is length-preserving and in-place: `msg` is frequently a string literal,
    // and the crash path must not allocate. Every current caller formats into a
    // <= 512 byte buffer, so the bound only truncates hypothetical future
    // messages.
    char redacted[1024];
    snprintf(redacted, sizeof(redacted), "%s", msg);
    ce::privacy::RedactUserAccountComponents(redacted);

    ExceptionSafeLock lock(g_TraceCrashMutex, g_TraceCrashOwnerThread);
    std::string dumpDir;
    {
        ExceptionSafeLock dirLock(g_DumpDirMutex, g_DumpDirMutexOwnerThread);
        dumpDir = CrashDumpDirectoryStorage();
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\crash.log", dumpDir.c_str());
    FILE* f = fopen(path, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d][%s][%lu] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                g_ProcessName, GetCurrentThreadId(), redacted);
        fclose(f);
    }
}
