/**
 * VTable Hooking Utility Implementation
 */

#include "vtable_hook.h"

namespace VTableHook {

    static bool g_Initialized = false;

    Status Initialize() {
        if (g_Initialized) return ErrorAlreadyInitialized;
        g_Initialized = true;
        return Success;
    }

    Status Shutdown() {
        g_Initialized = false;
        return Success;
    }

    Status Create(void* pVTableEntry, void* pDetour, void** ppOriginal) {
        if (!pVTableEntry || !pDetour) return ErrorNotExecutable;
        
        // VTable hook - pTarget is &vtable[index]
        void** ppEntry = reinterpret_cast<void**>(pVTableEntry);
        
        // Save original function pointer if requested
        if (ppOriginal) {
            *ppOriginal = *ppEntry;
        }
        
        // Patch the vtable entry directly
        DWORD oldProtect;
        if (!VirtualProtect(ppEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            return ErrorMemoryProtect;
        }
        
        *ppEntry = pDetour;
        
        VirtualProtect(ppEntry, sizeof(void*), oldProtect, &oldProtect);
        
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

    const char* StatusToString(Status status) {
        switch (status) {
            case Success: return "Success";
            case ErrorAlreadyInitialized: return "Already initialized";
            case ErrorNotInitialized: return "Not initialized";
            case ErrorAlreadyCreated: return "Already created";
            case ErrorNotCreated: return "Not created";
            case ErrorEnabled: return "Enabled";
            case ErrorDisabled: return "Disabled";
            case ErrorNotExecutable: return "Not executable";
            case ErrorUnsupportedFunction: return "Unsupported function";
            case ErrorMemoryAlloc: return "Memory allocation failed";
            case ErrorMemoryProtect: return "Memory protection failed";
            case ErrorModuleNotFound: return "Module not found";
            case ErrorFunctionNotFound: return "Function not found";
            default: return "Unknown error";
        }
    }

} // namespace VTableHook
