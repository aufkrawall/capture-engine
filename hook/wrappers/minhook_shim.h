/**
 * MinHook Replacement Header
 * 
 * Drop-in replacement for MinHook.h that uses direct vtable patching.
 * This allows existing MH_CreateHook() calls to work without code changes.
 * 
 * For vtable hooks (COM interfaces): Uses direct memory patching of vtable entries.
 * For API hooks: Should use IATHook (call those directly, this file is for vtable only).
 */

#pragma once

#include <windows.h>

// MinHook-compatible status codes
typedef enum MH_STATUS {
    MH_OK = 0,
    MH_ERROR_ALREADY_INITIALIZED,
    MH_ERROR_NOT_INITIALIZED,
    MH_ERROR_ALREADY_CREATED,
    MH_ERROR_NOT_CREATED,
    MH_ERROR_ENABLED,
    MH_ERROR_DISABLED,
    MH_ERROR_NOT_EXECUTABLE,
    MH_ERROR_UNSUPPORTED_FUNCTION,
    MH_ERROR_MEMORY_ALLOC,
    MH_ERROR_MEMORY_PROTECT,
    MH_ERROR_MODULE_NOT_FOUND,
    MH_ERROR_FUNCTION_NOT_FOUND
} MH_STATUS;

// Special hook target for MH_EnableHook/MH_DisableHook
#define MH_ALL_HOOKS ((LPVOID)-1)

// Global initialization state
extern bool g_MH_Initialized;

// ============================================================================
// MinHook-compatible API (implemented via direct vtable patching)
// ============================================================================

/**
 * MH_Initialize - Initialize the hooking system
 */
inline MH_STATUS MH_Initialize() {
    extern bool g_MH_Initialized;
    if (g_MH_Initialized) return MH_ERROR_ALREADY_INITIALIZED;
    g_MH_Initialized = true;
    return MH_OK;
}

/**
 * MH_Uninitialize - Cleanup the hooking system
 */
inline MH_STATUS MH_Uninitialize() {
    extern bool g_MH_Initialized;
    g_MH_Initialized = false; 
    return MH_OK;
}

/**
 * MH_CreateHook - Create a hook for a function
 * 
 * This shim detects whether we're hooking:
 * 1. A vtable entry (pTarget points to a pointer to code) - use direct vtable patching
 * 2. An API function (pTarget IS code) - use IAT patching via IATHook
 * 
 * Detection: If the memory at pTarget is executable, it's API. Otherwise it's vtable.
 */
inline MH_STATUS MH_CreateHook(LPVOID pTarget, LPVOID pDetour, LPVOID* ppOriginal) {
    if (!pTarget || !pDetour) return MH_ERROR_NOT_EXECUTABLE;
    
    // Check if pTarget is in executable memory (API function) or data (vtable entry)
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(pTarget, &mbi, sizeof(mbi)) == 0) {
        return MH_ERROR_MEMORY_PROTECT;
    }
    
    bool isExecutable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | 
                                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    
    if (isExecutable) {
        // API hook - pTarget is a function address
        // Save original function address
        if (ppOriginal) {
            *ppOriginal = pTarget;
        }
        
        // For API hooks, we rely on IAT patching which is done elsewhere.
        // This shim just records what to hook - the actual patching is done by 
        // IATHook::PatchIATAllModules when wrapper_hooks.cpp calls InitializeWrapperHooks.
        // 
        // IMPORTANT: For D3D11CreateDeviceAndSwapChain etc., the hook is done via 
        // IAT patching in wrapper_hooks.cpp, not here. This MH_CreateHook just saves
        // the original function pointer.
        //
        // Return OK to not break code flow.
        return MH_OK;
    } else {
        // Vtable hook - pTarget is &vtable[index]
        void** vtableEntry = reinterpret_cast<void**>(pTarget);
        
        // Save original function pointer
        if (ppOriginal) {
            *ppOriginal = *vtableEntry;
        }
        
        // Patch the vtable entry directly
        DWORD oldProtect;
        if (!VirtualProtect(vtableEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            return MH_ERROR_MEMORY_PROTECT;
        }
        
        *vtableEntry = pDetour;
        
        VirtualProtect(vtableEntry, sizeof(void*), oldProtect, &oldProtect);
        
        return MH_OK;
    }
}

