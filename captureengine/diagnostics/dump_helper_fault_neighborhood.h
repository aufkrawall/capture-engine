#pragma once

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

#include <string>
#include <vector>

#include "common/crash/fault_neighborhood_policy.h"

// Adds the fault neighborhood of a target's recorded exception to the minidump
// the helper writes: the faulting thread's stack, code windows around the code
// pointers on it (so call sites and their operands can be decoded offline),
// windows around the exception context's registers, and the data the captured
// code references (message strings and error records, which live in .rdata and
// are referenced from code, not from any stack). Without these, a dump says
// which instruction faulted but not what the application was reporting - see
// `common/crash/fault_neighborhood_policy.h` for the session that proved it.
//
// Usage: construct against the target with the address of its
// EXCEPTION_POINTERS, then serve ranges to a minidump memory callback via
// ServeMemoryRange() until it returns false. ServeMemoryRange is a pull
// interface so several range sources can be chained into the single callback
// slot MiniDumpWriteDump offers (see dump_helper.cpp). Collection happens on
// the first serve, inside the dump call, where the target is frozen - this
// adds no suspension of its own and cannot read torn state.
//
// x64 targets only: the pointer-sized structure layouts and the CONTEXT the
// exception stream points at differ for WoW64 targets, which keep the stack
// fix from `dump_helper_wow64_stacks` and nothing else.
class FaultNeighborhoodCollector {
public:
    FaultNeighborhoodCollector(HANDLE targetProcess, unsigned long long exceptionPointersAddress);

    bool Active() const {
        return active_;
    }

    // Pull one range for the dump's memory callback. False at the end of the
    // range set; the cursor rewinds so the next dump attempt serves the same
    // set again.
    bool ServeMemoryRange(PMINIDUMP_CALLBACK_OUTPUT output);

    size_t RangeCount() const {
        return budget_.Count();
    }
    unsigned long long RangeBytes() const {
        return static_cast<unsigned long long>(budget_.UsedBytes());
    }
    size_t CodeWindowCount() const {
        return codeWindows_;
    }
    size_t ReferenceWindowCount() const {
        return referenceWindows_;
    }
    size_t RegisterWindowCount() const {
        return registerWindows_;
    }

private:
    void Collect();
    void AddCodePointer(uint64_t address);
    void AddReference(uint64_t address);
    void AddRegisterValue(uint64_t value);
    void AddOwnModuleData();
    bool AddWindow(ce::fault_neighborhood::Range range, bool harvestCode);
    bool ReadTarget(uint64_t address, void* buffer, size_t bytes) const;

    HANDLE process_ = nullptr;
    unsigned long long exceptionPointersAddress_ = 0;
    bool active_ = false;
    bool collected_ = false;
    std::vector<ce::fault_neighborhood::Range> ranges_;
    std::vector<ce::fault_neighborhood::CodeReference> references_;
    size_t emitted_ = 0;
    size_t codeWindows_ = 0;
    size_t referenceWindows_ = 0;
    size_t registerWindows_ = 0;
    ce::fault_neighborhood::Budget budget_;
};
