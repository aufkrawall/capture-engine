/**
 * VTable Hooking Utility Implementation
 */

#include "vtable_hook.h"
#include "vtable_hook_policy.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "../common/hook_common.h"

namespace VTableHook {

static std::atomic<bool> g_Initialized{false};
static std::mutex g_VTableMutex;
struct HookOwnership {
    void* original = nullptr;
    void* detour = nullptr;
    void* allocationBase = nullptr;
};
static std::unordered_map<void**, HookOwnership> g_HookOwnership;

static bool QueryReadablePointerSlot(void** entry, MEMORY_BASIC_INFORMATION* memoryOut = nullptr) {
    MEMORY_BASIC_INFORMATION memory = {};
    if (!entry || VirtualQuery(reinterpret_cast<const void*>(entry), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & PAGE_GUARD)) {
        return false;
    }

    const DWORD baseProtection = memory.Protect & 0xFFu;
    if (baseProtection == PAGE_NOACCESS || baseProtection == PAGE_EXECUTE)
        return false;

    const auto regionEnd = reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    if (reinterpret_cast<uintptr_t>(entry) > regionEnd - sizeof(void*))
        return false;

    if (memoryOut)
        *memoryOut = memory;
    return true;
}

Status Initialize() {
    if (g_Initialized)
        return ErrorAlreadyInitialized;
    g_Initialized = true;
    return Success;
}

Status Shutdown() {
    g_Initialized = false;
    return Success;
}

Status Create(void* pVTableEntry, void* pDetour, void** ppOriginal) {
    if (!pVTableEntry || !pDetour)
        return ErrorNotExecutable;

    // CRITICAL FIX: Lock mutex to prevent concurrent hook creation/removal
    // This prevents race conditions when multiple threads hook the same vtable
    std::lock_guard<std::mutex> lock(g_VTableMutex);
    void* previousCallerOriginal = ppOriginal ? *ppOriginal : nullptr;

    // VTable hook - pTarget is &vtable[index]
    void** ppEntry = reinterpret_cast<void**>(pVTableEntry);
    MEMORY_BASIC_INFORMATION slotMemory = {};
    if (!QueryReadablePointerSlot(ppEntry, &slotMemory)) {
        HookLog("VTableHook: Create - VTable entry %p is unavailable", ppEntry);
        return ErrorNotExecutable;
    }
    void* currentValue = *ppEntry;

    auto ownership = g_HookOwnership.find(ppEntry);
    if (ownership != g_HookOwnership.end() &&
        ownership->second.allocationBase != slotMemory.AllocationBase) {
        g_HookOwnership.erase(ownership);
        ownership = g_HookOwnership.end();
    }

    // NULL ENTRY GUARD: Do NOT patch a NULL vtable entry. Writing to a NULL
    // entry when the vtable is incomplete (e.g. sl_interposer wrapper devices
    // that only implement a subset of ID3D12Device methods) writes past the
    // vtable boundary, corrupting adjacent memory.
    if (!currentValue) {
        HookLog("VTableHook: Create - VTable entry %p is NULL! Cannot hook NULL entry. Skipping.", ppEntry);
        return ErrorNotExecutable;
    }

    // IDEMPOTENCY CHECK: Is the VTable entry already set to our detour?
    if (currentValue == pDetour) {
        HookLog(
            "VTableHook: Create - Target %p ALREADY contains Detour %p. "
            "Skipping write.",
            ppEntry, pDetour);
        if (ownership == g_HookOwnership.end())
            return ErrorAlreadyCreated;
        if (ppOriginal)
            *ppOriginal = ownership->second.original;
        return Success;
    }

    if (ownership != g_HookOwnership.end()) {
        if (ownership->second.detour == pDetour &&
            ce::vtable_hook_policy::ShouldReclaimRestoredSlot(
                currentValue, ownership->second.detour, ownership->second.original)) {
            // A follower was removed and restored CE's predecessor. No live
            // slot points back to CE, so reclaiming O -> CE cannot form the
            // CE -> foreign -> CE cycle guarded below.
            HookLog("VTableHook: Create - predecessor restored at %p; reclaiming CE hook", ppEntry);
            g_HookOwnership.erase(ownership);
            ownership = g_HookOwnership.end();
        } else {
            // A foreign follower changed the slot after CE installed it.
            // Reinstalling here could create CE -> foreign -> CE recursion, so
            // preserve the established ownership record and chain.
            HookLogImportant("VTableHook: Create - preserving existing CE chain at %p (current=%p detour=%p)",
                             ppEntry, currentValue, ownership->second.detour);
            return ErrorAlreadyCreated;
        }
    }

    // SAFETY CHECK: Warn if the current value looks like one of our hooks
    // This helps diagnose cross-hook collisions (e.g., DX11 hooked before DX12)
    // Hook addresses are in our DLL's address range (typically starting with
    // 00007FFA5FCF...)
    HMODULE hSelf = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)pDetour, &hSelf);
    HMODULE hCurrentTarget = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)currentValue, &hCurrentTarget);

    bool isSelfHook = (hCurrentTarget == hSelf);

    if (isSelfHook && currentValue != pDetour) {
        // The vtable entry points to our DLL but NOT to this specific detour
        // This means ANOTHER one of our hooks is already installed!
        HookLog(
            "VTableHook: WARNING - Target %p contains %p which is ANOTHER hook "
            "from our DLL! SKIPPING.",
            ppEntry, currentValue);
        // CRITICAL FIX: Do NOT hook again. This prevents infinite recursion if we
        // accidentally hook the same vtable twice with slightly different function
        // pointers (e.g. template instantiation issues) or if we encounter a
        // double-hook scenario. Preserving the EXISTING hook is safer than creating
        // a cycle.
        return ErrorAlreadyCreated;
    }

    // DEBUG: Log memory region details
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void*>(ppEntry), &mbi, sizeof(mbi))) {
        HookLog(
            "VTableHook: DEBUG - Target %p in region: Base=%p, Size=%zu, "
            "Protect=0x%X",
            ppEntry, mbi.BaseAddress, mbi.RegionSize, mbi.Protect);
    }

    // Save original function pointer if requested
    if (ppOriginal) {
        *ppOriginal = currentValue;
        HookLog("VTableHook: Saving original %p to caller's storage", currentValue);
    }
    MemoryBarrier();

    HookLog("VTableHook: Create - Patching %p (Original=%p, Detour=%p, SelfHook=%d)", ppEntry, currentValue, pDetour,
            isSelfHook ? 1 : 0);

    try {
        const auto [insertedOwnership, inserted] =
            g_HookOwnership.emplace(ppEntry, HookOwnership{currentValue, pDetour, slotMemory.AllocationBase});
        (void)insertedOwnership;
        if (!inserted) {
            if (ppOriginal)
                *ppOriginal = previousCallerOriginal;
            return ErrorAlreadyCreated;
        }
    } catch (...) {
        HookLogImportant("VTableHook: Create - could not allocate ownership record for %p", ppEntry);
        if (ppOriginal)
            *ppOriginal = previousCallerOriginal;
        return ErrorMemoryAlloc;
    }

    // Patch the vtable entry directly
    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(ppEntry), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("VTableHook: Create - VirtualProtect FAILED for %p (Error=%lu)", ppEntry, GetLastError());
        g_HookOwnership.erase(ppEntry);
        if (ppOriginal)
            *ppOriginal = previousCallerOriginal;
        return ErrorMemoryProtect;
    }

    void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(ppEntry), pDetour,
                                                       currentValue);

    // CRITICAL FIX: Flush instruction cache after modifying code
    // While x86/x64 has strong cache coherency, this is required for correctness
    // and may be needed on certain configurations or future CPUs
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(ppEntry), sizeof(void*));

    DWORD newProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(ppEntry), sizeof(void*), oldProtect, &newProtect)) {
        HookLog("VTableHook: Create - VirtualProtect RESTORE FAILED for %p (Error=%lu)", ppEntry, GetLastError());
    }

    // A foreign hook may have won after our initial read. Never overwrite it.
    if (replaced != currentValue) {
        HookLogImportant("VTableHook: Create - ownership changed at %p (expected=%p observed=%p); preserving it",
                         ppEntry, currentValue, replaced);
        g_HookOwnership.erase(ppEntry);
        if (ppOriginal)
            *ppOriginal = previousCallerOriginal;
        return ErrorPatchFailed;
    }

    // If another injector replaced us after the successful CAS, its trampoline
    // can already retain pDetour as the next link. Keep the predecessor and the
    // ownership record published so that CE remains callable in that chain.
    if (*ppEntry != pDetour) {
        HookLogImportant(
            "VTableHook: Create - foreign hook followed CE at %p (current=%p); retaining CE chain state",
            ppEntry, *ppEntry);
    }

    return Success;
}

