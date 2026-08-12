#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#include <string>

// Installs the unhandled exception filter to generate Minidumps
void InstallCrashHandler();

// Sets the directory where crash dumps will be written.
//
// `archiveInstalledSymbols` also stages the installed PDBs/manifest into `dir\symbols` so a
// dump written there is analyzable later. Pass false while `dir` is only a provisional
// fallback — notably the logs ROOT, used before the session directory name exists. Archiving
// there drops a full symbol copy beside the per-session directories that nothing ever consumes
// (observed as a stray `installed\captureengine\logs\symbols`); the real session directory
// archives its own copy moments later.
void SetCrashDumpDirectory(const std::string& dir, bool archiveInstalledSymbols = true);

// Returns the current directory where crash dumps should be written.
std::string GetCrashDumpDirectory();

// Sets the process name for crash logging
void SetCrashProcessName(const char* name);

// Trace function for debugging the crash handler itself
void TraceCrash(const char* msg);

// Optional hook-module callback for recoverable execute faults such as lazy
// trampoline-pool DEP faults. Passing nullptr unregisters the handler.
using CrashExecutionFaultHandler = LONG (*)(EXCEPTION_POINTERS* pExceptionPointers, ULONG_PTR accessType,
                                            ULONG_PTR faultAddr);
void RegisterCrashExecutionFaultHandler(CrashExecutionFaultHandler handler);

// Writes an additional CE-owned dump for externally handled crashes when we still
// have a live process handle and want a session-local artifact with CE's naming.
bool WriteSupplementalCrashDump(const char* fileNameHint, HANDLE hProcess, DWORD processId,
                                MINIDUMP_TYPE preferredDumpType,
                                PMINIDUMP_EXCEPTION_INFORMATION exceptionParam = nullptr,
                                PMINIDUMP_USER_STREAM_INFORMATION userStreamParam = nullptr,
                                PMINIDUMP_CALLBACK_INFORMATION callbackParam = nullptr);

#ifdef CE_UNIT_TESTS
LONG DispatchCrashExecutionFaultHandlerForTesting(EXCEPTION_POINTERS* pExceptionPointers);
#endif
