// Stack-overflow attribution for crash.log (crash_stack_overflow_walk.h). Runs on the dump worker thread
// against the faulting thread's copied context; the faulting thread waits in the crash filter meanwhile, so
// its stack stays put for the walk.

#include "crash_stack_overflow_walk.h"

#include <cstdarg>
#include <cstdio>

namespace ce::crash_stack {
namespace {

#ifdef _WIN64
constexpr BYTE kUnwindFlagChainInfo = 0x4;

// Committed part of the reservation holding `address`; its top is the thread's StackBase. Unwinding only moves
// up from the faulting RSP, so `address` is the lowest byte the walk may read.
bool QueryStackBounds(uintptr_t address, uintptr_t* low, uintptr_t* high) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) || !region.AllocationBase ||
        region.State != MEM_COMMIT) {
        return false;
    }
    uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    for (int step = 0; step < 64; ++step) {
        MEMORY_BASIC_INFORMATION next{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(end), &next, sizeof(next)) ||
            next.AllocationBase != region.AllocationBase || next.State != MEM_COMMIT) {
            break;
        }
        end = reinterpret_cast<uintptr_t>(next.BaseAddress) + next.RegionSize;
    }
    *low = address;
    *high = end;
    return end > address;
}

// The frame base RtlVirtualUnwind will read saved registers from, when the function addresses its frame through
// a register (UNWIND_INFO FrameRegister, also behind chained entries). False when it does not, or while `rip`
// is still inside the prolog that sets that register up.
bool FrameRegisterBase(const CONTEXT& frame, uintptr_t imageBase, const RUNTIME_FUNCTION* entry,
                       uintptr_t* frameBase) {
    const DWORD ripOffset = static_cast<DWORD>(frame.Rip - imageBase);
    for (int depth = 0; entry && depth < 8; ++depth) {
        // UNWIND_INFO: Version:3 Flags:5, SizeOfProlog, CountOfCodes, FrameRegister:4 FrameOffset:4, codes.
        const BYTE* info = reinterpret_cast<const BYTE*>(imageBase + entry->UnwindData);
        const BYTE frameRegister = info[3] & 0x0F;
        if (frameRegister != 0) {
            if (depth == 0 && ripOffset < entry->BeginAddress + info[1]) {
                return false;
            }
            const DWORD64* registers = &frame.Rax;  // Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8..R15
            *frameBase = static_cast<uintptr_t>(registers[frameRegister]) - 16u * (info[3] >> 4);
            return true;
        }
        if (!((info[0] >> 3) & kUnwindFlagChainInfo)) {
            return false;
        }
        const size_t codeSlots = (info[2] + 1u) & ~1u;  // 2-byte codes, padded to an even count
        entry = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codeSlots * 2);
    }
    return false;
}

uintptr_t ImageBaseOf(uintptr_t address) {
    PVOID base = nullptr;
    RtlPcToFileHeader(reinterpret_cast<PVOID>(address), &base);
    return reinterpret_cast<uintptr_t>(base);
}
#endif

// The name the image was linked as (its export directory), read straight from the mapped headers - only for a
// base that is still a loaded image's, so a stale or made-up base is never dereferenced.
const char* ImageExportName(uintptr_t imageBase) {
#ifndef _WIN64
    (void)imageBase;
    return nullptr;  // WalkStack produces no frames on x86
#else
    PVOID loadedBase = nullptr;
    if (!imageBase || !RtlPcToFileHeader(reinterpret_cast<PVOID>(imageBase), &loadedBase) ||
        reinterpret_cast<uintptr_t>(loadedBase) != imageBase) {
        return nullptr;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.VirtualAddress + sizeof(IMAGE_EXPORT_DIRECTORY) > imageSize) {
        return nullptr;
    }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(imageBase + directory.VirtualAddress);
    if (exports->Name == 0 || exports->Name >= imageSize) {
        return nullptr;
    }
    return reinterpret_cast<const char*>(imageBase + exports->Name);
#endif
}

class LineWriter {
   public:
    LineWriter(char* out, size_t size) : out_(out), size_(size) {
        if (out_ && size_) {
            out_[0] = '\0';
        }
    }

    __attribute__((format(printf, 2, 3))) void Append(const char* format, ...) {
        if (!out_ || used_ + 1 >= size_) {
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(out_ + used_, size_ - used_, format, args);
        va_end(args);
        if (written > 0) {
            used_ += static_cast<size_t>(written) < size_ - used_ ? static_cast<size_t>(written) : size_ - used_ - 1;
        }
    }

    void AppendFrame(const Frame& frame) {
        const char* name = ImageExportName(frame.imageBase);
        const auto offset = static_cast<unsigned long long>(frame.returnAddress - frame.imageBase);
        if (name) {
            Append("%.48s+0x%llX", name, offset);
        } else if (frame.imageBase) {
            Append("image@%p+0x%llX", reinterpret_cast<void*>(frame.imageBase), offset);
        } else {
            Append("%p", reinterpret_cast<void*>(frame.returnAddress));
        }
    }

   private:
    char* out_;
    size_t size_;
    size_t used_ = 0;
};

}  // namespace

