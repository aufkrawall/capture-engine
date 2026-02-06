#pragma once

/**
 * VTable Hook - MinHook Backend
 * 
 * This is a drop-in replacement for VTableHook that uses MinHook
 * internally for more stable hooking.
 * 
 * Instead of patching vtable entries directly (which can race with other hooks),
 * we use MinHook's trampoline-based approach on the actual function addresses.
 */

#include <windows.h>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace VTableHookMH {

// Status codes (compatible with original VTableHook)
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
 * This version uses MinHook internally instead of direct vtable patching.
 * It resolves the actual function address from the vtable and hooks that.
 * 
 * @param pVTableEntry Address of the VTable entry (e.g. &vtable[10])
 * @param pDetour      Pointer to the detour function
 * @param ppOriginal   [Out] Receives the original function pointer
 */
Status Create(void* pVTableEntry, void* pDetour, void** ppOriginal);

// Enable a hook (no-op for MinHook backend - hooks are enabled on creation)
Status Enable(void* pTarget);

// Disable a hook (no-op for MinHook backend)
Status Disable(void* pTarget);

// Helpers
const char* StatusToString(Status status);

// Check if a vtable entry is already hooked
bool IsHooked(void* pVTableEntry);

// Remove a hook (restore original)
Status Remove(void* pVTableEntry);

} // namespace VTableHookMH

// Backwards compatibility namespace
namespace VTableHook {
    using namespace VTableHookMH;
}
