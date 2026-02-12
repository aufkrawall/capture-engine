/**
 * VTable Hooking Utility
 *
 * Direct VTable patching - no trampoline-based hooking.
 * Simpler and more compatible with other overlays/hooks.
 */

#pragma once

#include <atomic>
#include <windows.h>

namespace VTableHook {

// Status codes
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
  ErrorPatchFailed,
  ErrorUnknown
};

// Initialize the hooking system
Status Initialize();

// Shutdown the hooking system
Status Shutdown();

/**
 * Create a hook for a virtual function table entry.
 *
 * Uses direct VTable pointer patching - the vtable entry is replaced
 * with the detour function pointer. The original is saved to ppOriginal.
 *
 * @param pVTableEntry Address of the VTable entry (e.g. &vtable[10])
 * @param pDetour      Pointer to the detour function
 * @param ppOriginal   [Out] Receives the original function pointer
 */
Status Create(void *pVTableEntry, void *pDetour, void **ppOriginal);

// Enable a hook (no-op - VTable hooks are always enabled)
Status Enable(void *pTarget);

// Disable a hook (no-op - VTable hooks cannot be temporarily disabled)
Status Disable(void *pTarget);

// Convert status to string
const char *StatusToString(Status status);

} // namespace VTableHook