bool WalkStack(const CONTEXT& context, size_t maxFrames, uintptr_t ownImageBase, OverflowFrameSummary* summary,
               bool* complete) {
    if (complete) {
        *complete = false;
    }
#ifndef _WIN64
    (void)context;
    (void)maxFrames;
    (void)ownImageBase;
    (void)summary;
    return false;
#else
    if (!summary || !complete) {
        return false;
    }
    if (ownImageBase == 0) {
        ownImageBase = ImageBaseOf(reinterpret_cast<uintptr_t>(&WalkStack));
    }
    uintptr_t low = 0;
    uintptr_t high = 0;
    if (!QueryStackBounds(static_cast<uintptr_t>(context.Rsp), &low, &high)) {
        return false;
    }
    CONTEXT frame = context;
    UNWIND_HISTORY_TABLE history{};
    // Step 0 unwinds the faulting frame itself; every later step's RIP is a return address.
    for (size_t step = 0; step <= maxFrames; ++step) {
        const auto rip = static_cast<uintptr_t>(frame.Rip);
        const auto rsp = static_cast<uintptr_t>(frame.Rsp);
        if (rip == 0) {
            *complete = true;  // the thread's first frame returns nowhere
            break;
        }
        if (step == maxFrames || rsp < low || rsp + sizeof(DWORD64) > high) {
            break;
        }
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(frame.Rip, &imageBase, &history);
        if (step > 0) {
            const uintptr_t owner = entry ? static_cast<uintptr_t>(imageBase) : ImageBaseOf(rip);
            summary->Add(Frame{rip, owner}, owner != 0 && owner == ownImageBase);
        }
        if (!entry) {
            // A leaf function keeps no frame: its return address is at RSP.
            frame.Rip = *reinterpret_cast<const DWORD64*>(rsp);
            frame.Rsp = rsp + sizeof(DWORD64);
        } else {
            uintptr_t frameBase = 0;
            if (FrameRegisterBase(frame, static_cast<uintptr_t>(imageBase), entry, &frameBase) &&
                (frameBase < rsp || frameBase >= high)) {
                break;  // a corrupt frame register would make the unwinder read outside the stack
            }
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, frame.Rip, entry, &frame, &handlerData,
                             &establisherFrame, nullptr);
        }
        if (static_cast<uintptr_t>(frame.Rsp) <= rsp) {
            break;  // no progress: unwind data that does not describe this frame
        }
    }
    summary->Finish();
    return true;
#endif
}

void FormatOverflowAttribution(const OverflowFrameSummary& summary, bool complete, char* out, size_t outSize) {
    LineWriter line(out, outSize);
    line.Append("StackOverflow attribution: %zu frames unwound (complete=%d); ", summary.Frames(), complete ? 1 : 0);
    const size_t period = summary.CyclePeriod();
    if (period != 0) {
        line.Append("cycle of %zu frame(s) from depth %zu repeats over %zu frames: [", period, summary.CycleStart(),
                    summary.CycleFrames());
    } else {
        line.Append("no repeating cycle in the innermost %zu, first: [",
                    summary.Frames() < OverflowFrameSummary::kHeadFrames ? summary.Frames()
                                                                         : OverflowFrameSummary::kHeadFrames);
    }
    constexpr size_t kShownFrames = 6;
    const size_t available = period != 0 ? period : summary.Frames();
    const size_t shown = available < kShownFrames ? available : kShownFrames;
    for (size_t i = 0; i < shown; ++i) {
        if (i) {
            line.Append(", ");
        }
        line.AppendFrame(summary.CycleFrame(i));
    }
    line.Append("%s", shown < available ? ", ...]" : "]");
    line.Append("; CE frames=%zu", summary.OwnFrames());
    if (summary.OwnFrames() != 0) {
        line.Append(" (first at depth %zu)", summary.FirstOwnDepth());
    }
    // Callee first, as a debugger prints a stack: "a <- b" reads "a, called from b".
    line.Append("; outermost: ");
    for (size_t i = summary.TailCount(); i-- > 0;) {
        line.AppendFrame(summary.Tail(i));
        if (i) {
            line.Append(" <- ");
        }
    }
}

}  // namespace ce::crash_stack
