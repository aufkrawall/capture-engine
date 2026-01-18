/**
 * VTable Hooking Utility
 * 
 * Provides Direct VTable patching capabilities.
 * Replaces the legacy MinHook shim.
 */

#pragma once

#include <windows.h>
#include <vector>
#include <mutex>

namespace VTableHook {

    // Status codes for hook operations
    enum Status {
        Success = 0,
        ErrorAlreadyInitialized,
        ErrorNotInitialized,
        ErrorAlreadyCreated,
        ErrorNotCreated,
        ErrorEnabled,
        ErrorDisabled,
        ErrorNotExecutable,
        ErrorUnsupportedFunction,
        ErrorMemoryAlloc,
        ErrorMemoryProtect,
        ErrorModuleNotFound,
        ErrorFunctionNotFound,
        ErrorUnknown
    };

    // Initialize the hooking system
    Status Initialize();

    // Shutdown the hooking system
    Status Shutdown();

    /**
     * Create a hook for a virtual function table entry.
     * 
     * @param pVTableEntry Address of the VTable entry to patch (e.g. &vtable[10])
     * @param pDetour      Pointer to the detour function
     * @param ppOriginal   [Out] Receives the original function pointer
     */
    Status Create(void* pVTableEntry, void* pDetour, void** ppOriginal);

    /**
     * Enable a hook.
     * For VTable hooks, this is effectively a no-op as Create() enables immediately.
     * Kept for API compatibility during refactoring.
     */
    Status Enable(void* pTarget);

    /**
     * Disable a hook.
     * Note: This does NOT restore the original VTable entry automatically 
     * unless we track it properly. Current implementation is "Create-Only".
     */
    Status Disable(void* pTarget);

    // Helpers
    const char* StatusToString(Status status);

} // namespace VTableHook
