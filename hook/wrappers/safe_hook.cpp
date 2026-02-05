/**
 * Safe Hooking Wrapper - Stub Implementation
 *
 * MinHook has been removed. This provides stub functions.
 * IAT patching (iat_hook.cpp) is used instead for API interception.
 */

#include "safe_hook.h"
#include "../common/hook_common.h"

namespace SafeHook {

std::atomic<bool> g_Initialized{false};
std::mutex g_HookMutex;

bool Initialize()
{
    g_Initialized.store(true, std::memory_order_release);
    HookLog("SafeHook: Stub initialized (MinHook removed, use IAT patching)");
    return true;
}

void Shutdown()
{
    g_Initialized.store(false, std::memory_order_release);
}

bool CreateHook(void* pTarget, void* pDetour, void** ppOriginal)
{
    (void)pTarget;
    (void)pDetour;
    (void)ppOriginal;
    HookLog("SafeHook: CreateHook stub called - MinHook removed, use IAT patching instead");
    return false;
}

bool EnableHook(void* pTarget)
{
    (void)pTarget;
    return false;
}

bool DisableHook(void* pTarget)
{
    (void)pTarget;
    return false;
}

bool RemoveHook(void* pTarget)
{
    (void)pTarget;
    return false;
}

bool HookCOMInterface(void** ppVTableEntry, void* pDetour, void** ppOriginal)
{
    (void)ppVTableEntry;
    (void)pDetour;
    (void)ppOriginal;
    return false;
}

} // namespace SafeHook
