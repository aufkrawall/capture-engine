#include "crash_first_chance.h"

#include <atomic>
#include <iterator>

namespace ce::crash_first_chance {

namespace {

// One slot per thread that recorded a fault. A thread reuses its own slot; a
// new thread claims a free one, and when none is free it takes over the slot
// its id hashes to. A slot is only ever written by the thread that owns it, and
// only read back by that same thread at termination, so the per-slot `writing`
// flag exists solely to keep a takeover from tearing a concurrent record.
struct alignas(16) FaultSlot {
    std::atomic<DWORD> threadId{0};
    std::atomic<bool> writing{false};
    std::atomic<uint64_t> sequence{0};
    EXCEPTION_RECORD record{};
    CONTEXT context{};
};

constexpr size_t kSlotCount = 64;
FaultSlot g_Slots[kSlotCount];

struct CandidateSlot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<bool> writing{false};
    FaultCandidate candidate{};
};
constexpr size_t kCandidateRingCapacity = 16;
CandidateSlot g_CandidateRing[kCandidateRingCapacity];
std::atomic<uint64_t> g_CandidateCounter{0};
std::atomic<bool> g_DiagnosticArmEnabled{false};

std::atomic<uint64_t> g_Recorded{0};
std::atomic<uint64_t> g_ResolvedByContinue{0};
std::atomic<uint64_t> g_Dropped{0};

struct AddressRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;
};
AddressRange g_DispatcherRanges[2];
std::atomic<bool> g_DispatcherRangesResolved{false};
std::atomic<bool> g_Installed{false};
std::atomic<uintptr_t> g_HandlerModuleBegin{0};
std::atomic<size_t> g_HandlerModuleSize{0};

void ResolveHandlerModuleRange() {
    if (g_HandlerModuleSize.load(std::memory_order_acquire) != 0)
        return;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(&Install), &memory, sizeof(memory)) || !memory.AllocationBase)
        return;
    const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(memory.AllocationBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    g_HandlerModuleBegin.store(reinterpret_cast<uintptr_t>(dos), std::memory_order_relaxed);
    g_HandlerModuleSize.store(nt->OptionalHeader.SizeOfImage, std::memory_order_release);
}

AddressRange ResolveFunctionRange(HMODULE ntdll, const char* exportName) {
    AddressRange range;
    const auto address = reinterpret_cast<uintptr_t>(GetProcAddress(ntdll, exportName));
    if (!address)
        return range;
#ifdef _WIN64
    DWORD64 imageBase = 0;
    if (const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(address, &imageBase, nullptr)) {
        range.begin = static_cast<uintptr_t>(imageBase + function->BeginAddress);
        range.end = static_cast<uintptr_t>(imageBase + function->EndAddress);
        return range;
    }
#endif
    // No unwind table to ask (x86): both functions are far shorter than this,
    // and only a return address inside them can match.
    range.begin = address;
    range.end = address + 0x100;
    return range;
}

void ResolveDispatcherRanges() {
    if (g_DispatcherRangesResolved.load(std::memory_order_acquire))
        return;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return;
    // Hardware faults are dispatched from KiUserExceptionDispatcher; software
    // raises (RaiseException, C++ throw) dispatch from inside RtlRaiseException.
    g_DispatcherRanges[0] = ResolveFunctionRange(ntdll, "KiUserExceptionDispatcher");
    g_DispatcherRanges[1] = ResolveFunctionRange(ntdll, "RtlRaiseException");
    g_DispatcherRangesResolved.store(true, std::memory_order_release);
}

bool IsInDispatcher(uintptr_t address) {
    for (const AddressRange& range : g_DispatcherRanges) {
        if (range.begin != 0 && address >= range.begin && address < range.end)
            return true;
    }
    return false;
}

FaultSlot* FindOwnSlot(DWORD threadId) {
    for (FaultSlot& slot : g_Slots) {
        if (slot.threadId.load(std::memory_order_acquire) == threadId)
            return &slot;
    }
    return nullptr;
}

bool TryClaimSlotForWrite(FaultSlot& slot, DWORD expectedThreadId, DWORD newThreadId) {
    bool expectedWriting = false;
    if (!slot.writing.compare_exchange_strong(expectedWriting, true, std::memory_order_acq_rel))
        return false;
    const DWORD currentOwner = slot.threadId.load(std::memory_order_acquire);
    if (currentOwner != expectedThreadId && currentOwner != newThreadId) {
        slot.writing.store(false, std::memory_order_release);
        return false;
    }
    slot.sequence.fetch_add(1, std::memory_order_acq_rel);
    slot.threadId.store(newThreadId, std::memory_order_release);
    return true;
}

bool TryTakeOverSlotForWrite(FaultSlot& slot, DWORD newThreadId) {
    bool expectedWriting = false;
    if (!slot.writing.compare_exchange_strong(expectedWriting, true, std::memory_order_acq_rel))
        return false;
    slot.sequence.fetch_add(1, std::memory_order_acq_rel);
    slot.threadId.store(newThreadId, std::memory_order_release);
    return true;
}