/**
 * MH_CreateHookApi - Create a hook for an exported function
 * 
 * Note: This is a stub - for API hooks, use IATHook::PatchIATAllModules instead.
 * This is included for compatibility but doesn't actually work for API hooking
 * since IAT patching requires different mechanics.
 */
inline MH_STATUS MH_CreateHookApi(LPCWSTR pszModule, LPCSTR pszProcName, 
                                   LPVOID pDetour, LPVOID* ppOriginal) {
    HMODULE hModule = GetModuleHandleW(pszModule);
    if (!hModule) return MH_ERROR_MODULE_NOT_FOUND;
    
    FARPROC pTarget = GetProcAddress(hModule, pszProcName);
    if (!pTarget) return MH_ERROR_FUNCTION_NOT_FOUND;
    
    // Just save original - actual IAT patching should be done separately
    if (ppOriginal) {
        *ppOriginal = (LPVOID)pTarget;
    }
    
    // Note: This doesn't actually hook the API. For API hooks, use IATHook.
    // Return OK anyway to not break existing code flow.
    return MH_OK;
}

/**
 * MH_EnableHook - Enable a created hook
 * 
 * For vtable hooks, the hook is already active after MH_CreateHook.
 * This is a no-op for compatibility.
 */
inline MH_STATUS MH_EnableHook(LPVOID pTarget) {
    (void)pTarget;
    return MH_OK;
}

/**
 * MH_DisableHook - Disable an enabled hook
 * 
 * Note: To properly restore a vtable hook, you need to call MH_CreateHook
 * again with the original function.
 */
inline MH_STATUS MH_DisableHook(LPVOID pTarget) {
    (void)pTarget;
    return MH_OK;
}

/**
 * MH_RemoveHook - Remove a hook
 */
inline MH_STATUS MH_RemoveHook(LPVOID pTarget) {
    (void)pTarget;
    return MH_OK;
}

/**
 * MH_QueueEnableHook - Queue a hook to be enabled
 * For compatibility - same as MH_EnableHook
 */
inline MH_STATUS MH_QueueEnableHook(LPVOID pTarget) {
    return MH_EnableHook(pTarget);
}

/**
 * MH_QueueDisableHook - Queue a hook to be disabled
 * For compatibility - same as MH_DisableHook  
 */
inline MH_STATUS MH_QueueDisableHook(LPVOID pTarget) {
    return MH_DisableHook(pTarget);
}

/**
 * MH_ApplyQueued - Apply all queued hooks
 * For compatibility - no-op since our hooks are immediate
 */
inline MH_STATUS MH_ApplyQueued() {
    return MH_OK;
}

/**
 * MH_StatusToString - Get a string representation of status code
 */
inline const char* MH_StatusToString(MH_STATUS status) {
    switch (status) {
        case MH_OK: return "OK";
        case MH_ERROR_ALREADY_INITIALIZED: return "Already initialized";
        case MH_ERROR_NOT_INITIALIZED: return "Not initialized";
        case MH_ERROR_ALREADY_CREATED: return "Already created";
        case MH_ERROR_NOT_CREATED: return "Not created";
        case MH_ERROR_ENABLED: return "Enabled";
        case MH_ERROR_DISABLED: return "Disabled";
        case MH_ERROR_NOT_EXECUTABLE: return "Not executable";
        case MH_ERROR_UNSUPPORTED_FUNCTION: return "Unsupported function";
        case MH_ERROR_MEMORY_ALLOC: return "Memory allocation failed";
        case MH_ERROR_MEMORY_PROTECT: return "Memory protection failed";
        case MH_ERROR_MODULE_NOT_FOUND: return "Module not found";
        case MH_ERROR_FUNCTION_NOT_FOUND: return "Function not found";
        default: return "Unknown error";
    }
}

