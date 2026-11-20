#include "process_thread_walk.h"

#include <tlhelp32.h>

#include <atomic>

namespace ce::process_threads {

namespace {

// STATUS_NO_MORE_ENTRIES: the process-scoped walk reached its last thread.
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001AL);
// STATUS_ACCESS_DENIED: the next thread's DACL denies the requested rights.
constexpr LONG kStatusAccessDenied = static_cast<LONG>(0xC0000022L);
constexpr ACCESS_MASK kMinimalQueryAccess = THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

using PFN_NtGetNextThread = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

PFN_NtGetNextThread ResolveNtGetNextThread() {
    static PFN_NtGetNextThread cached = []() -> PFN_NtGetNextThread {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return nullptr;
        return reinterpret_cast<PFN_NtGetNextThread>(GetProcAddress(ntdll, "NtGetNextThread"));
    }();
    return cached;
}

std::atomic<bool> g_processScopedWalkUsable{true};

using PFN_NtQueryInformationThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
// THREADINFOCLASS values (winternl.h names only some of them).
constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
constexpr ULONG kThreadIsTerminated = 20;
constexpr ULONG kThreadSuspendCount = 35;

PFN_NtQueryInformationThread ResolveNtQueryInformationThread() {
    static PFN_NtQueryInformationThread cached = []() -> PFN_NtQueryInformationThread {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll)
            return nullptr;
        return reinterpret_cast<PFN_NtQueryInformationThread>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    }();
    return cached;
}

}  // namespace

bool PrepareThreadStateQueries() {
    return ResolveNtQueryInformationThread() != nullptr;
}

ThreadExitState QueryThreadExitState(HANDLE thread) {
    if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0)
        return ThreadExitState::kExited;
    const PFN_NtQueryInformationThread query = ResolveNtQueryInformationThread();
    ULONG terminated = 0;
    if (query && query(thread, kThreadIsTerminated, &terminated, sizeof(terminated), nullptr) >= 0 && terminated != 0)
        return ThreadExitState::kExiting;
    return ThreadExitState::kRunning;
}

bool QueryThreadSuspendCount(HANDLE thread, DWORD* count) {
    const PFN_NtQueryInformationThread query = ResolveNtQueryInformationThread();
    ULONG value = 0;
    if (!count || !query || query(thread, kThreadSuspendCount, &value, sizeof(value), nullptr) < 0)
        return false;
    *count = value;
    return true;
}

const void* QueryThreadStartAddress(HANDLE thread) {
    const PFN_NtQueryInformationThread query = ResolveNtQueryInformationThread();
    void* start = nullptr;
    if (!query || query(thread, kThreadQuerySetWin32StartAddress, static_cast<void*>(&start), sizeof(start), nullptr) < 0)
        return nullptr;
    return start;
}

bool ProcessScopedWalkAvailable() {
    return ResolveNtGetNextThread() != nullptr && g_processScopedWalkUsable.load(std::memory_order_acquire);
}

void ResetProcessScopedWalkAvailabilityForTesting() {
    g_processScopedWalkUsable.store(true, std::memory_order_release);
}

void SetProcessScopedWalkAvailableForTesting(bool available) {
    g_processScopedWalkUsable.store(available, std::memory_order_release);
}

WalkResult WalkCurrentProcessThreads(DWORD excludeThreadId, ACCESS_MASK desiredAccess, const ThreadVisitor& visit) {
    return WalkCurrentProcessThreads(
        excludeThreadId, desiredAccess, 0,
        [&visit](HANDLE threadHandle, DWORD threadId, bool) { return visit(threadHandle, threadId); });
}

