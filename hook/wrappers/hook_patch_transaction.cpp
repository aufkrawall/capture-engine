#include "hook_patch_transaction.h"

#include <tlhelp32.h>
#include <algorithm>

namespace ce::hook_patch {

ThreadQuiescence::ThreadQuiescence(const void* patchAddress, size_t patchSize) {
    if (!patchAddress || patchSize == 0)
        return;

    const DWORD processId = GetCurrentProcessId();
    const DWORD currentThreadId = GetCurrentThreadId();
    try {
        threads_.reserve(1024);
    } catch (...) {
        return;
    }

    bool stableSnapshot = false;
    for (int pass = 0; pass < 4; ++pass) {
        const size_t previousCount = threads_.size();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return;

        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);
        bool enumerationSucceeded = Thread32First(snapshot, &entry) != FALSE;
        if (enumerationSucceeded) {
            do {
                if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == currentThreadId)
                    continue;
                const bool tracked = std::any_of(threads_.begin(), threads_.end(), [&](const SuspendedThread& thread) {
                    return thread.handle && thread.threadId == entry.th32ThreadID;
                });
                if (tracked)
                    continue;
                if (threads_.size() == threads_.capacity()) {
                    enumerationSucceeded = false;
                    break;
                }
                HANDLE thread =
                    OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                               FALSE, entry.th32ThreadID);
                if (!thread) {
                    if (GetLastError() == ERROR_INVALID_PARAMETER)
                        continue;  // Thread exited after the snapshot.
                    enumerationSucceeded = false;
                    break;
                }
                threads_.push_back({thread, entry.th32ThreadID, false});
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!enumerationSucceeded)
            return;

        for (size_t i = previousCount; i < threads_.size(); ++i) {
            auto& thread = threads_[i];
            if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) {
                    CloseHandle(thread.handle);
                    thread.handle = nullptr;
                    continue;
                }
                return;
            }
            thread.suspended = true;
        }
        if (threads_.size() == previousCount) {
            stableSnapshot = true;
            break;
        }
    }
    if (!stableSnapshot)
        return;

    const uintptr_t patchStart = reinterpret_cast<uintptr_t>(patchAddress);
    for (const auto& thread : threads_) {
        if (!thread.handle)
            continue;
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread.handle, &context)) {
            if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0)
                continue;
            return;
        }
#ifdef _WIN64
        const uintptr_t instructionPointer = static_cast<uintptr_t>(context.Rip);
#else
        const uintptr_t instructionPointer = static_cast<uintptr_t>(context.Eip);
#endif
        if (IsInstructionPointerInsidePatchRange(instructionPointer, patchStart, patchSize))
            return;
    }
    ready_ = true;
}

ThreadQuiescence::~ThreadQuiescence() {
    for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) {
        if (it->suspended)
            ResumeThread(it->handle);
        if (it->handle)
            CloseHandle(it->handle);
    }
}

}  // namespace ce::hook_patch
