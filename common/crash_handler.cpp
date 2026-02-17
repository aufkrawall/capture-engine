#include "crash_handler.h"
#include "logging.h"
#include <cstdio>
#include <ctime>
#include <dbghelp.h>
#include <direct.h>
#include <errno.h>
#include <string>
#include <windows.h>

static std::string g_DumpDir = ".";
static char g_ProcessName[256] = "unknown";
static HMODULE g_hDbgHelp = NULL;
typedef BOOL(WINAPI *MINIDUMPWRITEDUMP)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
static MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

void SetCrashDumpDirectory(const std::string &dir) { g_DumpDir = dir; }

void SetCrashProcessName(const char *name) {
  if (name) {
    strncpy(g_ProcessName, name, sizeof(g_ProcessName) - 1);
    g_ProcessName[sizeof(g_ProcessName) - 1] = '\0';
  }
}

// Trace function for debugging the crash handler itself
void TraceCrash(const char *msg) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\crash.log",
           g_DumpDir.c_str());
  FILE *f = fopen(path, "a");
  if (f) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d][%s][%u] %s\n", st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, g_ProcessName, GetCurrentThreadId(),
            msg);
    fclose(f);
  }
}

struct DumpParams {
  EXCEPTION_POINTERS *pExceptionPointers;
  DWORD threadId;
};

// Worker thread to write minidump safely away from the crashed stack
DWORD WINAPI DumpWorker(LPVOID lpParam) {
  DumpParams *params = (DumpParams *)lpParam;

  TraceCrash("DumpWorker started");

  time_t now = time(0);
  struct tm tstruct;
  char buf[80];
  localtime_s(&tstruct, &now);
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tstruct);

  char dumpPath[MAX_PATH];
  snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", g_DumpDir.c_str(),
           buf);

  TraceCrash("Creating dump file...");
  TraceCrash(dumpPath);

  // Ensure directory exists with proper error checking
  int mkdirResult = _mkdir(g_DumpDir.c_str());
  if (mkdirResult == 0) {
    TraceCrash("Created dump directory");
  } else if (errno == EEXIST) {
    TraceCrash("Dump directory already exists");
  } else {
    TraceCrash("Failed to create dump directory!");
  }

  HANDLE hFile = CreateFileA(
      dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);

  if (hFile != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = params->threadId;
    mdei.ExceptionPointers = params->pExceptionPointers;
    mdei.ClientPointers = FALSE;

    MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs |
                                        MiniDumpWithIndirectlyReferencedMemory);

    DWORD written = 0;
    const char *header = "CRASH_DUMP_START\r\n";
    WriteFile(hFile, header, (DWORD)strlen(header), &written, NULL);
    FlushFileBuffers(hFile);

    TraceCrash("Calling MiniDumpWriteDump from worker thread...");

    BOOL rv = FALSE;
    if (g_pMiniDumpWriteDump) {
      rv = g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, mdt, &mdei, 0, 0);
    } else {
      TraceCrash("g_pMiniDumpWriteDump is NULL in worker!");
    }

    if (rv) {
      TraceCrash("MiniDumpWriteDump Success");
      FlushFileBuffers(hFile);
      char msg[256];
      snprintf(msg, sizeof(msg), "[CrashHandler] Minidump created at: %s\n",
               dumpPath);
      OutputDebugStringA(msg);
    } else {
      TraceCrash("MiniDumpWriteDump Failed");
      char msg[256];
      DWORD err = GetLastError();
      snprintf(msg, sizeof(msg),
               "[CrashHandler] MiniDumpWriteDump failed: %lu (0x%08lX)\n", err,
               err);
      OutputDebugStringA(msg);

      char errPath[MAX_PATH];
      snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt",
               g_DumpDir.c_str());
      FILE *f = fopen(errPath, "w");
      if (f) {
        fprintf(
            f,
            "MiniDumpWriteDump failed. Error: %lu (0x%08lX)\nDump Path: %s\n",
            err, err, dumpPath);
        fclose(f);
      }
    }
    CloseHandle(hFile);
  } else {
    TraceCrash("Failed to create dump file");
  }

  return 0;
}

