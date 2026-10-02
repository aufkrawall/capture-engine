#include "process_thread_walk.h"

#include <tlhelp32.h>

#include <atomic>

namespace ce::process_threads {

namespace {

// STATUS_NO_MORE_ENTRIES: the process-scoped walk reached its last thread.
constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001AL);

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

}  // namespace

bool ProcessScopedWalkAvailable() {
    return ResolveNtGetNextThread() != nullptr && g_processScopedWalkUsable.load(std::memory_order_acquire);
}

void ResetProcessScopedWalkAvailabilityForTesting() {
    g_processScopedWalkUsable.store(true, std::memory_order_release);
}

WalkResult WalkCurrentProcessThreads(DWORD excludeThreadId, ACCESS_MASK desiredAccess, const ThreadVisitor& visit) {
    const PFN_NtGetNextThread ntGetNextThread = ResolveNtGetNextThread();
    if (!ntGetNextThread || !g_processScopedWalkUsable.load(std::memory_order_acquire))
        return WalkResult::kUnavailable;

    const HANDLE process = GetCurrentProcess();

    // The cursor must stay open across the next NtGetNextThread call, so the
    // handle handed to the visitor is a duplicate the visitor owns outright.
    HANDLE cursor = nullptr;
    for (;;) {
        HANDLE next = nullptr;
        const LONG status = ntGetNextThread(process, cursor, desiredAccess, 0, 0, &next);
        if (cursor) {
            CloseHandle(cursor);
            cursor = nullptr;
        }
        if (status == kStatusNoMoreEntries)
            return WalkResult::kCompleted;
        if (status < 0 || !next) {
            // Never observed inside one's own process. Latch the route off
            // rather than hand back a partial thread set that a quiescence
            // would treat as complete.
            g_processScopedWalkUsable.store(false, std::memory_order_release);
            return WalkResult::kUnavailable;
        }
        cursor = next;

        const DWORD threadId = GetThreadId(next);
        if (threadId == 0 || threadId == excludeThreadId)
            continue;

        HANDLE owned = nullptr;
        if (!DuplicateHandle(process, next, process, &owned, desiredAccess, FALSE, 0) || !owned) {
            g_processScopedWalkUsable.store(false, std::memory_order_release);
            CloseHandle(cursor);
            return WalkResult::kUnavailable;
        }
        if (!visit(owned, threadId)) {
            CloseHandle(cursor);
            return WalkResult::kAborted;
        }
    }
}

WalkResult WalkCurrentProcessThreadsViaSystemSnapshot(DWORD excludeThreadId, ACCESS_MASK desiredAccess,
                                                      const ThreadVisitor& visit) {
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
            HANDLE thread = OpenThread(desiredAccess, FALSE, entry.th32ThreadID);
            if (!thread) {
                if (GetLastError() == ERROR_INVALID_PARAMETER)
                    continue;  // Thread exited after the snapshot was taken.
                result = WalkResult::kAborted;
                break;
            }
            if (!visit(thread, entry.th32ThreadID)) {
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
