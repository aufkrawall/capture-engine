#include "dump_helper_wow64_stacks.h"

#include <tlhelp32.h>

namespace {

ce::wow64_stack_ranges::Region QueryRegion(HANDLE process, uint64_t address) {
    ce::wow64_stack_ranges::Region region;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)), &info, sizeof(info)) !=
        sizeof(info)) {
        return region;
    }
    region.valid = true;
    region.base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.BaseAddress));
    region.size = static_cast<uint64_t>(info.RegionSize);
    region.allocationBase = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(info.AllocationBase));
    region.committed =
        info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    return region;
}

bool TargetIsWow64(HANDLE process) {
    BOOL isWow64 = FALSE;
    return process && IsWow64Process(process, &isWow64) && isWow64;
}

}  // namespace

Wow64StackCollector::Wow64StackCollector(HANDLE targetProcess, DWORD targetProcessId)
    : process_(targetProcess), processId_(targetProcessId), active_(TargetIsWow64(targetProcess)) {
    if (active_) {
        ranges_.reserve(64);
    }
}

MINIDUMP_CALLBACK_INFORMATION Wow64StackCollector::CallbackInformation() {
    MINIDUMP_CALLBACK_INFORMATION information = {};
    information.CallbackRoutine = &Wow64StackCollector::MinidumpCallback;
    information.CallbackParam = this;
    return information;
}

void Wow64StackCollector::CollectThreadStack(HANDLE thread) {
    if (!thread || !process_) {
        return;
    }

    WOW64_CONTEXT context = {};
    context.ContextFlags = WOW64_CONTEXT_CONTROL;
    if (!Wow64GetThreadContext(thread, &context)) {
        return;
    }

    ce::wow64_stack_ranges::Range range;
    if (!ce::wow64_stack_ranges::ResolveStackRange(
            static_cast<uint64_t>(context.Esp),
            [this](uint64_t address) { return QueryRegion(process_, address); }, range)) {
        return;
    }
    if (!budget_.Admit(range)) {
        return;
    }
    ranges_.push_back(range);
}

// dbghelp runs its thread callbacks before it asks for extra memory, which is
// where the stack pointers come from. Should a future dbghelp ask for memory
// first, this recovers the same information by enumerating the target's threads
// directly: MiniDumpWriteDump has the target frozen for the whole call, so a
// WOW64 context read here is as valid as one read from the thread callback.
void Wow64StackCollector::CollectEveryThreadStackOnce() {
    if (threadSweepDone_) {
        return;
    }
    threadSweepDone_ = true;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId_) {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            CollectThreadStack(thread);
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

bool Wow64StackCollector::NextRange(ce::wow64_stack_ranges::Range& range) {
    if (ranges_.empty() && !collected_) {
        CollectEveryThreadStackOnce();
    }
    if (emitted_ >= ranges_.size()) {
        // WriteSupplementalCrashDump falls back to smaller dump types when an
        // attempt fails, and each attempt walks threads and memory again. The
        // ranges do not change between attempts, so the collection is frozen
        // here and the cursor rewound for the next one.
        collected_ = true;
        emitted_ = 0;
        return false;
    }
    range = ranges_[emitted_++];
    return true;
}

BOOL CALLBACK Wow64StackCollector::MinidumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT input,
                                                    PMINIDUMP_CALLBACK_OUTPUT output) {
    auto* collector = static_cast<Wow64StackCollector*>(param);
    if (!collector || !input || !output) {
        return TRUE;
    }

    switch (input->CallbackType) {
        case ThreadCallback:
            // The target is already suspended for the duration of the dump, so
            // the WOW64 context read here is the one the thread still has when
            // the memory is copied.
            collector->threadSweepDone_ = true;
            if (!collector->collected_) {
                collector->CollectThreadStack(input->Thread.ThreadHandle);
            }
            return TRUE;
        case ThreadExCallback:
            collector->threadSweepDone_ = true;
            if (!collector->collected_) {
                collector->CollectThreadStack(input->ThreadEx.ThreadHandle);
            }
            return TRUE;
        case MemoryCallback: {
            ce::wow64_stack_ranges::Range range;
            if (!collector->NextRange(range)) {
                // A zero-length answer is how the callback reports that it has
                // no more regions to contribute; dbghelp then stops asking.
                output->MemoryBase = 0;
                output->MemorySize = 0;
                return TRUE;
            }
            // MinGW's dbghelp headers name this union member pair
            // MemoryBase/MemorySize; the Windows SDK spells the same fields
            // Memory.StartOfMemoryRange/Memory.MemorySize.
            output->MemoryBase = range.start;
            output->MemorySize = static_cast<ULONG>(range.size);
            return TRUE;
        }
        default:
            return TRUE;
    }
}
