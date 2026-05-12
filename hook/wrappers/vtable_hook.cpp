/**
 * VTable Hooking Utility Implementation
 */

#include "vtable_hook.h"
#include <atomic>
#include <mutex>
#include "../common/hook_common.h"

namespace VTableHook {

static std::atomic<bool> g_Initialized{false};
static std::mutex g_VTableMutex;

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

    // VTable hook - pTarget is &vtable[index]
    void** ppEntry = reinterpret_cast<void**>(pVTableEntry);
    void* currentValue = *ppEntry;

    // NULL ENTRY GUARD: Do NOT patch a NULL vtable entry. Writing to a NULL
    // entry when the vtable is incomplete (e.g. sl_interposer wrapper devices
    // that only implement a subset of ID3D12Device methods) writes past the
    // vtable boundary, corrupting adjacent memory.
    if (!currentValue) {
        HookLog("VTableHook: Create - VTable entry %p is NULL! Cannot hook NULL entry. Skipping.", ppEntry);
        if (ppOriginal)
            *ppOriginal = nullptr;
        return ErrorNotExecutable;
    }

    // IDEMPOTENCY CHECK: Is the VTable entry already set to our detour?
    if (currentValue == pDetour) {
        HookLog(
            "VTableHook: Create - Target %p ALREADY contains Detour %p. "
            "Skipping write.",
            ppEntry, pDetour);
        // If the caller requested the original, we can't give the TRUE original
        // because it's lost from the VTable. However, the caller likely already has
        // it stored if they are calling Create again. If they don't (e.g. static
        // reset), they are in trouble (Original lost). But we should NOT overwrite
        // ppOriginal with *ppEntry (which is pDetour) because that creates a cycle.
        return Success;
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
        return Success;
    }

    // DEBUG: Log memory region details
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ppEntry, &mbi, sizeof(mbi))) {
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

    HookLog("VTableHook: Create - Patching %p (Original=%p, Detour=%p, SelfHook=%d)", ppEntry, currentValue, pDetour,
            isSelfHook ? 1 : 0);

    // Patch the vtable entry directly
    DWORD oldProtect;
    if (!VirtualProtect(ppEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("VTableHook: Create - VirtualProtect FAILED for %p (Error=%lu)", ppEntry, GetLastError());
        return ErrorMemoryProtect;
    }

    *ppEntry = pDetour;

    // CRITICAL FIX: Flush instruction cache after modifying code
    // While x86/x64 has strong cache coherency, this is required for correctness
    // and may be needed on certain configurations or future CPUs
    FlushInstructionCache(GetCurrentProcess(), ppEntry, sizeof(void*));

    DWORD newProtect;
    if (!VirtualProtect(ppEntry, sizeof(void*), oldProtect, &newProtect)) {
        HookLog("VTableHook: Create - VirtualProtect RESTORE FAILED for %p (Error=%lu)", ppEntry, GetLastError());
    }

    // Verify the patch
    void* verifyValue = *ppEntry;
    if (verifyValue != pDetour) {
        HookLog("VTableHook: Create - VERIFY FAILED! Expected=%p, Got=%p", pDetour, verifyValue);
        return ErrorPatchFailed;
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

    DWORD oldProtect;
    if (!VirtualProtect(ppEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("VTableHook: Remove - VirtualProtect FAILED for %p (Error=%lu)", ppEntry, GetLastError());
        return ErrorMemoryProtect;
    }

    *ppEntry = pOriginal;

    VirtualProtect(ppEntry, sizeof(void*), oldProtect, &oldProtect);

    HookLog("VTableHook: Remove - Restored %p to %p", ppEntry, pOriginal);
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
