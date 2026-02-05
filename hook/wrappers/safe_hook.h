/**
 * Safe Hooking Wrapper - Stub Implementation
 *
 * MinHook has been removed. This provides stub functions that return false.
 * IAT patching is used instead for API interception.
 */

#pragma once

#include <windows.h>
#include <atomic>
#include <mutex>

namespace SafeHook {

// Initialization status
extern std::atomic<bool> g_Initialized;
extern std::mutex g_HookMutex;

/**
 * Initialize subsystem (no-op, always returns true)
 */
bool Initialize();

/**
 * Shutdown (no-op)
 */
void Shutdown();

/**
 * Create a hook for a target function (stub - returns false)
 */
bool CreateHook(void* pTarget, void* pDetour, void** ppOriginal);

/**
 * Enable a previously created hook (stub - returns false)
 */
bool EnableHook(void* pTarget);

/**
 * Disable a hook (stub - returns false)
 */
bool DisableHook(void* pTarget);

/**
 * Remove a hook permanently (stub - returns false)
 */
bool RemoveHook(void* pTarget);

/**
 * Hook a COM interface method (stub - returns false)
 */
bool HookCOMInterface(void** ppVTableEntry, void* pDetour, void** ppOriginal);

/**
 * Thread-safe scoped hook guard (no-op)
 */
class ScopedHookDisable {
    void* m_pTarget;
public:
    ScopedHookDisable(void* pTarget) : m_pTarget(pTarget) {}
    ~ScopedHookDisable() {}
};

} // namespace SafeHook
