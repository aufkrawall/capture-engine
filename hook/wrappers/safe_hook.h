/**
 * Safe Hooking Wrapper using MinHook
 * 
 * This provides a robust, thread-safe alternative to raw VTable hooking
 * for critical API interception points.
 */

#pragma once

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <mutex>

namespace SafeHook {

// Initialization status
extern std::atomic<bool> g_Initialized;
extern std::mutex g_HookMutex;

/**
 * Initialize MinHook subsystem
 * Call once at DLL load
 */
bool Initialize();

/**
 * Shutdown MinHook and restore all hooks
 */
void Shutdown();

/**
 * Create a hook for a target function
 * 
 * @param pTarget     Target function address
 * @param pDetour     Detour function address  
 * @param ppOriginal  Pointer to store original function
 * @return true on success
 */
bool CreateHook(void* pTarget, void* pDetour, void** ppOriginal);

/**
 * Enable a previously created hook
 */
bool EnableHook(void* pTarget);

/**
 * Disable a hook (temporary)
 */
bool DisableHook(void* pTarget);

/**
 * Remove a hook permanently
 */
bool RemoveHook(void* pTarget);

/**
 * Hook a COM interface method using VTable patching with MinHook backup
 * This is safer than raw VTable patching as it validates the target
 */
bool HookCOMInterface(void** ppVTableEntry, void* pDetour, void** ppOriginal);

/**
 * Thread-safe scoped hook guard
 * Automatically disables hook on construction, re-enables on destruction
 * Useful for calling original functions without recursion
 */
class ScopedHookDisable {
    void* m_pTarget;
    bool m_WasEnabled;
public:
    ScopedHookDisable(void* pTarget);
    ~ScopedHookDisable();
};

} // namespace SafeHook
