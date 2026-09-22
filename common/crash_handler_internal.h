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
extern std::atomic<DWORD> g_DumpDirMutexOwnerThread;

// Takes a mutex unless the calling thread already holds it, in which case it
// proceeds unlocked. Windows re-enters a vectored exception handler on the same
// thread when a fault is raised while the handler is running, so a plain lock
// on the crash path is a lock a thread can end up waiting on for itself.
// Gothic II session 20260916_011148 froze there: the render thread overflowed
// its stack, CE's filter ran, and TraceCrash blocked in pthread_mutex_lock with
// crash.log still empty - the stack-overflow dump that would have named the
// recursion was never written.
//
// Only correct where the protected work is safe to re-enter on one thread:
// appending to crash.log and reading the dump directory both are.
class ExceptionSafeLock {
public:
    ExceptionSafeLock(std::mutex& mutex, std::atomic<DWORD>& owner) : mutex_(mutex), owner_(owner) {
        const DWORD self = GetCurrentThreadId();
        if (owner_.load(std::memory_order_acquire) == self) {
            return;
        }
        mutex_.lock();
        held_ = true;
        owner_.store(self, std::memory_order_release);
    }

    ~ExceptionSafeLock() {
        if (!held_) {
            return;
        }
        owner_.store(0, std::memory_order_release);
        mutex_.unlock();
    }

    ExceptionSafeLock(const ExceptionSafeLock&) = delete;
    ExceptionSafeLock& operator=(const ExceptionSafeLock&) = delete;

private:
    std::mutex& mutex_;
    std::atomic<DWORD>& owner_;
    bool held_ = false;
};
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

// Runs the registered CrashPreDumpCallback, if any. Called by the fatal path
// once per dump attempt, before any thread of this process is suspended.
void NotifyCrashPreDump();

// The registered CrashDumpEnvironmentHooks accessors now live in the public
// crash_handler.h, because the freeze watchdog needs the same decision.

// Renames a finished .inprogress dump to its final name, falling back to a copy.
// Sets *preservedTempDump when the temporary file had to be left in place.
bool PromoteInProgressDumpFile(const char* tempDumpPath, const char* dumpPath, const char* traceContext,
                               bool* preservedTempDump);

int IncrementExceptionCount(std::atomic<int>& counter);
void ActivateCrashTrace();
void RegisterWithWER();