Status Enable(void* pTarget) {
    (void)pTarget;
    return Success;
}

Status Disable(void* pTarget) {
    (void)pTarget;
    return Success;
}

Status Remove(void* pVTableEntry, void* pOriginal) {
    if (!pVTableEntry || !pOriginal)
        return ErrorNotExecutable;

    // CRITICAL FIX: Lock mutex to prevent concurrent hook creation/removal
    std::lock_guard<std::mutex> lock(g_VTableMutex);

    void** ppEntry = reinterpret_cast<void**>(pVTableEntry);
    const auto ownership = g_HookOwnership.find(ppEntry);
    if (ownership == g_HookOwnership.end()) {
        HookLog("VTableHook: Remove - no CE ownership record for %p", ppEntry);
        return ErrorNotCreated;
    }
    MEMORY_BASIC_INFORMATION slotMemory = {};
    if (!QueryReadablePointerSlot(ppEntry, &slotMemory) ||
        slotMemory.AllocationBase != ownership->second.allocationBase) {
        HookLogImportant("VTableHook: Remove - dropping stale ownership record for unavailable slot %p", ppEntry);
        g_HookOwnership.erase(ownership);
        return Success;
    }
    if (*ppEntry != ownership->second.detour) {
        if (ce::vtable_hook_policy::ShouldPreserveForeignFollower(
                *ppEntry, ownership->second.detour, ownership->second.original)) {
            HookLogImportant(
                "VTableHook: Remove - preserving foreign replacement %p at %p and retaining CE chain ownership "
                "(detour=%p)",
                *ppEntry, ppEntry, ownership->second.detour);
            return Success;
        } else {
            HookLog("VTableHook: Remove - slot %p was already restored to predecessor %p", ppEntry, *ppEntry);
        }
        g_HookOwnership.erase(ownership);
        return Success;
    }

    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(ppEntry), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("VTableHook: Remove - VirtualProtect FAILED for %p (Error=%lu)", ppEntry, GetLastError());
        return ErrorMemoryProtect;
    }

    void* restoreValue = ownership->second.original ? ownership->second.original : pOriginal;
    void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(ppEntry), restoreValue,
                                                       ownership->second.detour);

    VirtualProtect(reinterpret_cast<void*>(ppEntry), sizeof(void*), oldProtect, &oldProtect);

    if (replaced != ownership->second.detour) {
        HookLogImportant("VTableHook: Remove - preserving concurrent foreign replacement %p at %p", replaced,
                         ppEntry);
        return Success;
    }

    HookLog("VTableHook: Remove - Restored %p to %p", ppEntry, *ppEntry);
    g_HookOwnership.erase(ownership);
    return Success;
}

const char* StatusToString(Status status) {
    switch (status) {
        case Success:
            return "Success";
        case ErrorAlreadyInitialized:
            return "Already initialized";
        case ErrorNotInitialized:
            return "Not initialized";
        case ErrorAlreadyCreated:
            return "Already created";
        case ErrorNotCreated:
            return "Not created";
        case ErrorEnabled:
            return "Enabled";
        case ErrorDisabled:
            return "Disabled";
        case ErrorNotExecutable:
            return "Not executable";
        case ErrorUnsupportedFunction:
            return "Unsupported function";
        case ErrorMemoryAlloc:
            return "Memory allocation failed";
        case ErrorMemoryProtect:
            return "Memory protection failed";
        case ErrorModuleNotFound:
            return "Module not found";
        case ErrorFunctionNotFound:
            return "Function not found";
        case ErrorPatchFailed:
            return "Patch verification failed";
        default:
            return "Unknown error";
    }
}

}  // namespace VTableHook
