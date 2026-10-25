#include "tool_thread_suspension.h"

#include "process_thread_walk.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>

void HookLog(const char* fmt, ...);

namespace ce::third_party_load {

bool ToolThreadSuspension::IsToolThread(const void* startAddress) const {
    const uintptr_t address = reinterpret_cast<uintptr_t>(startAddress);
    return std::any_of(moduleRanges_.begin(), moduleRanges_.end(), [&](const auto& range) {
        return address >= range.first && address < range.first + range.second;
    });
}

bool ToolThreadSuspension::Suspend() {
    const DWORD currentThreadId = GetCurrentThreadId();
    const DWORD processId = GetCurrentProcessId();
    // Before anything is suspended: resolving the query can take a loader lock.
    ce::process_threads::PrepareThreadStateQueries();
    // Two enumeration passes: the second catches tool threads that were
    // created while the first pass ran. A busy game keeps spawning its own
    // threads, so requiring a globally stable snapshot here would always
    // fail (session 20260813_033912 logged "could not suspend peer threads
    // cleanly" for exactly that reason). Only previously loaded tools'
    // threads are ever suspended; game and driver threads are filtered out
    // by their start address.
    for (int pass = 0; pass < 2; ++pass) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            ResumeAll();
            return false;
        }
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        bool enumerationOk = true;
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == currentThreadId)
                    continue;
                const bool alreadyTracked = std::any_of(threads_.begin(), threads_.end(), [&](const ToolThread& known) {
                    return known.threadId == entry.th32ThreadID;
                });
                if (alreadyTracked)
                    continue;
                // THREAD_QUERY_INFORMATION: the start-address query fails on a limited
                // handle, and from 2026-08-13 until this was found no tool thread was
                // ever recognized, so nothing was suspended.
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE,
                                           entry.th32ThreadID);
                if (!thread) {
                    if (GetLastError() == ERROR_INVALID_PARAMETER)
                        continue;  // Thread exited after the snapshot.
                    enumerationOk = false;
                    break;
                }
                const void* startAddress = ce::process_threads::QueryThreadStartAddress(thread);
                if (!startAddress || !IsToolThread(startAddress)) {
                    CloseHandle(thread);
                    continue;
                }
                threads_.push_back({thread, entry.th32ThreadID, false});
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!enumerationOk) {
            ResumeAll();
            return false;
        }
        for (size_t i = processedCount_; i < threads_.size(); ++i) {
            ToolThread& tool = threads_[i];
            if (SuspendThread(tool.handle) == static_cast<DWORD>(-1)) {
                // An exiting thread refuses suspension until it is gone and never runs user code again;
                // one another owner holds at the 127 maximum is stopped and its suspension is not ours.
                const bool exiting = ce::process_threads::QueryThreadExitState(tool.handle) !=
                                     ce::process_threads::ThreadExitState::kRunning;
                DWORD suspendCount = 0;
                const bool held = !exiting && ce::process_threads::QueryThreadSuspendCount(tool.handle, &suspendCount) &&
                                  suspendCount > 0;
                if (exiting || held) {
                    if (held) {
                        ++heldThreads_;
                        lastHeldThreadId_ = tool.threadId;
                    }
                    CloseHandle(tool.handle);
                    tool.handle = nullptr;
                    continue;
                }
                ResumeAll();
                return false;
            }
            tool.suspended = true;
        }
        processedCount_ = threads_.size();
    }
    return true;
}

size_t ToolThreadSuspension::SuspendedThreads() const {
    return static_cast<size_t>(
        std::count_if(threads_.begin(), threads_.end(), [](const ToolThread& tool) { return tool.suspended; }));
}

void ToolThreadSuspension::ResumeAll() {
    // Only suspensions this object made: a refused or held thread was never
    // ours, and resuming it would undo another owner's suspension.
    for (auto& tool : threads_) {
        if (tool.suspended)
            ResumeThread(tool.handle);
        if (tool.handle)
            CloseHandle(tool.handle);
    }
    threads_.clear();
    processedCount_ = 0;  // a retry's Suspend() must start over, not skip every re-found thread
    // Logged here, with every tool thread running again.
    if (heldThreads_ != 0) {
        static std::atomic<uint32_t> s_heldToolThreadLogs{0};
        if (s_heldToolThreadLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLog("ThirdParty preload: %u tool thread(s) already held suspended by another owner (last tid=%lu) - "
                    "treated as stopped, left suspended",
                    heldThreads_, static_cast<unsigned long>(lastHeldThreadId_));
        }
        heldThreads_ = 0;
    }
}

}  // namespace ce::third_party_load
