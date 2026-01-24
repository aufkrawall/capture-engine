#include <cstdio>
#include <ctime>
#include <string>
#include <windows.h>
#include <dbghelp.h>
#include <direct.h>
#include "crash_handler.h"
#include "logging.h"

static std::string g_DumpDir = ".";
static HMODULE g_hDbgHelp = NULL;
typedef BOOL(WINAPI *MINIDUMPWRITEDUMP)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);
static MINIDUMPWRITEDUMP g_pMiniDumpWriteDump = NULL;

void SetCrashDumpDirectory(const std::string &dir) {
  g_DumpDir = dir;
}

LONG WINAPI CrashHandlerExceptionFilter(EXCEPTION_POINTERS *pExceptionPointers) {
  DWORD code = pExceptionPointers->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && 
      code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_STACK_OVERFLOW) {
      return EXCEPTION_CONTINUE_SEARCH;
  }

  OutputDebugStringA("[CrashHandler] CRASH DETECTED! Generating minidump...\n");

  if (!g_pMiniDumpWriteDump) {
      OutputDebugStringA("[CrashHandler] Failed: DbgHelp not loaded.\n");
      return EXCEPTION_CONTINUE_SEARCH;
  }

  time_t now = time(0);
  struct tm tstruct;
  char buf[80];
  localtime_s(&tstruct, &now);
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tstruct);

  char dumpPath[MAX_PATH];
  snprintf(dumpPath, sizeof(dumpPath), "%s\\crash_%s.dmp", g_DumpDir.c_str(), buf);

  _mkdir(g_DumpDir.c_str());

  HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  if (hFile != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = pExceptionPointers;
    mdei.ClientPointers = FALSE;

    MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);

    BOOL rv = g_pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, mdt, &mdei, 0, 0);

    if (rv) {
      char msg[256];
      snprintf(msg, sizeof(msg), "[CrashHandler] Minidump created at: %s\n", dumpPath);
      OutputDebugStringA(msg);
    } else {
      char msg[256];
      DWORD err = GetLastError();
      snprintf(msg, sizeof(msg), "[CrashHandler] MiniDumpWriteDump failed: %d (0x%08X)\n", err, err);
      OutputDebugStringA(msg);
      
      // Fallback: Write error to text file
      char errPath[MAX_PATH];
      snprintf(errPath, sizeof(errPath), "%s\\crash_error.txt", g_DumpDir.c_str());
      FILE* f = fopen(errPath, "w");
      if (f) {
          fprintf(f, "MiniDumpWriteDump failed. Error: %d (0x%08X)\nDump Path: %s\n", err, err, dumpPath);
          fclose(f);
      }
    }

    CloseHandle(hFile);
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
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
