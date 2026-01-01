#include "crash_handler.h"
#include <ctime>
#include <dbghelp.h> // Still need header for MINIDUMP types
#include <direct.h>  // For _mkdir
#include <iostream>
#include <string>
#include <windows.h>

static std::string g_DumpDir = ".";

// Typedef for MiniDumpWriteDump function pointer
typedef BOOL(WINAPI *MINIDUMPWRITEDUMP)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

void SetCrashDumpDirectory(const std::string &dir) {
  g_DumpDir = dir;
  // Don't create directory here - will create on-demand when crash occurs
}

LONG WINAPI
CrashHandlerExceptionFilter(EXCEPTION_POINTERS *pExceptionPointers) {
  std::cout << "[CrashHandler] CRASH DETECTED! Generating minidump..."
            << std::endl;

  time_t now = time(0);
  struct tm tstruct;
  char buf[80];
  localtime_s(&tstruct, &now);
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tstruct);

  std::string dumpPath = g_DumpDir + "\\crash_" + std::string(buf) + ".dmp";

  HMODULE hDbgHelp = LoadLibraryA("DbgHelp.dll");
  if (!hDbgHelp) {
    std::cerr << "[CrashHandler] Failed to load DbgHelp.dll" << std::endl;
    return EXCEPTION_CONTINUE_SEARCH;
  }

  MINIDUMPWRITEDUMP pMiniDumpWriteDump =
      (MINIDUMPWRITEDUMP)GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
  if (!pMiniDumpWriteDump) {
    std::cerr
        << "[CrashHandler] Failed to find MiniDumpWriteDump in DbgHelp.dll"
        << std::endl;
    FreeLibrary(hDbgHelp);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Create dump directory on demand (only when crash actually occurs)
  _mkdir(g_DumpDir.c_str());

  HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  if ((hFile != NULL) && (hFile != INVALID_HANDLE_VALUE)) {
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = pExceptionPointers;
    mdei.ClientPointers = FALSE;

    MINIDUMP_TYPE mdt = MiniDumpNormal;

    BOOL rv =
        pMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                           mdt, (pExceptionPointers != 0) ? &mdei : 0, 0, 0);

    if (rv) {
      std::cout << "[CrashHandler] Minidump created at: " << dumpPath
                << std::endl;
    } else {
      std::cerr << "[CrashHandler] MiniDumpWriteDump failed. Error: "
                << GetLastError() << std::endl;
    }

    CloseHandle(hFile);
  } else {
    std::cerr << "[CrashHandler] Failed to create dump file: " << dumpPath
              << std::endl;
  }

  FreeLibrary(hDbgHelp);

  return EXCEPTION_EXECUTE_HANDLER; // Let Windows handle (or terminate)
}

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(CrashHandlerExceptionFilter);
  std::cout << "[CrashHandler] Crash handler installed." << std::endl;
}
