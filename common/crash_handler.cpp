#include "crash_handler.h"
#include <dbghelp.h>
#include <direct.h>
#include <errno.h>
#include <werapi.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include "logging.h"

static std::string g_DumpDir = ".\\logs";
static std::mutex g_DumpDirMutex;
static char g_ProcessName[256] = "unknown";
static HMODULE g_hDbgHelp = NULL;
static std::atomic<bool> g_DumpAlreadyWritten{false};
static std::atomic<bool> g_ForceUnhandledDump{false};
static std::mutex g_TraceCrashMutex;
static std::atomic<int> g_VEHCallCount{0};
typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
static MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

// Register this process with WER (Windows Error Reporting) so crash dumps are
// generated even for __fastfail() which bypasses VEH and UEF handlers.
// This is critical for catching /GS stack buffer overrun (0xC0000409) crashes.
static void RegisterWithWER() {
    // Prevent Windows Error Reporting dialog from appearing
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);

    // Enable WER crash dumps - this catches __fastfail and other exceptions
    // that bypass our VEH handler
    HMODULE hWer = GetModuleHandleW(L"wer.dll");
    if (!hWer)
        hWer = LoadLibraryW(L"wer.dll");
    if (hWer) {
        typedef HRESULT(WINAPI * PFN_WerSetFlags)(DWORD);
        auto pfnWerSetFlags = (PFN_WerSetFlags)GetProcAddress(hWer, "WerSetFlags");
        if (pfnWerSetFlags) {
            pfnWerSetFlags(0x00000003);  // WER_FAULT_REPORTING_NO_UI | WER_FAULT_REPORTING_QUEUE
        }
    }

    // Also register our dump directory with WER so dumps go to our logs folder
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (hKernel) {
        // WerRegisterFile - registers a file with WER to include in crash reports
        typedef HRESULT(WINAPI * PFN_WerRegisterFile)(PCWSTR, DWORD, DWORD);
        auto pfnWerRegisterFile = (PFN_WerRegisterFile)GetProcAddress(hKernel, "WerRegisterFile");
        if (pfnWerRegisterFile) {
            // Register our dump directory as a file to include in WER reports
            wchar_t dumpDirW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, g_DumpDir.c_str(), -1, dumpDirW, MAX_PATH);
            pfnWerRegisterFile(dumpDirW, 1 /*WER_FILE_ANOTHER*/, 0);
        }

        // Enable WER local dumps programmatically (creates dumps in %LOCALAPPDATA%\CrashDumps)
        // This is a fallback in case our VEH/UEF crash handlers don't catch the exception
        typedef HRESULT(WINAPI * PFN_WerAddNamedDumpStore)(PCWSTR, PCWSTR);
        auto pfnWerAddNamedDumpStore = (PFN_WerAddNamedDumpStore)GetProcAddress(hKernel, "WerAddNamedDumpStore");
        if (pfnWerAddNamedDumpStore) {
            wchar_t dumpDirW[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, g_DumpDir.c_str(), -1, dumpDirW, MAX_PATH);
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
        MultiByteToWideChar(CP_UTF8, 0, g_DumpDir.c_str(), -1, dumpDirW, MAX_PATH);
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
        MultiByteToWideChar(CP_UTF8, 0, g_DumpDir.c_str(), -1, dumpDirW2, MAX_PATH);
        RegSetValueExW(hKey, L"DumpType", 0, REG_DWORD, (BYTE*)&dumpType, sizeof(dumpType));
        RegSetValueExW(hKey, L"DumpCount", 0, REG_DWORD, (BYTE*)&dumpCount, sizeof(dumpCount));
        RegSetValueExW(hKey, L"DumpFolder", 0, REG_EXPAND_SZ, (BYTE*)dumpDirW2,
                       (DWORD)((wcslen(dumpDirW2) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

void SetCrashDumpDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_DumpDirMutex);
    g_DumpDir = dir;
}

void SetCrashProcessName(const char* name) {
    if (name) {
        strncpy(g_ProcessName, name, sizeof(g_ProcessName) - 1);
        g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
    }
}

// Trace function for debugging the crash handler itself
// Thread-safe: uses mutex to prevent concurrent file corruption
void TraceCrash(const char* msg) {
    std::lock_guard<std::mutex> lock(g_TraceCrashMutex);
    std::string dumpDir;
    {
        std::lock_guard<std::mutex> dirLock(g_DumpDirMutex);
        dumpDir = g_DumpDir;
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

struct DumpParams {
    EXCEPTION_POINTERS* pExceptionPointers;
    DWORD threadId;
};

// Worker thread to write minidump safely away from the crashed stack
DWORD WINAPI DumpWorker(LPVOID lpParam) {
    std::unique_ptr<DumpParams> params(static_cast<DumpParams*>(lpParam));

    TraceCrash("DumpWorker started");

    // Read dump directory under mutex
    std::string dumpDir;
    {
        std::lock_guard<std::mutex> dirLock(g_DumpDirMutex);
        dumpDir = g_DumpDir;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[128];
    snprintf(buf, sizeof(buf), "%04u%02u%02u_%02u%02u%02u_%03u_pid%lu_tid%lu", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), params->threadId);

    char dumpPath[MAX_PATH];
    snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", dumpDir.c_str(), buf);

    TraceCrash("Creating dump file...");
    TraceCrash(dumpPath);

    // Ensure directory exists with proper error checking
    if (CreateDirectoryA(dumpDir.c_str(), NULL)) {
        TraceCrash("Created dump directory");
    } else {
        DWORD dirErr = GetLastError();
        if (dirErr == ERROR_ALREADY_EXISTS) {
            TraceCrash("Dump directory already exists");
        } else {
            char dirErrMsg[128];
            snprintf(dirErrMsg, sizeof(dirErrMsg), "Failed to create dump directory (err=%lu)", dirErr);
            TraceCrash(dirErrMsg);
        }
    }

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD createErr = GetLastError();
        char createErrMsg[160];
        snprintf(createErrMsg, sizeof(createErrMsg), "Failed to create dump file at configured path (err=%lu)",
                 createErr);
        TraceCrash(createErrMsg);
    }

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = params->threadId;
        mdei.ExceptionPointers = params->pExceptionPointers;
        mdei.ClientPointers = FALSE;

        MINIDUMP_TYPE primaryType =
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);

        struct DumpAttempt {
            MINIDUMP_TYPE type;
            bool withExceptionInfo;
            const char* label;
        };

        const DumpAttempt attempts[] = {
            {primaryType, true, "primary"},
            {(MINIDUMP_TYPE)MiniDumpNormal, true, "fallback-normal"},
            {(MINIDUMP_TYPE)MiniDumpNormal, false, "fallback-no-exception"},
        };
        const size_t attemptCount = sizeof(attempts) / sizeof(attempts[0]);

        TraceCrash("Calling MiniDumpWriteDump from worker thread...");

        BOOL rv = FALSE;
        DWORD err = ERROR_SUCCESS;
        if (g_pMiniDumpWriteDump) {
            for (size_t i = 0; i < attemptCount; ++i) {
                char attemptMsg[192];
                snprintf(attemptMsg, sizeof(attemptMsg), "MiniDumpWriteDump attempt %llu/%llu (%s)",
                         (unsigned long long)(i + 1), (unsigned long long)attemptCount, attempts[i].label);
                TraceCrash(attemptMsg);

                PMINIDUMP_EXCEPTION_INFORMATION pExc = attempts[i].withExceptionInfo ? &mdei : nullptr;
                rv = g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, attempts[i].type, pExc, 0,
                                          0);
                if (rv) {
                    break;
                }

                err = GetLastError();
                snprintf(attemptMsg, sizeof(attemptMsg), "MiniDumpWriteDump attempt failed (%s): %lu (0x%08lX)",
                         attempts[i].label, err, err);
                TraceCrash(attemptMsg);

                if (i + 1 < attemptCount) {
                    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
                    SetEndOfFile(hFile);
                }
            }
        } else {
            TraceCrash("g_pMiniDumpWriteDump is NULL in worker!");
            err = ERROR_PROC_NOT_FOUND;
        }

        if (rv) {
            TraceCrash("MiniDumpWriteDump Success");
            FlushFileBuffers(hFile);
            char msg[256];
            snprintf(msg, sizeof(msg), "[CrashHandler] Minidump created at: %s\n", dumpPath);
            OutputDebugStringA(msg);
        } else {
            TraceCrash("MiniDumpWriteDump Failed");
            char msg[256];
            snprintf(msg, sizeof(msg), "[CrashHandler] MiniDumpWriteDump failed: %lu (0x%08lX)\n", err, err);
            OutputDebugStringA(msg);

            char errPath[MAX_PATH];
            snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt", dumpDir.c_str());
            FILE* f = fopen(errPath, "w");
            if (f) {
                fprintf(f, "MiniDumpWriteDump failed. Error: %lu (0x%08lX)\nDump Path: %s\n", err, err, dumpPath);
                fclose(f);
            }
        }
        CloseHandle(hFile);
    } else {
        TraceCrash("Failed to create dump file");
    }

    return 0;
}