LONG WINAPI
CrashHandlerExceptionFilter(EXCEPTION_POINTERS *pExceptionPointers) {
  DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

  // CRITICAL: Early-exit for C++ exceptions (0xE06D7363) to prevent log spam
  // These are normal throw/catch operations handled by the C++ runtime
  // Logging them fills crash.log with 12MB+ of useless data
  if (code == 0xE06D7363) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Log ALL exceptions for debugging (including non-crash)
  char codeStr[128];
  snprintf(codeStr, sizeof(codeStr),
           "VEH Exception: 0x%08lX at %p (PID:%lu TID:%lu)", code,
           pExceptionPointers->ExceptionRecord->ExceptionAddress,
           GetCurrentProcessId(), GetCurrentThreadId());
  TraceCrash(codeStr);

  // Catch ALL exception types except debug/informational
  // Many games use custom exception codes or we might miss unknown crash codes
  bool isKnownDebugException = (code == 0x406D1388 || // Thread naming
                                code == 0x40010006 || // OutputDebugString
                                code == 0x4001000A || // WOW64 debug
                                code == 0x4000001F || // Wow64 breakpoint
                                code == 0x80000003);  // Breakpoint (debug)

  // Also catch common crash types explicitly
  bool isKnownCrash =
      (code == EXCEPTION_ACCESS_VIOLATION ||
       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
       code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT ||
       code == EXCEPTION_PRIV_INSTRUCTION || code == 0x40000015 || // Abort
       code == 0xC0000409 || // Stack buffer overrun
       code == 0xC0000006 || // In-page I/O error
       code == 0xC000001D || // Illegal instruction
       code == 0xC0000025 || // Non-continuable exception
       code == 0xC0000374 || // Heap corruption
       code == 0xC00000FD || // Stack overflow (alt)
       code == 0x00008000);  // UE5 GPU crash (D3D device removed)

  // If it's a known debug exception, skip it
  if (isKnownDebugException) {
    TraceCrash("Debug exception, continuing search");
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // For unknown exceptions, log them but only handle if it looks like a crash
  // (first chance exceptions are often caught and handled by the app)
  if (!isKnownCrash) {
    // Unknown exception - could be a custom crash code
    // Log it but don't handle unless it's truly fatal
    TraceCrash("Unknown exception code - continuing search (first chance)");
    // Note: If the app doesn't handle this, it will come back as a second
    // chance Unfortunately VEH doesn't easily distinguish first/second chance
    return EXCEPTION_CONTINUE_SEARCH;
  }

  TraceCrash("CRASH DETECTED - Handling exception");


  // Ensure trace log goes to the correct dir
  TraceCrash("CrashHandlerExceptionFilter entered");

  char bufCode[64];
  snprintf(bufCode, sizeof(bufCode), "Exception Code: 0x%08X", code);
  TraceCrash(bufCode);

  OutputDebugStringA(
      "[CrashHandler] CRASH DETECTED! Spawning worker for minidump...\n");

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
    DumpWorker(&params); // Fallback to inline if thread creation fails
  }

  TraceCrash("Handler finished - Returning EXCEPTION_CONTINUE_SEARCH");
  return EXCEPTION_CONTINUE_SEARCH;
}

static bool g_CrashHandlerInstalled = false;

static LPTOP_LEVEL_EXCEPTION_FILTER g_OldUnhandledFilter = NULL;

LONG WINAPI
UnhandledExceptionFilterCallback(EXCEPTION_POINTERS *pExceptionPointers) {
  DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;

  char codeStr[128];
  snprintf(codeStr, sizeof(codeStr), "UnhandledExceptionFilter: 0x%08lX at %p",
           code, pExceptionPointers->ExceptionRecord->ExceptionAddress);
  TraceCrash(codeStr);

  // Always handle crashes here
  bool isKnownCrash =
      (code == EXCEPTION_ACCESS_VIOLATION ||
       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
       code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT ||
       code == EXCEPTION_PRIV_INSTRUCTION || code == 0x40000015 || // Abort
       code == 0xC0000409 || // Stack buffer overrun
       code == 0xC0000006 || // In-page I/O error
       code == 0xC000001D || // Illegal instruction
       code == 0xC0000025 || // Non-continuable exception
       code == 0xC0000374 || // Heap corruption
       code == 0xC00000FD);  // Stack overflow (alt)

  if (isKnownCrash) {
    TraceCrash("Unhandled exception is a crash - handling it");
    // Call the VEH handler to do the actual dump
    return CrashHandlerExceptionFilter(pExceptionPointers);
  }

  // Call previous filter if any
  if (g_OldUnhandledFilter) {
    return g_OldUnhandledFilter(pExceptionPointers);
  }
  return EXCEPTION_CONTINUE_SEARCH;
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
      g_pMiniDumpWriteDump =
          (MINIDUMPWRITEDUMP)GetProcAddress(g_hDbgHelp, "MiniDumpWriteDump");
      TraceCrash(g_pMiniDumpWriteDump ? "DbgHelp loaded successfully"
                                      : "Failed to get MiniDumpWriteDump");
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
  g_OldUnhandledFilter =
      SetUnhandledExceptionFilter(UnhandledExceptionFilterCallback);
  TraceCrash("Unhandled exception filter installed");

  OutputDebugStringA(
      "[CrashHandler] Crash handler installed (VEH + UnhandledFilter).\n");
}
