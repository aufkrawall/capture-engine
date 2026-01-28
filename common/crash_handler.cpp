#include "crash_handler.h"
#include <dbghelp.h>
#include <direct.h>
#include <windows.h>
#include <cstdio>
#include <ctime>
#include <string>
#include "logging.h"

static std::string g_DumpDir = ".";
static HMODULE g_hDbgHelp = NULL;
typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
static MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

void SetCrashDumpDirectory(const std::string& dir) { g_DumpDir = dir; }

// Trace function for debugging the crash handler itself
void TraceCrash(const char* msg)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\crash_handler_trace.txt", g_DumpDir.c_str());
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "[%u] %s\n", GetCurrentThreadId(), msg);
        fclose(f);
    }
}

struct DumpParams {
    EXCEPTION_POINTERS* pExceptionPointers;
    DWORD threadId;
};

// Worker thread to write minidump safely away from the crashed stack
DWORD WINAPI DumpWorker(LPVOID lpParam)
{
    DumpParams* params = (DumpParams*)lpParam;

    TraceCrash("DumpWorker started");

    time_t now = time(0);
    struct tm tstruct;
    char buf[80];
    localtime_s(&tstruct, &now);
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tstruct);

    char dumpPath[MAX_PATH];
    snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", g_DumpDir.c_str(), buf);

    TraceCrash("Creating dump file...");
    _mkdir(g_DumpDir.c_str());

    HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = params->threadId;
        mdei.ExceptionPointers = params->pExceptionPointers;
        mdei.ClientPointers = FALSE;

        MINIDUMP_TYPE mdt =
            (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);

        DWORD written = 0;
        const char* header = "CRASH_DUMP_START\r\n";
        WriteFile(hFile, header, (DWORD)strlen(header), &written, NULL);
        FlushFileBuffers(hFile);

        TraceCrash("Calling MiniDumpWriteDump from worker thread...");

        BOOL rv = FALSE;
        if (g_pMiniDumpWriteDump) {
            rv = g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, mdt, &mdei, 0, 0);
        } else {
            TraceCrash("g_pMiniDumpWriteDump is NULL in worker!");
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
            DWORD err = GetLastError();
            snprintf(msg, sizeof(msg), "[CrashHandler] MiniDumpWriteDump failed: %d (0x%08X)\n", err, err);
            OutputDebugStringA(msg);

            char errPath[MAX_PATH];
            snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt", g_DumpDir.c_str());
            FILE* f = fopen(errPath, "w");
            if (f) {
                fprintf(f, "MiniDumpWriteDump failed. Error: %d (0x%08X)\nDump Path: %s\n", err, err, dumpPath);
                fclose(f);
            }
        }
        CloseHandle(hFile);
    } else {
        TraceCrash("Failed to create dump file");
    }

    return 0;
}

LONG WINAPI CrashHandlerExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers)
{
    DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Ensure trace log goes to the correct dir
    TraceCrash("CrashHandlerExceptionFilter entered");

    char bufCode[64];
    snprintf(bufCode, sizeof(bufCode), "Exception Code: 0x%08X", code);
    TraceCrash(bufCode);

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
        TraceCrash("Worker thread spawned, waiting...");
        WaitForSingleObject(hThread, INFINITE);
        TraceCrash("Worker thread finished.");
        CloseHandle(hThread);
    } else {
        TraceCrash("Failed to create worker thread! Attempting inline dump...");
        DumpWorker(&params);  // Fallback to inline if thread creation fails
    }

    TraceCrash("Handler finished - Returning EXCEPTION_CONTINUE_SEARCH");
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    // Pre-load DbgHelp.dll to ensure it's available during a crash (avoid loader lock issues)
    if (!g_hDbgHelp) {
        g_hDbgHelp = LoadLibraryA("DbgHelp.dll");
        if (g_hDbgHelp) {
            g_pMiniDumpWriteDump = (MINIDUMPWRITEDUMP)GetProcAddress(g_hDbgHelp, "MiniDumpWriteDump");
        }
    }

    AddVectoredExceptionHandler(1, CrashHandlerExceptionFilter);
    OutputDebugStringA("[CrashHandler] VEH Crash handler installed.\n");
}
