#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <vector>

#include "../common/wow64_stack_range_policy.h"

// Adds a WoW64 target's 32-bit thread stacks to a minidump the 64-bit helper
// writes. Without them the dump carries only the x64 syscall thunk for every
// thread and not one 32-bit caller can be recovered; see
// `common/wow64_stack_range_policy.h` for the sessions that proved it.
//
// Usage: construct against the target process and, when Active() is true, pass
// CallbackInformation() to MiniDumpWriteDump. The collector must outlive that
// call. Stack pointers are read from inside dbghelp's own thread callback,
// where the target is already suspended for the dump, so this adds no
// suspension of its own and can never read a torn context.
class Wow64StackCollector {
public:
    Wow64StackCollector(HANDLE targetProcess, DWORD targetProcessId);

    bool Active() const {
        return active_;
    }

    // Valid only while this object is alive.
    MINIDUMP_CALLBACK_INFORMATION CallbackInformation();

    size_t RangeCount() const {
        return budget_.Count();
    }
    unsigned long long RangeBytes() const {
        return static_cast<unsigned long long>(budget_.UsedBytes());
    }

private:
    static BOOL CALLBACK MinidumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                          PMINIDUMP_CALLBACK_OUTPUT output);
    void CollectThreadStack(HANDLE thread);
    void CollectEveryThreadStackOnce();
    bool NextRange(ce::wow64_stack_ranges::Range& range);

    HANDLE process_ = nullptr;
    DWORD processId_ = 0;
    bool active_ = false;
    bool threadSweepDone_ = false;
    bool collected_ = false;
    std::vector<ce::wow64_stack_ranges::Range> ranges_;
    size_t emitted_ = 0;
    ce::wow64_stack_ranges::RangeBudget budget_;
};
