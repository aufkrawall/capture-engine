#pragma once

// Private surface shared between crash_handler.cpp (state, dump directory,
// symbol archiving, WER registration, tracing) and crash_dump_writer.cpp (the
// dump worker and the exception filters). Nothing here is part of the public
// crash-handler API; include crash_handler.h for that.

#include "crash_handler.h"

#include <atomic>
#include <mutex>
#include <string>

typedef BOOL(WINAPI* MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                        PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                        PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                        PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

// Defined in crash_handler.cpp.
extern std::mutex g_DumpDirMutex;
extern HMODULE g_hDbgHelp;
extern std::atomic<bool> g_DumpAttemptInProgress;
extern std::atomic<bool> g_DumpSuccessfullyWritten;
extern std::atomic<bool> g_ForceUnhandledDump;
extern std::atomic<int> g_VEHCallCount;
extern std::atomic<int> g_RPCDisconnectedExceptionCount;
extern std::atomic<int> g_RPCServerUnavailableExceptionCount;
extern std::atomic<int> g_ENoInterfaceExceptionCount;
extern MINIDUMPWRITEDUMP g_pMiniDumpWriteDump;

// Dump-directory storage. Callers must hold g_DumpDirMutex.
std::string& CrashDumpDirectoryStorage();

// Offers an execute-fault access violation to the registered hook-module
// handler; returns EXCEPTION_CONTINUE_SEARCH when nobody claims it.
LONG DispatchCrashExecutionFaultHandler(EXCEPTION_POINTERS* pExceptionPointers);

// Registered CrashDumpEnvironmentHooks accessors. Each answers conservatively
// (false) when the hook module registered nothing.
bool HasExternalCrashDumpCapture();
bool CaptureCrashDumpWithExternalHelper(const char* dumpFileNameHint);
bool IsForeignOverlayLoadedForCrashDump();

// Renames a finished .inprogress dump to its final name, falling back to a copy.
// Sets *preservedTempDump when the temporary file had to be left in place.
bool PromoteInProgressDumpFile(const char* tempDumpPath, const char* dumpPath, const char* traceContext,
                               bool* preservedTempDump);

int IncrementExceptionCount(std::atomic<int>& counter);
void ActivateCrashTrace();
void RegisterWithWER();