// Internal test interleave seam; these helpers are not part of the exported crash API.
std::atomic<void (*)()> g_BeforeClearForTesting{nullptr};

bool ClearOwnedFault(DWORD threadId) {
    FaultSlot* slot = FindOwnSlot(threadId);
    if (!slot) return false;
    if (const auto hook = g_BeforeClearForTesting.load(std::memory_order_acquire)) hook();
    if (!TryClaimSlotForWrite(*slot, threadId, threadId)) return false;
    slot->threadId.store(0, std::memory_order_release);
    slot->sequence.fetch_add(1, std::memory_order_release);
    slot->writing.store(false, std::memory_order_release);
    return true;
}

LONG CALLBACK FirstChanceContinueHandler(PEXCEPTION_POINTERS) {
    // Continue handlers run when a handler resumed execution
    // (EXCEPTION_CONTINUE_EXECUTION): the fault this thread recorded is
    // resolved - Mono's and the JVM's vectored handlers take exactly this path.
    if (ClearOwnedFault(GetCurrentThreadId()))
        g_ResolvedByContinue.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void RecordFault(const EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
        return;
    const DWORD threadId = GetCurrentThreadId();
    FaultSlot* slot = FindOwnSlot(threadId);
    if (slot && !TryClaimSlotForWrite(*slot, threadId, threadId)) {
        slot = nullptr;
    }
    if (!slot) {
        for (FaultSlot& candidate : g_Slots) {
            if (candidate.threadId.load(std::memory_order_relaxed) == 0 &&
                TryClaimSlotForWrite(candidate, 0, threadId)) {
                slot = &candidate;
                break;
            }
        }
    }
    if (!slot) {
        FaultSlot& fallback = g_Slots[(threadId / 4) % kSlotCount];
        if (!TryTakeOverSlotForWrite(fallback, threadId)) {
            g_Dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        slot = &fallback;
    }
    slot->record = *pointers->ExceptionRecord;
    // Never follow the nested chain into the faulting stack.
    slot->record.ExceptionRecord = nullptr;
    slot->context = *pointers->ContextRecord;
    // The copy holds only the fixed CONTEXT; an XSTATE flag would make dbghelp
    // read the extended area that follows the original on the faulting stack.
    slot->context.ContextFlags &= ~static_cast<DWORD>(0x40);
    slot->sequence.fetch_add(1, std::memory_order_release);
    slot->writing.store(false, std::memory_order_release);
    g_Recorded.fetch_add(1, std::memory_order_relaxed);

    if (g_DiagnosticArmEnabled.load(std::memory_order_acquire)) {
        const uint64_t seq = g_CandidateCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        CandidateSlot& candidateSlot = g_CandidateRing[seq % kCandidateRingCapacity];
        bool expectedWriting = false;
        if (candidateSlot.writing.compare_exchange_strong(expectedWriting, true, std::memory_order_acq_rel)) {
            candidateSlot.candidate.threadId = threadId;
            candidateSlot.candidate.exceptionCode = pointers->ExceptionRecord->ExceptionCode;
            candidateSlot.candidate.instructionAddress =
                reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
            candidateSlot.candidate.sequence = seq;
            candidateSlot.candidate.hasRecordAndContext = true;
            candidateSlot.candidate.confirmedFault = false;
            candidateSlot.candidate.record = *pointers->ExceptionRecord;
            candidateSlot.candidate.record.ExceptionRecord = nullptr;
            candidateSlot.candidate.context = *pointers->ContextRecord;
            candidateSlot.candidate.context.ContextFlags &= ~static_cast<DWORD>(0x40);
            candidateSlot.sequence.store(seq, std::memory_order_release);
            candidateSlot.writing.store(false, std::memory_order_release);
        }
    }
}

void ClearFaultForCurrentThread() {
    ClearOwnedFault(GetCurrentThreadId());
}

bool CopyFaultForCurrentThread(EXCEPTION_RECORD* record, CONTEXT* context) {
    const DWORD threadId = GetCurrentThreadId();
    FaultSlot* slot = FindOwnSlot(threadId);
    if (!slot)
        return false;
    // Claim the slot while copying non-atomic CONTEXT/EXCEPTION_RECORD payloads.
    // A seqlock retry alone would still permit a C++ data race with takeover.
    if (!TryClaimSlotForWrite(*slot, threadId, threadId)) return false;
    EXCEPTION_RECORD recordCopy = slot->record;
    CONTEXT contextCopy = slot->context;
    slot->sequence.fetch_add(1, std::memory_order_release);
    slot->writing.store(false, std::memory_order_release);
    // Thread ids are reused. A record left behind by an exited thread whose id
    // this thread now carries faulted on a different stack.
    ULONG_PTR stackLow = 0;
    ULONG_PTR stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
#ifdef _WIN64
    const ULONG_PTR faultStack = static_cast<ULONG_PTR>(contextCopy.Rsp);
#else
    const ULONG_PTR faultStack = static_cast<ULONG_PTR>(contextCopy.Esp);
#endif
    if (faultStack < stackLow || faultStack >= stackHigh)
        return false;
    if (record)
        *record = recordCopy;
    if (context)
        *context = contextCopy;
    return true;
}

bool IsCurrentThreadInsideExceptionDispatch() {
    ResolveDispatcherRanges();
    void* frames[62] = {};
    const USHORT frameCount = RtlCaptureStackBackTrace(0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
    for (USHORT index = 0; index < frameCount; ++index) {
        if (IsInDispatcher(reinterpret_cast<uintptr_t>(frames[index])))
            return true;
    }
    return false;
}

void Install() {
    ResolveHandlerModuleRange();
    ResolveDispatcherRanges();
    bool expected = false;
    if (!g_Installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    AddVectoredContinueHandler(1, FirstChanceContinueHandler);
}

bool IsAddressInHandlerModule(const void* address) {
    const size_t size = g_HandlerModuleSize.load(std::memory_order_acquire);
    const uintptr_t begin = g_HandlerModuleBegin.load(std::memory_order_relaxed);
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    return size != 0 && value >= begin && value - begin < size;
}

Statistics GetStatistics() {
    Statistics statistics;
    statistics.recorded = g_Recorded.load(std::memory_order_relaxed);
    statistics.resolvedByContinue = g_ResolvedByContinue.load(std::memory_order_relaxed);
    statistics.dropped = g_Dropped.load(std::memory_order_relaxed);
    return statistics;
}

void SetDiagnosticArmEnabled(bool enabled) {
    g_DiagnosticArmEnabled.store(enabled, std::memory_order_release);
}

bool IsDiagnosticArmEnabled() {
    return g_DiagnosticArmEnabled.load(std::memory_order_acquire);
}

size_t CopyFaultCandidates(FaultCandidate* outCandidates, size_t maxCandidates) {
    if (!outCandidates || maxCandidates == 0) {
        return 0;
    }
    size_t count = 0;
    for (CandidateSlot& slot : g_CandidateRing) {
        const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
        if (seq == 0) {
            continue;
        }
        if (slot.writing.load(std::memory_order_acquire)) {
            continue;
        }
        outCandidates[count] = slot.candidate;
        ++count;
        if (count >= maxCandidates) {
            break;
        }
    }
    return count;
}

bool FindBestFaultCandidate(FaultCandidate* outCandidate, DWORD terminatingThreadId, DWORD exitCode) {
    if (!outCandidate) {
        return false;
    }
    FaultCandidate candidates[kCandidateRingCapacity];
    const size_t count = CopyFaultCandidates(candidates, kCandidateRingCapacity);
    if (count == 0) {
        return false;
    }

    // 1. Same thread as terminating thread
    for (size_t i = 0; i < count; ++i) {
        if (candidates[i].threadId == terminatingThreadId) {
            *outCandidate = candidates[i];
            outCandidate->confirmedFault = true;
            return true;
        }
    }

    // 2. Matching nonzero exit code from any thread
    if (exitCode != 0) {
        for (size_t i = 0; i < count; ++i) {
            if (candidates[i].exceptionCode == exitCode) {
                *outCandidate = candidates[i];
                outCandidate->confirmedFault = false;
                return true;
            }
        }
    }

    // 3. Most recent candidate (highest sequence)
    size_t newestIndex = 0;
    uint64_t highestSeq = candidates[0].sequence;
    for (size_t i = 1; i < count; ++i) {
        if (candidates[i].sequence > highestSeq) {
            highestSeq = candidates[i].sequence;
            newestIndex = i;
        }
    }
    *outCandidate = candidates[newestIndex];
    outCandidate->confirmedFault = (outCandidate->threadId == terminatingThreadId);
    return true;
}

void SetBeforeClearForTesting(void (*hook)()) { g_BeforeClearForTesting.store(hook, std::memory_order_release); }
bool TakeOverCurrentFaultForTesting(DWORD newOwner) {
    auto* slot = FindOwnSlot(GetCurrentThreadId());
    if (!slot || !TryTakeOverSlotForWrite(*slot, newOwner)) return false;
    slot->sequence.fetch_add(1, std::memory_order_release);
    slot->writing.store(false, std::memory_order_release);
    return true;
}
bool HasFaultOwnerForTesting(DWORD owner) { return FindOwnSlot(owner) != nullptr; }

void ResetForTesting() {
    g_BeforeClearForTesting.store(nullptr, std::memory_order_release);
    g_DiagnosticArmEnabled.store(false, std::memory_order_release);
    g_CandidateCounter.store(0, std::memory_order_release);
    for (CandidateSlot& slot : g_CandidateRing) {
        slot.sequence.store(0, std::memory_order_release);
        slot.writing.store(false, std::memory_order_release);
        slot.candidate = {};
    }
    for (FaultSlot& slot : g_Slots) {
        slot.threadId.store(0, std::memory_order_release);
        slot.sequence.store(0, std::memory_order_release);
        slot.writing.store(false, std::memory_order_release);
    }
}

bool IsAddressInDispatcherForTesting(const void* address) {
    ResolveDispatcherRanges();
    return IsInDispatcher(reinterpret_cast<uintptr_t>(address));
}

}  // namespace ce::crash_first_chance