WalkResult WalkCurrentProcessThreads(DWORD excludeThreadId, ACCESS_MASK desiredAccess, ACCESS_MASK fallbackAccess,
                                     const ThreadAccessVisitor& visit) {
    const PFN_NtGetNextThread ntGetNextThread = ResolveNtGetNextThread();
    if (!ntGetNextThread || !g_processScopedWalkUsable.load(std::memory_order_acquire))
        return WalkResult::kUnavailable;

    const HANDLE process = GetCurrentProcess();

    // The cursor must stay open across the next NtGetNextThread call, so the
    // handle handed to the visitor is a duplicate the visitor owns outright.
    HANDLE cursor = nullptr;
    for (;;) {
        HANDLE next = nullptr;
        bool grantedDesiredAccess = true;
        LONG status = ntGetNextThread(process, cursor, desiredAccess, 0, 0, &next);
        if (status == kStatusAccessDenied && fallbackAccess != 0) {
            // A peer thread's DACL denied desiredAccess (e.g. an anti-freeze
            // hook installer thread denying suspend/context rights). Retry from
            // the same cursor with reduced query access so the walk can inspect
            // and step past it without poisoning process-scoped enumeration.
            grantedDesiredAccess = false;
            status = ntGetNextThread(process, cursor, fallbackAccess, 0, 0, &next);
            if (status == kStatusAccessDenied && fallbackAccess != kMinimalQueryAccess) {
                status = ntGetNextThread(process, cursor, kMinimalQueryAccess, 0, 0, &next);
            }
        }
        if (cursor) {
            CloseHandle(cursor);
            cursor = nullptr;
        }
        if (status == kStatusNoMoreEntries)
            return WalkResult::kCompleted;
        if (status < 0 || !next) {
            // Never observed inside one's own process outside DACL-restricted
            // threads. Latch the route off rather than hand back a partial
            // thread set that a quiescence would treat as complete.
            g_processScopedWalkUsable.store(false, std::memory_order_release);
            return WalkResult::kUnavailable;
        }
        cursor = next;

        const DWORD threadId = GetThreadId(next);
        if (threadId == 0 || threadId == excludeThreadId)
            continue;

        HANDLE owned = nullptr;
        if (!DuplicateHandle(process, next, process, &owned, 0, FALSE, DUPLICATE_SAME_ACCESS) || !owned) {
            g_processScopedWalkUsable.store(false, std::memory_order_release);
            CloseHandle(cursor);
            return WalkResult::kUnavailable;
        }
        if (!visit(owned, threadId, grantedDesiredAccess)) {
            CloseHandle(cursor);
            return WalkResult::kAborted;
        }
    }
}

WalkResult WalkCurrentProcessThreadsViaSystemSnapshot(DWORD excludeThreadId, ACCESS_MASK desiredAccess,
                                                      const ThreadVisitor& visit) {
    return WalkCurrentProcessThreadsViaSystemSnapshot(
        excludeThreadId, desiredAccess, 0,
        [&visit](HANDLE threadHandle, DWORD threadId, bool) { return visit(threadHandle, threadId); });
}

WalkResult WalkCurrentProcessThreadsViaSystemSnapshot(DWORD excludeThreadId, ACCESS_MASK desiredAccess,
                                                      ACCESS_MASK fallbackAccess, const ThreadAccessVisitor& visit) {
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return WalkResult::kAborted;

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    WalkResult result = WalkResult::kCompleted;
    if (Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == excludeThreadId)
                continue;
            bool grantedDesiredAccess = true;
            HANDLE thread = OpenThread(desiredAccess, FALSE, entry.th32ThreadID);
            if (!thread) {
                const DWORD err = GetLastError();
                if (err == ERROR_INVALID_PARAMETER)
                    continue;  // Thread exited after the snapshot was taken.
                if (err == ERROR_ACCESS_DENIED && fallbackAccess != 0) {
                    grantedDesiredAccess = false;
                    thread = OpenThread(fallbackAccess, FALSE, entry.th32ThreadID);
                    if (!thread && GetLastError() == ERROR_ACCESS_DENIED && fallbackAccess != kMinimalQueryAccess) {
                        thread = OpenThread(kMinimalQueryAccess, FALSE, entry.th32ThreadID);
                    }
                    if (!thread) {
                        if (GetLastError() == ERROR_INVALID_PARAMETER)
                            continue;
                        result = WalkResult::kAborted;
                        break;
                    }
                } else {
                    result = WalkResult::kAborted;
                    break;
                }
            }
            if (!visit(thread, entry.th32ThreadID, grantedDesiredAccess)) {
                result = WalkResult::kAborted;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    } else {
        result = WalkResult::kAborted;
    }
    CloseHandle(snapshot);
    return result;
}

}  // namespace ce::process_threads
