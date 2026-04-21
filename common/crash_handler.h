#pragma once

#include <windows.h>
#include <dbghelp.h>
#include <string>

// Installs the unhandled exception filter to generate Minidumps
void InstallCrashHandler();

// Sets the directory where crash dumps will be written
void SetCrashDumpDirectory(const std::string& dir);

// Returns the current directory where crash dumps should be written.
std::string GetCrashDumpDirectory();

// Sets the process name for crash logging
void SetCrashProcessName(const char* name);

// Trace function for debugging the crash handler itself
void TraceCrash(const char* msg);

// Writes an additional CE-owned dump for externally handled crashes when we still
// have a live process handle and want a session-local artifact with CE's naming.
bool WriteSupplementalCrashDump(const char* fileNameHint, HANDLE hProcess, DWORD processId,
                                MINIDUMP_TYPE preferredDumpType,
                                PMINIDUMP_EXCEPTION_INFORMATION exceptionParam = nullptr,
                                PMINIDUMP_USER_STREAM_INFORMATION userStreamParam = nullptr,
                                PMINIDUMP_CALLBACK_INFORMATION callbackParam = nullptr);
