#include "crash_handler.h"
#include <dbghelp.h>
#include <direct.h>
#include <errno.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include "logging.h"

static std::string g_DumpDir = ".\\logs";
static char g_ProcessName[256] = "unknown";
static HMODULE g_hDbgHelp = NULL;
static std::atomic<bool> g_DumpAlreadyWritten{false};
static thread_local bool g_ForceUnhandledDump = false;
typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
static MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

void SetCrashDumpDirectory(const std::string& dir) {
    g_DumpDir = dir;
}

void SetCrashProcessName(const char* name) {
    if (name) {
        strncpy(g_ProcessName, name, sizeof(g_ProcessName) - 1);
        g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
    }
}

// Trace function for debugging the crash handler itself
void TraceCrash(const char* msg) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\crash.log", g_DumpDir.c_str());
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
    DumpParams* params = (DumpParams*)lpParam;

    TraceCrash("DumpWorker started");

    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[128];
    snprintf(buf, sizeof(buf), "%04u%02u%02u_%02u%02u%02u_%03u_pid%lu_tid%lu", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), params->threadId);

    char dumpPath[MAX_PATH];
    snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", g_DumpDir.c_str(), buf);

    TraceCrash("Creating dump file...");
    TraceCrash(dumpPath);

    // Ensure directory exists with proper error checking
    if (CreateDirectoryA(g_DumpDir.c_str(), NULL)) {
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
            snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt", g_DumpDir.c_str());
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

    // Skip known benign first-chance exceptions to avoid high-volume log spam.
    bool forceUnhandledDump = g_ForceUnhandledDump;
    bool isKnownDebugException = (code == 0x406D1388 ||  // Thread naming
                                  code == 0x40010006 ||  // OutputDebugString
                                  code == 0x4001000A ||  // WOW64 debug
                                  code == 0x4000001F ||  // Wow64 breakpoint
                                  code == 0x80000003);   // Breakpoint (debug)
    if (isKnownDebugException) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == 0xE06D7363 && !forceUnhandledDump) {  // C++ throw/catch
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Log non-benign exceptions for debugging and crash triage.
    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "VEH Exception: 0x%08lX at %p (PID:%lu TID:%lu)", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress, GetCurrentProcessId(), GetCurrentThreadId());
    TraceCrash(codeStr);

    // Also catch common crash types explicitly
    bool isKnownCrash =
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT ||
         code == EXCEPTION_PRIV_INSTRUCTION || code == 0x40000015 ||  // Abort
         code == 0xC0000409 ||                                        // Stack buffer overrun
         code == 0xC0000006 ||                                        // In-page I/O error
         code == 0xC000001D ||                                        // Illegal instruction
         code == 0xC0000025 ||                                        // Non-continuable exception
         code == 0xC0000374 ||                                        // Heap corruption
         code == 0xC00000FD ||                                        // Stack overflow (alt)
         code == 0x00008000 ||                                        // UE5 GPU crash (D3D device removed)
         code == 0x00004000 ||                                        // UE5 fatal assertion (check/ensure)
         code == 0x80000002 ||                                        // Guard page violation
         code == 0xC000013A ||                                        // Control-C/Control-Break
         code == 0xC0000142);                                         // DLL init failed

    // For unknown exceptions, log them but only handle if it looks like a crash
    // (first chance exceptions are often caught and handled by the app)
    if (!isKnownCrash && !forceUnhandledDump) {
        // Unknown exception - could be a custom crash code
        // Log it but don't handle unless it's truly fatal
        TraceCrash("Unknown exception code - continuing search (first chance)");
        // Note: If the app doesn't handle this, it will come back as a second
        // chance Unfortunately VEH doesn't easily distinguish first/second chance
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!isKnownCrash && forceUnhandledDump) {
        TraceCrash("Unhandled non-standard exception code - forcing dump generation");
    }

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
                 "RAX=0x%016llX RCX=0x%016llX RDX=0x%016llX",
                 (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp,
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
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
        TraceCrash("g_pMiniDumpWriteDump is NULL");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DumpParams params;
    params.pExceptionPointers = pExceptionPointers;
    params.threadId = GetCurrentThreadId();

    // Spawn thread to handle dump writing (crucial for Stack Overflow exceptions)
    HANDLE hThread = CreateThread(NULL, 0, DumpWorker, &params, 0, NULL);

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
        DumpWorker(&params);  // Fallback to inline if thread creation fails
    }

    TraceCrash("Handler finished - Returning EXCEPTION_CONTINUE_SEARCH");
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_CrashHandlerInstalled = false;

static LPTOP_LEVEL_EXCEPTION_FILTER g_OldUnhandledFilter = NULL;

LONG WINAPI UnhandledExceptionFilterCallback(EXCEPTION_POINTERS* pExceptionPointers) {
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

    char codeStr[128];
    snprintf(codeStr, sizeof(codeStr), "UnhandledExceptionFilter: 0x%08lX at %p", code,
             pExceptionPointers->ExceptionRecord->ExceptionAddress);
    TraceCrash(codeStr);

    TraceCrash("Unhandled exception reached top-level filter - forcing dump");
    g_ForceUnhandledDump = true;
    LONG result = CrashHandlerExceptionFilter(pExceptionPointers);
    g_ForceUnhandledDump = false;
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
    // lock issues)
    if (!g_hDbgHelp) {
        g_hDbgHelp = LoadLibraryA("DbgHelp.dll");
        if (g_hDbgHelp) {
            g_pMiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(g_hDbgHelp, "MiniDumpWriteDump");
            TraceCrash(g_pMiniDumpWriteDump ? "DbgHelp loaded successfully" : "Failed to get MiniDumpWriteDump");
        } else {
            TraceCrash("Failed to load DbgHelp.dll");
        }
    }

    // Install Vectored Exception Handler (catches exceptions before SEH)
    PVOID vehHandle = AddVectoredExceptionHandler(1, CrashHandlerExceptionFilter);
    if (vehHandle) {
        TraceCrash("VEH handler installed");
    } else {
        TraceCrash("Failed to install VEH handler");
    }

    // Also install Unhandled Exception Filter as backup
    // (some games might install their own handlers that preempt VEH)
    g_OldUnhandledFilter = SetUnhandledExceptionFilter(UnhandledExceptionFilterCallback);
    TraceCrash("Unhandled exception filter installed");

    OutputDebugStringA("[CrashHandler] Crash handler installed (VEH + UnhandledFilter).\n");
}
