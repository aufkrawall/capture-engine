// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <atomic>
#include <mutex>

#include "iat_hook.h"
#include "iat_import_table.h"

namespace IATHook::detail {

inline bool WriteBaselineChange(const BaselineChange& change) {
    DWORD protection = 0;
    if (!VirtualProtect(static_cast<void*>(change.entry), sizeof(void*), PAGE_READWRITE, &protection))
        return false;
    void* previous = InterlockedCompareExchangePointer(change.entry, change.replacement, change.expected);
    DWORD ignored = 0;
    VirtualProtect(static_cast<void*>(change.entry), sizeof(void*), protection, &ignored);
    return previous == change.expected;
}

template <typename Write>
bool RunBaselineTransaction(const std::vector<BaselineChange>& changes, std::mutex& patchLock,
                            Write write, bool* rollbackFailed) {
    std::lock_guard<std::mutex> lock(patchLock);
    *rollbackFailed = false;
    // PE import slots contain pointer-sized integers. Keep that object type for
    // atomic reads; the pointer view is only for Windows' interlocked API.
    for (const auto& change : changes) {
        if (!change.entry || !change.replacement ||
            !IsCommittedReadableMemory(static_cast<const void*>(change.entry), sizeof(void*)) ||
            std::atomic_ref<ULONG_PTR>(*reinterpret_cast<ULONG_PTR*>(change.entry)).load(std::memory_order_acquire) !=
                reinterpret_cast<ULONG_PTR>(change.expected))
            return false;
    }
    size_t written = 0;
    for (; written < changes.size(); ++written) {
        if (!write(changes[written]))
            break;
    }
    if (written == changes.size())
        return true;
    while (written != 0) {
        const auto& change = changes[--written];
        // A foreign owner arriving during rollback keeps its own predecessor.
        if (!write(BaselineChange{change.entry, change.replacement, change.expected}))
            *rollbackFailed = true;
    }
    return false;
}

}  // namespace IATHook::detail
