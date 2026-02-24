#include "crash_handler.h"
#include "logging.h"
#include <atomic>
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
static std::atomic<bool> g_DumpAlreadyWritten{false};
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
  snprintf(path, sizeof(path), "%s\\crash.log", g_DumpDir.c_str());
  FILE *f = fopen(path, "a");
  if (f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d][%s][%lu] %s\n", st.wHour, st.wMinute,
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

  // Skip known benign first-chance exceptions to avoid high-volume log spam.
  bool isKnownDebugException = (code == 0x406D1388 || // Thread naming
                                code == 0x40010006 || // OutputDebugString
                                code == 0x4001000A || // WOW64 debug
                                code == 0x4000001F || // Wow64 breakpoint
                                code == 0x80000003);  // Breakpoint (debug)
  if (code == 0xE06D7363 || isKnownDebugException) { // C++ throw/catch
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Log non-benign exceptions for debugging and crash triage.
  char codeStr[128];
  snprintf(codeStr, sizeof(codeStr),
           "VEH Exception: 0x%08lX at %p (PID:%lu TID:%lu)", code,
           pExceptionPointers->ExceptionRecord->ExceptionAddress,
           GetCurrentProcessId(), GetCurrentThreadId());
  TraceCrash(codeStr);

  // Also catch common crash types explicitly
  bool isKnownCrash =
      (code == EXCEPTION_ACCESS_VIOLATION ||
       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
       code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
       code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT ||
       code == EXCEPTION_PRIV_INSTRUCTION || code == 0x40000015 || // Abort
       code == 0xC0000409 ||                                       // Stack buffer overrun
       code == 0xC0000006 ||                                       // In-page I/O error
       code == 0xC000001D ||                                       // Illegal instruction
       code == 0xC0000025 ||                                       // Non-continuable exception
       code == 0xC0000374 ||                                       // Heap corruption
       code == 0xC00000FD ||                                       // Stack overflow (alt)
       code == 0x00008000 ||                                       // UE5 GPU crash (D3D device removed)
       code == 0x80000002 ||                                       // Guard page violation
       code == 0xC000013A ||                                       // Control-C/Control-Break
       code == 0xC0000142);                                        // DLL init failed

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
  if (code == EXCEPTION_ACCESS_VIOLATION &&
      pExceptionPointers->ExceptionRecord->NumberParameters >= 2) {
    ULONG_PTR accessType =
        pExceptionPointers->ExceptionRecord->ExceptionInformation[0];
    ULONG_PTR faultAddr =
        pExceptionPointers->ExceptionRecord->ExceptionInformation[1];
    char avDetail[256];
    snprintf(avDetail, sizeof(avDetail),
             "Access Violation: %s address 0x%p",
             accessType == 0 ? "READ from" : (accessType == 1 ? "WRITE to" : "DEP at"),
             (void *)faultAddr);
    TraceCrash(avDetail);
  }

  // Log crash address with module info
  {
    HMODULE hCrashMod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)pExceptionPointers->ExceptionRecord->ExceptionAddress,
                       &hCrashMod);
    char modName[MAX_PATH] = "unknown";
    if (hCrashMod)
      GetModuleFileNameA(hCrashMod, modName, MAX_PATH);

    char *modBaseName = strrchr(modName, '\\');
    modBaseName = modBaseName ? modBaseName + 1 : modName;

    char crashLoc[512];
    snprintf(crashLoc, sizeof(crashLoc),
             "Crash in module: %s at 0x%p (base=0x%p, offset=0x%llX)",
             modBaseName,
             pExceptionPointers->ExceptionRecord->ExceptionAddress,
             (void *)hCrashMod,
             hCrashMod ? (unsigned long long)((uintptr_t)pExceptionPointers->ExceptionRecord->ExceptionAddress - (uintptr_t)hCrashMod) : 0ULL);
    TraceCrash(crashLoc);
  }

  // Log key registers for post-mortem analysis
  {
    CONTEXT *ctx = pExceptionPointers->ContextRecord;
    char regBuf[512];
#ifdef _WIN64
    snprintf(regBuf, sizeof(regBuf),
             "Registers: RIP=0x%016llX RSP=0x%016llX RBP=0x%016llX "
             "RAX=0x%016llX RCX=0x%016llX RDX=0x%016llX",
             (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp,
             (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rax,
             (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
#else
    snprintf(regBuf, sizeof(regBuf),
             "Registers: EIP=0x%08X ESP=0x%08X EBP=0x%08X "
             "EAX=0x%08X ECX=0x%08X EDX=0x%08X",
             ctx->Eip, ctx->Esp, ctx->Ebp, ctx->Eax, ctx->Ecx, ctx->Edx);
#endif
    TraceCrash(regBuf);
  }

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
       code == 0xC0000409 ||                                       // Stack buffer overrun
       code == 0xC0000006 ||                                       // In-page I/O error
       code == 0xC000001D ||                                       // Illegal instruction
       code == 0xC0000025 ||                                       // Non-continuable exception
       code == 0xC0000374 ||                                       // Heap corruption
       code == 0xC00000FD ||                                       // Stack overflow (alt)
       code == 0x00008000 ||                                       // UE5 GPU crash (D3D device removed)
       code == 0x80000002 ||                                       // Guard page violation
       code == 0xC000013A ||                                       // Control-C/Control-Break
       code == 0xC0000142);                                        // DLL init failed

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
