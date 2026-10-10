/**
 * What a crash dump must record to say WHY an application faulted, not just
 * where.
 *
 * `MiniDumpWriteDump` records the faulting instruction and whatever the stacks
 * happen to point at. Session 20261010_160937 is what that costs: The Witcher 3
 * died of its own compiled-in breakpoint trap, the 182 MB rich dump carried the
 * game's whole .data segment but none of the evidence that says why - the
 * call-site code of the reporting function (not stack-referenced), and the
 * error strings its LEA instructions point at (in .rdata, referenced from code,
 * not from any stack). Attribution worked; root cause was unreachable.
 *
 * The fix is to give dbghelp the fault neighborhood through its memory
 * callback, the same mechanism `dump_helper_wow64_stacks` uses: the faulting
 * thread's stack, a code window around every code pointer on it (call sites
 * and their LEA/CALL operands are decodable from those), windows around the
 * exception context's registers (error objects live there), and the data those
 * code windows reference. This header owns the range arithmetic and the
 * (deliberately heuristic) reference scan so both can be tested without a
 * process; everything here is bounded so a crash dump cannot become a
 * full-memory dump.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ce::fault_neighborhood {

// A 1 MiB stack is what a real thread can fill before it faults, and the bytes
// nearest the stack pointer carry the call chain; more of the same stack is
// rarely worth the dump size.
inline constexpr uint64_t kStackBytes = 1ull << 20;
// Code pointers on a stack point just after a CALL, so the interesting
// operands (the call itself, the surrounding LEAs) sit before them.
inline constexpr uint64_t kCodeWindowBeforeBytes = 1024;
inline constexpr uint64_t kCodeWindowAfterBytes = 3072;
inline constexpr uint64_t kRegisterWindowBytes = 4096;
inline constexpr uint64_t kReferenceWindowBytes = 1024;

inline constexpr size_t kMaxCodeWindows = 32;
inline constexpr size_t kMaxRegisterWindows = 16;
inline constexpr size_t kMaxReferenceWindows = 96;
inline constexpr size_t kMaxHarvestedRefsPerWindow = 64;
inline constexpr size_t kMaxRanges = 256;
inline constexpr uint64_t kMaxTotalBytes = 24ull << 20;

// One memory range to hand dbghelp.
struct Range {
    uint64_t start = 0;
    uint64_t size = 0;
};

// One `VirtualQueryEx` answer, reduced to what the walk needs.
struct Region {
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t allocationBase = 0;
    bool committed = false;
    bool readable = false;
    bool executable = false;
    bool valid = false;
};

inline bool RangesOverlap(const Range& a, const Range& b) {
    return a.size != 0 && b.size != 0 && a.start < b.start + b.size && b.start < a.start + a.size;
}

// Window around a code pointer (a return address, the faulting instruction).
inline Range CodeWindow(uint64_t address) {
    Range range;
    range.start = address > kCodeWindowBeforeBytes ? address - kCodeWindowBeforeBytes : 0;
    range.size = kCodeWindowBeforeBytes + kCodeWindowAfterBytes;
    return range;
}

// Window around a data pointer (a register holding an error object, a LEA
// target such as a message string).
inline Range ReferenceWindow(uint64_t address, uint64_t bytes = kReferenceWindowBytes) {
    Range range;
    // 16-byte alignment keeps the ranges stable across nearby pointers.
    range.start = address & ~uint64_t{15};
    range.size = bytes + (address - range.start);
    return range;
}

// Keeps the added memory bounded. A range that would exceed the budget is
// truncated to what is left rather than dropped: the bytes nearest the pointer
// are the ones that carry the meaning.
class Budget {
public:
    bool Admit(Range& range) {
        if (range.size == 0 || count_ >= kMaxRanges || usedBytes_ >= kMaxTotalBytes) {
            return false;
        }
        const uint64_t remaining = kMaxTotalBytes - usedBytes_;
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

// Resolves the committed span above `address`, following adjacent committed
// regions of the same reservation (a thread stack is one reservation whose
// guard page splits it into several regions). `query(address)` answers
// `VirtualQueryEx`. Capped at `maxBytes`.
template <typename QueryFn>
inline bool ResolveCommittedSpanAbove(uint64_t address, uint64_t maxBytes, QueryFn&& query, Range& out) {
    out = {};
    if (address == 0 || maxBytes == 0) {
        return false;
    }

    const Region first = query(address);
    if (!first.valid || !first.committed || first.size == 0) {
        return false;
    }
    if (address < first.base || address >= first.base + first.size) {
        return false;
    }

    uint64_t end = first.base + first.size;
    for (size_t step = 0; step < kMaxRanges; ++step) {
        if (end - address >= maxBytes) {
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

    if (end <= address) {
        return false;
    }
    out.start = address;
    out.size = end - address > maxBytes ? maxBytes : end - address;
    return true;
}

// One address found in captured code: a code target (a relative CALL - the
// report/assert functions) or a data target (a rip-relative LEA/MOV - message
// strings and error records, which live in .rdata).
struct CodeReference {
    uint64_t address = 0;
    bool code = false;
};

// Heuristic reference scan over captured code. There is no disassembler here on
// purpose; a wrong hit only spends bounded budget on a window nobody needed.
// Recognized:
//   E8 rel32                    - relative CALL (the report/assert functions)
//   48/49/4C/4D 8B|8D ModRM     - rip-relative MOV/LEA (message strings, error
//                                 records and other const data live in .rdata)
// Targets are appended to `out` in scan order, deduplicated, capped at
// `maxRefs`.
inline void HarvestCodeReferences(const uint8_t* code, size_t size, uint64_t codeAddress, size_t maxRefs,
                                  std::vector<CodeReference>& out) {
    if (!code || size < 5 || maxRefs == 0) {
        return;
    }
    const size_t initial = out.size();
    auto append = [&](uint64_t target, bool isCode) {
        for (size_t i = initial; i < out.size(); ++i) {
            if (out[i].address == target) {
                return;
            }
        }
        if (out.size() - initial >= maxRefs) {
            return;
        }
        out.push_back({target, isCode});
    };

    for (size_t i = 0; i + 5 <= size; ++i) {
        const uint8_t* at = code + i;
        if (at[0] == 0xE8) {
            int32_t relative = 0;
            std::memcpy(&relative, at + 1, sizeof(relative));
            append(codeAddress + i + 5 + static_cast<int64_t>(relative), true);
            continue;
        }
        if (i + 7 > size) {
            continue;
        }
        const bool rex = at[0] == 0x48 || at[0] == 0x49 || at[0] == 0x4C || at[0] == 0x4D;
        if (!rex || (at[1] != 0x8B && at[1] != 0x8D)) {
            continue;
        }
        const uint8_t modrm = at[2];
        const bool ripRelative = (modrm & 0xC7) == 0x05;
        if (!ripRelative) {
            continue;
        }
        int32_t displacement = 0;
        std::memcpy(&displacement, at + 3, sizeof(displacement));
        append(codeAddress + i + 7 + static_cast<int64_t>(displacement), false);
    }
}

// One PE section worth recording: writable, non-executable - the runtime state
// a debugger needs when a dump carries no module data segments.
struct SectionRange {
    uint64_t rva = 0;
    uint64_t size = 0;
};

// Enumerates the writable non-executable sections of a PE image from the bytes
// of its headers (DOS header through the section table), as the target maps
// them. Virtual size wins over raw size: .bss has no raw data.
inline bool HarvestWritableDataSections(const uint8_t* headers, size_t size, std::vector<SectionRange>& out) {
    constexpr uint32_t kScnMemExecute = 0x20000000;
    constexpr uint32_t kScnMemWrite = 0x80000000;
    constexpr size_t kDosHeaderSize = 0x40;
    constexpr size_t kSectionHeaderSize = 40;

    if (!headers || size < kDosHeaderSize) {
        return false;
    }
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, headers + 0x3C, sizeof(lfanew));
    if (lfanew > size || size - lfanew < 24) {
        return false;
    }
    const uint8_t* nt = headers + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) {
        return false;
    }
    uint16_t sectionCount = 0;
    uint16_t optionalHeaderSize = 0;
    std::memcpy(&sectionCount, nt + 4 + 2, sizeof(sectionCount));
    std::memcpy(&optionalHeaderSize, nt + 4 + 16, sizeof(optionalHeaderSize));

    const size_t sectionTable = static_cast<size_t>(lfanew) + 4 + 20 + optionalHeaderSize;
    if (sectionTable > size || sectionCount * kSectionHeaderSize > size - sectionTable) {
        return false;
    }
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const uint8_t* section = headers + sectionTable + i * kSectionHeaderSize;
        uint32_t virtualSize = 0;
        uint32_t virtualAddress = 0;
        uint32_t rawSize = 0;
        uint32_t characteristics = 0;
        std::memcpy(&virtualSize, section + 8, sizeof(virtualSize));
        std::memcpy(&virtualAddress, section + 12, sizeof(virtualAddress));
        std::memcpy(&rawSize, section + 16, sizeof(rawSize));
        std::memcpy(&characteristics, section + 36, sizeof(characteristics));
        if ((characteristics & kScnMemWrite) == 0 || (characteristics & kScnMemExecute) != 0) {
            continue;
        }
        const uint64_t span = virtualSize > rawSize ? virtualSize : rawSize;
        if (span == 0) {
            continue;
        }
        out.push_back({virtualAddress, span});
    }
    return true;
}

}  // namespace ce::fault_neighborhood