LONG WINAPI CrashHandlerExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers) {
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

    // Track VEH call count for detecting runaway exception storms
    int callCount = g_VEHCallCount.fetch_add(1, std::memory_order_acq_rel);

    // If the UnhandledExceptionFilter has asked us to force a dump, do it
    // regardless of exception code.
    bool forceDump = g_ForceUnhandledDump.load(std::memory_order_acquire);

    // Read dump directory with try_lock to avoid deadlock if crashed thread owns the mutex
    std::string dumpDir;
    {
        std::unique_lock<std::mutex> dirLock(g_DumpDirMutex, std::try_to_lock);
        if (dirLock.owns_lock()) {
            dumpDir = g_DumpDir;
        } else {
            dumpDir = ".\\logs";  // Fallback default if mutex is contended
        }
    }

    // Skip ONLY truly benign exceptions that are used for debug/IPC purposes
    // These are first-chance only and never indicate real crashes.
    if (!forceDump) {
        switch (code) {
            case 0x406D1388:  // Thread naming exception (VS debugger)
            case 0x40010006:  // OutputDebugString
            case 0x4001000A:  // WOW64 debug
            case 0x4000001F:  // Wow64 breakpoint
                return EXCEPTION_CONTINUE_SEARCH;
            default:
                break;
        }
    }

    // COM disconnect exceptions on thread pool workers are benign during
    // process shutdown. COM's LRPC infrastructure tries to dispatch pending
    // RPC calls after the process has started releasing COM objects.
    // This causes RPC_E_DISCONNECTED on a TppWorkerThread.
    // Only dump if we get an excessive number (indicates a real issue).
    if (!forceDump && code == 0x80010108) {
        if (callCount < 5) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // RPC_S_SERVER_UNAVAILABLE (0x800706ba) on thread pool timer callbacks:
    // Windows COM timer tries to clean up marshaling context for a dead process.
    // This is benign during cross-process teardown (e.g., inject process exits).
    if (!forceDump && code == 0x800706ba) {
        if (callCount < 3) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // Breakpoints: only skip if we haven't been called too many times
    // (excessive breakpoints suggest a real crash loop)
    if (!forceDump && code == 0x80000003 && callCount < 5) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Generic C++ exceptions are commonly used for recoverable library error
    // paths (for example, D3D11/WinRT throwing before the caller falls back to
    // a safe path). MinGW/clang uses 0x20474343 (" GCC"), while MSVC/CRT uses
    // 0xE06D7363. Do not treat first-chance C++ EH as a crash here; if it is
    // truly unhandled, the top-level UEF path will re-enter with forceDump=true
    // and write the dump there.
    if (!forceDump && (code == 0xE06D7363 || code == 0x20474343)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // STATUS_STACK_BUFFER_OVERRUN (0xC0000409) is often raised via __fastfail()
    // which bypasses normal VEH. If we catch it here, it's a second-chance
    // or the process has a custom handler. Always dump these - they indicate
    // real corruption.
    if (code == 0xC0000409) {
        TraceCrash("STACK_BUFFER_OVERRUN detected - generating dump");
        // Fall through to dump generation
    }

    // STATUS_FATAL_USER_CALLBACK_EXCEPTION - crash in a Windows callback
    if (code == 0xC000041D) {
        TraceCrash("FATAL_USER_CALLBACK_EXCEPTION - generating dump");
    }

    // Log the exception for debugging
    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "VEH Exception: 0x%08lX at 0x%p (PID:%lu TID:%lu, call#%d)", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress, GetCurrentProcessId(), GetCurrentThreadId(),
             callCount);
    TraceCrash(codeStr);

    // COM disconnected exceptions often precede real crashes in DirectX
    // when the GPU driver resets or the device is lost. Always dump these.
    if (code == 0x80010108 ||  // RPC_E_DISCONNECTED
        code == 0x80004005 ||  // E_FAIL
        code == 0x8876086A ||  // DXGI_ERROR_DEVICE_RESET
        code == 0x887A0006 ||  // DXGI_ERROR_DEVICE_HUNG
        code == 0x887A0007 ||  // DXGI_ERROR_DEVICE_REMOVED
        code == 0x887A0020) {  // DXGI_ERROR_ACCESS_LOST
        TraceCrash("COM/DXGI fatal exception detected - generating dump");
        // Fall through to dump generation
    }

    // UE5 ensure() assertion (0x4000): continuable, but UE5 may call
    // TerminateProcess shortly after. Write a FAST MiniDumpNormal for
    // diagnostics (<50 ms, ~100 KB) then let UE5's handler continue.
    if (code == 0x00004000) {
        {
            HMODULE hMod = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)pExceptionPointers->ExceptionRecord->ExceptionAddress, &hMod);
            char modName[MAX_PATH] = "unknown";
            if (hMod)
                GetModuleFileNameA(hMod, modName, MAX_PATH);
            char* baseName = strrchr(modName, '\\');
            baseName = baseName ? baseName + 1 : modName;
            char loc[512];
            snprintf(loc, sizeof(loc), "UE5 ensure() in %s at 0x%p (offset 0x%llX)", baseName,
                     pExceptionPointers->ExceptionRecord->ExceptionAddress,
                     hMod ? (unsigned long long)((uintptr_t)pExceptionPointers->ExceptionRecord->ExceptionAddress -
                                                 (uintptr_t)hMod)
                          : 0ULL);
            TraceCrash(loc);
        }

        // Quick inline dump — MiniDumpNormal only, no worker thread / timeout.
        if (g_pMiniDumpWriteDump) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char dumpPath[MAX_PATH];
            snprintf(dumpPath, sizeof(dumpPath), "%s\\assert_%04u%02u%02u_%02u%02u%02u_%03u_pid%lu.dmp",
                     dumpDir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     GetCurrentProcessId());

            HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mdei;
                mdei.ThreadId = GetCurrentThreadId();
                mdei.ExceptionPointers = pExceptionPointers;
                mdei.ClientPointers = FALSE;

                if (g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mdei, NULL,
                                         NULL)) {
                    TraceCrash("Quick assert dump written");
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Assert dump: %s", dumpPath);
                    TraceCrash(msg);
                }
                CloseHandle(hFile);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ANY exception that reaches here is considered potentially fatal.
    // Write a dump for ALL of them. This is the critical change: instead of
    // whitelisting "known crashes", we blacklist "known benign" and dump
    // everything else. This ensures we capture 0xC0000409, COM exceptions,
    // and any other crash codes that might not be in our known list.

    TraceCrash("CRASH DETECTED - Handling exception");

    // Prevent duplicate dumps (VEH + UEF both fire for the same crash)
    bool expected = false;
    if (!g_DumpAlreadyWritten.compare_exchange_strong(expected, true)) {
        TraceCrash("Dump already written by previous handler, skipping");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Ensure trace log goes to the correct dir
    TraceCrash("CrashHandlerExceptionFilter entered");

    char bufCode[64];
    snprintf(bufCode, sizeof(bufCode), "Exception Code: 0x%08lX", code);
    TraceCrash(bufCode);

    // Log detailed exception info for access violations
    if (code == EXCEPTION_ACCESS_VIOLATION && pExceptionPointers->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR accessType = pExceptionPointers->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR faultAddr = pExceptionPointers->ExceptionRecord->ExceptionInformation[1];
        char avDetail[256];
        snprintf(avDetail, sizeof(avDetail), "Access Violation: %s address 0x%p",
                 accessType == 0 ? "READ from" : (accessType == 1 ? "WRITE to" : "DEP at"), (void*)faultAddr);
        TraceCrash(avDetail);
    }

    // Log crash address with module info
    {
        HMODULE hCrashMod = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pExceptionPointers->ExceptionRecord->ExceptionAddress, &hCrashMod);
        char modName[MAX_PATH] = "unknown";
        if (hCrashMod)
            GetModuleFileNameA(hCrashMod, modName, MAX_PATH);

        char* modBaseName = strrchr(modName, '\\');
        modBaseName = modBaseName ? modBaseName + 1 : modName;

        char crashLoc[512];
        snprintf(crashLoc, sizeof(crashLoc), "Crash in module: %s at 0x%p (base=0x%p, offset=0x%llX)", modBaseName,
                 pExceptionPointers->ExceptionRecord->ExceptionAddress, (void*)hCrashMod,
                 hCrashMod ? (unsigned long long)((uintptr_t)pExceptionPointers->ExceptionRecord->ExceptionAddress -
                                                  (uintptr_t)hCrashMod)
                           : 0ULL);
        TraceCrash(crashLoc);
    }

    // Log key registers for post-mortem analysis
    {
        CONTEXT* ctx = pExceptionPointers->ContextRecord;
        char regBuf[512];
#ifdef _WIN64
        snprintf(regBuf, sizeof(regBuf),
                 "Registers: RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX "
                 "RAX=0x%016llX RCX=0x%016llX RDX=0x%016llX R8=0x%016llX R9=0x%016llX",
                 (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp,
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
                 (unsigned long long)ctx->R8, (unsigned long long)ctx->R9);
#else
        snprintf(regBuf, sizeof(regBuf),
                 "Registers: EIP=0x%08X ESP=0x%08X EBP=0x%08X "
                 "EAX=0x%08X ECX=0x%08X EDX=0x%08X",
                 ctx->Eip, ctx->Esp, ctx->Ebp, ctx->Eax, ctx->Ecx, ctx->Edx);
#endif
        TraceCrash(regBuf);
    }

    OutputDebugStringA("[CrashHandler] CRASH DETECTED! Spawning worker for minidump...\n");

    if (!g_pMiniDumpWriteDump) {
        TraceCrash("g_pMiniDumpWriteDump is NULL - cannot write dump!");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Heap-allocate params to avoid dangling pointer if this function returns
    // before the worker thread starts reading the data.
    auto* params = new DumpParams{pExceptionPointers, GetCurrentThreadId()};

    // Spawn thread to handle dump writing (crucial for Stack Overflow exceptions)
    HANDLE hThread = CreateThread(NULL, 0, DumpWorker, params, 0, NULL);

    if (hThread) {
        TraceCrash("Worker thread spawned, waiting (15s timeout)...");
        DWORD waitResult = WaitForSingleObject(hThread, 15000);
        if (waitResult == WAIT_TIMEOUT) {
            TraceCrash("Worker thread timed out after 15s - continuing without dump");
        } else {
            TraceCrash("Worker thread finished.");
        }
        CloseHandle(hThread);
    } else {
        TraceCrash("Failed to create worker thread! Attempting inline dump...");
        DumpWorker(params);  // Fallback to inline if thread creation fails (DumpWorker takes ownership)
    }

    TraceCrash("Handler finished - Returning EXCEPTION_CONTINUE_SEARCH");
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_CrashHandlerInstalled = false;

static LPTOP_LEVEL_EXCEPTION_FILTER g_OldUnhandledFilter = NULL;

LONG WINAPI UnhandledExceptionFilterCallback(EXCEPTION_POINTERS* pExceptionPointers) {
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "UnhandledExceptionFilter: 0x%08lX at 0x%p (TID:%lu)", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress, GetCurrentThreadId());
    TraceCrash(codeStr);

    // Unhandled exception filter is the LAST line of defense. ALWAYS dump,
    // regardless of exception code. If we reached this filter, the exception
    // was not handled by anyone else and the process is about to terminate.
    TraceCrash("Unhandled exception reached top-level filter - FORCING dump");
    g_ForceUnhandledDump.store(true, std::memory_order_release);
    LONG result = CrashHandlerExceptionFilter(pExceptionPointers);
    g_ForceUnhandledDump.store(false, std::memory_order_release);

    if (g_OldUnhandledFilter && g_OldUnhandledFilter != UnhandledExceptionFilterCallback) {
        LONG prevResult = g_OldUnhandledFilter(pExceptionPointers);
        if (prevResult != EXCEPTION_CONTINUE_SEARCH) {
            return prevResult;
        }
    }
    if (result == EXCEPTION_CONTINUE_EXECUTION) {
        return result;
    }

    TraceCrash("Unhandled exception consumed by crash handler");
    return EXCEPTION_EXECUTE_HANDLER;
}

void InstallCrashHandler() {
    // Prevent double-installation
    if (g_CrashHandlerInstalled) {
        TraceCrash("Crash handler already installed");
        return;
    }
    g_CrashHandlerInstalled = true;

    TraceCrash("Installing crash handler...");

    // Pre-load DbgHelp.dll to ensure it's available during a crash (avoid loader
    // lock issues). Load from System32 to prevent DLL hijacking.
    if (!g_hDbgHelp) {
        char sysDir[MAX_PATH];
        UINT len = GetSystemDirectoryA(sysDir, MAX_PATH);
        if (len > 0 && len < MAX_PATH - 16) {
            strcat(sysDir, "\\DbgHelp.dll");
            g_hDbgHelp = LoadLibraryA(sysDir);
        }
        if (!g_hDbgHelp) {
            g_hDbgHelp = LoadLibraryA("DbgHelp.dll");
        }
        if (g_hDbgHelp) {
            g_pMiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(g_hDbgHelp, "MiniDumpWriteDump");
            TraceCrash(g_pMiniDumpWriteDump ? "DbgHelp loaded successfully" : "Failed to get MiniDumpWriteDump");
        } else {
            TraceCrash("Failed to load DbgHelp.dll");
        }
    }

    // Disable Windows error dialogs and register with WER
    // This prevents Windows from showing crash dialogs and ensures our
    // crash handler has priority.
    RegisterWithWER();

    // Install Vectored Exception Handler (catches exceptions before SEH)
    PVOID vehHandle = AddVectoredExceptionHandler(1, CrashHandlerExceptionFilter);
    if (vehHandle) {
        TraceCrash("VEH handler installed");
    } else {
        TraceCrash("Failed to install VEH handler");
    }

    // Also install a SECOND VEH handler with LAST priority (0)
    // This catches exceptions that other VEH handlers might have skipped.
    // Some frameworks install VEH handlers that return EXCEPTION_CONTINUE_SEARCH
    // for crashes they don't recognize. Our last-position handler catches those.
    PVOID vehLastHandle = AddVectoredExceptionHandler(0, CrashHandlerExceptionFilter);
    if (vehLastHandle) {
        TraceCrash("VEH last-position handler installed");
    }

    // Also install Unhandled Exception Filter as backup
    // (some games might install their own handlers that preempt VEH)
    g_OldUnhandledFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterCallback);
    TraceCrash("Unhandled exception filter installed");

    OutputDebugStringA("[CrashHandler] Crash handler installed (VEH + VEH-last + UnhandledFilter).\n");
}
