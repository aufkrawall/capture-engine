/**
 * Which memory a 64-bit minidump of a WoW64 target has to be told to include.
 *
 * `MiniDumpWriteDump` records a thread's stack from the CONTEXT it can see. For
 * a WoW64 thread written by a 64-bit dumper that is the x64 side of the thread,
 * which holds nothing but the syscall thunk: every frame the 32-bit process
 * actually executed lives on a separate stack the dump never mentions. The
 * result loads, resolves symbols and shows registers, and then cannot produce a
 * single caller - Gothic II session `20260916_000027` and both sessions in
 * commit 339eccf0 were attributed from opcodes alone for exactly that reason.
 *
 * The fix is to hand dbghelp the 32-bit stacks through its memory callback.
 * This header owns the range arithmetic so it can be tested without a process:
 * how far above a WoW64 stack pointer the committed stack reaches, and the caps
 * that keep a process with hundreds of threads from turning a crash dump into a
 * full-memory dump.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace ce::wow64_stack_ranges {

// A 32-bit thread stack is at most 1 MiB committed in practice, and a stack
// that somehow reports more is far likelier to be a mis-walked region than a
// useful one. The totals bound the whole dump: 64 threads' worth of stack is
// already more than any crash needs, and the dump must stay small enough to
// keep being written on the crash path.
inline constexpr uint64_t kMaxRangeBytesPerThread = 1ull << 20;
inline constexpr uint64_t kMaxTotalRangeBytes = 64ull << 20;
inline constexpr size_t kMaxRanges = 512;

// The 32-bit address space, which every WoW64 stack lies inside.
inline constexpr uint64_t kWow64AddressLimit = 1ull << 32;

// One `VirtualQueryEx` answer, reduced to what the walk needs.
struct Region {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t allocationBase = 0;
    bool committed = false;
    bool valid = false;
};

struct Range {
    uint64_t start = 0;
    uint64_t size = 0;
};

// Resolves the committed stack above `stackPointer`. `query(address)` answers
// `VirtualQueryEx`. The walk follows adjacent committed regions that belong to
// the same reservation, because a thread stack is one reservation whose guard
// page and uncommitted tail split it into several regions.
template <typename QueryFn>
inline bool ResolveStackRange(uint64_t stackPointer, QueryFn&& query, Range& out) {
    out = {};
    if (stackPointer == 0 || stackPointer >= kWow64AddressLimit) {
        return false;
    }

    const Region first = query(stackPointer);
    if (!first.valid || !first.committed || first.size == 0) {
        return false;
    }
    if (stackPointer < first.base || stackPointer >= first.base + first.size) {
        return false;
    }

    uint64_t end = first.base + first.size;
    for (size_t step = 0; step < kMaxRanges; ++step) {
        if (end >= kWow64AddressLimit) {
            break;
        }
        const Region next = query(end);
        if (!next.valid || !next.committed || next.size == 0) {
            break;
        }
        if (next.allocationBase != first.allocationBase || next.base != end) {
            break;
        }
        end = next.base + next.size;
    }

    if (end <= stackPointer) {
        return false;
    }
    uint64_t size = end - stackPointer;
    if (size > kMaxRangeBytesPerThread) {
        size = kMaxRangeBytesPerThread;
    }
    out.start = stackPointer;
    out.size = size;
    return true;
}

// Keeps the added memory bounded across threads. A range that would exceed the
// budget is truncated to what is left rather than dropped, because the bytes
// nearest the stack pointer are the ones that carry the call chain.
class RangeBudget {
public:
    bool Admit(Range& range) {
        if (range.size == 0 || count_ >= kMaxRanges || usedBytes_ >= kMaxTotalRangeBytes) {
            return false;
        }
        const uint64_t remaining = kMaxTotalRangeBytes - usedBytes_;
        if (range.size > remaining) {
            range.size = remaining;
        }
        usedBytes_ += range.size;
        ++count_;
        return true;
    }

    size_t Count() const {
        return count_;
    }
    uint64_t UsedBytes() const {
        return usedBytes_;
    }

private:
    size_t count_ = 0;
    uint64_t usedBytes_ = 0;
};

}  // namespace ce::wow64_stack_ranges
