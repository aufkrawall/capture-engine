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
    EXCEPTION_RECORD record{};
    CONTEXT context{};
};

constexpr size_t kSlotCount = 64;
FaultSlot g_Slots[kSlotCount];

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

LONG CALLBACK FirstChanceContinueHandler(PEXCEPTION_POINTERS) {
    // Continue handlers run when a handler resumed execution
    // (EXCEPTION_CONTINUE_EXECUTION): the fault this thread recorded is
    // resolved - Mono's and the JVM's vectored handlers take exactly this path.
    if (FaultSlot* slot = FindOwnSlot(GetCurrentThreadId())) {
        slot->threadId.store(0, std::memory_order_release);
        g_ResolvedByContinue.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void RecordFault(const EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
        return;
    const DWORD threadId = GetCurrentThreadId();
    FaultSlot* slot = FindOwnSlot(threadId);
    if (!slot) {
        for (FaultSlot& candidate : g_Slots) {
            DWORD expected = 0;
            if (candidate.threadId.compare_exchange_strong(expected, threadId, std::memory_order_acq_rel)) {
                slot = &candidate;
                break;
            }
        }
    }
    if (!slot) {
        slot = &g_Slots[(threadId / 4) % kSlotCount];
        slot->threadId.store(threadId, std::memory_order_release);
    }
    bool expectedWriting = false;
    if (!slot->writing.compare_exchange_strong(expectedWriting, true, std::memory_order_acquire)) {
        g_Dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot->record = *pointers->ExceptionRecord;
    // Never follow the nested chain into the faulting stack.
    slot->record.ExceptionRecord = nullptr;
    slot->context = *pointers->ContextRecord;
    // The copy holds only the fixed CONTEXT; an XSTATE flag would make dbghelp
    // read the extended area that follows the original on the faulting stack.
    slot->context.ContextFlags &= ~static_cast<DWORD>(0x40);
    slot->writing.store(false, std::memory_order_release);
    g_Recorded.fetch_add(1, std::memory_order_relaxed);
}

void ClearFaultForCurrentThread() {
    if (FaultSlot* slot = FindOwnSlot(GetCurrentThreadId()))
        slot->threadId.store(0, std::memory_order_release);
}

bool CopyFaultForCurrentThread(EXCEPTION_RECORD* record, CONTEXT* context) {
    const DWORD threadId = GetCurrentThreadId();
    FaultSlot* slot = FindOwnSlot(threadId);
    if (!slot || slot->writing.load(std::memory_order_acquire))
        return false;
    EXCEPTION_RECORD recordCopy = slot->record;
    CONTEXT contextCopy = slot->context;
    // A takeover by another thread between the lookup and the copy would have
    // changed the owner; only an unchanged owner makes the copy this thread's.
    if (slot->threadId.load(std::memory_order_acquire) != threadId)
        return false;
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
    ResolveDispatcherRanges();
    bool expected = false;
    if (!g_Installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;
    AddVectoredContinueHandler(1, FirstChanceContinueHandler);
}

Statistics GetStatistics() {
    Statistics statistics;
    statistics.recorded = g_Recorded.load(std::memory_order_relaxed);
    statistics.resolvedByContinue = g_ResolvedByContinue.load(std::memory_order_relaxed);
    statistics.dropped = g_Dropped.load(std::memory_order_relaxed);
    return statistics;
}

void ResetForTesting() {
    for (FaultSlot& slot : g_Slots) {
        slot.threadId.store(0, std::memory_order_release);
        slot.writing.store(false, std::memory_order_release);
    }
}

bool IsAddressInDispatcherForTesting(const void* address) {
    ResolveDispatcherRanges();
    return IsInDispatcher(reinterpret_cast<uintptr_t>(address));
}

}  // namespace ce::crash_first_chance
