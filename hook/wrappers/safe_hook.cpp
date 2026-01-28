/**
 * Safe Hooking Wrapper using MinHook - Implementation
 */

#include "safe_hook.h"
#include "../common/hook_common.h"
#include <vector>
#include <algorithm>

namespace SafeHook {

std::atomic<bool> g_Initialized{false};
std::mutex g_HookMutex;

// Track all hooks for cleanup
struct HookInfo {
    void* pTarget;
    void* pDetour;
    void* pOriginal;
    bool enabled;
};
static std::vector<HookInfo> g_Hooks;
static std::mutex g_HooksMutex;

bool Initialize()
{
    if (g_Initialized.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);
    if (g_Initialized.load(std::memory_order_acquire)) {
        return true;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        HookLog("SafeHook: MH_Initialize failed: %s", MH_StatusToString(status));
        return false;
    }

    g_Initialized.store(true, std::memory_order_release);
    HookLog("SafeHook: MinHook initialized successfully");
    return true;
}

void Shutdown()
{
    if (!g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);
    
    // Disable all hooks first
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        for (auto& info : g_Hooks) {
            if (info.enabled) {
                MH_DisableHook(info.pTarget);
            }
        }
        g_Hooks.clear();
    }

    MH_Uninitialize();
    g_Initialized.store(false, std::memory_order_release);
    HookLog("SafeHook: MinHook shutdown");
}

bool CreateHook(void* pTarget, void* pDetour, void** ppOriginal)
{
    if (!g_Initialized.load(std::memory_order_acquire)) {
        if (!Initialize()) {
            return false;
        }
    }

    if (!pTarget || !pDetour) {
        HookLog("SafeHook: CreateHook called with null pointer");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);

    // Check if already hooked
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        for (const auto& info : g_Hooks) {
            if (info.pTarget == pTarget) {
                HookLog("SafeHook: Target %p already hooked", pTarget);
                if (ppOriginal) *ppOriginal = info.pOriginal;
                return true;
            }
        }
    }

    MH_STATUS status = MH_CreateHook(pTarget, pDetour, ppOriginal);
    if (status != MH_OK) {
        HookLog("SafeHook: MH_CreateHook failed for %p: %s", pTarget, MH_StatusToString(status));
        return false;
    }

    // Track the hook
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        HookInfo info;
        info.pTarget = pTarget;
        info.pDetour = pDetour;
        info.pOriginal = ppOriginal ? *ppOriginal : nullptr;
        info.enabled = false;
        g_Hooks.push_back(info);
    }

    HookLog("SafeHook: Created hook at %p -> %p", pTarget, pDetour);
    return true;
}

bool EnableHook(void* pTarget)
{
    if (!g_Initialized.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);

    MH_STATUS status = MH_EnableHook(pTarget);
    if (status != MH_OK) {
        HookLog("SafeHook: MH_EnableHook failed: %s", MH_StatusToString(status));
        return false;
    }

    // Update tracking
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        for (auto& info : g_Hooks) {
            if (info.pTarget == pTarget) {
                info.enabled = true;
                break;
            }
        }
    }

    return true;
}

bool DisableHook(void* pTarget)
{
    if (!g_Initialized.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);

    MH_STATUS status = MH_DisableHook(pTarget);
    if (status != MH_OK) {
        return false;
    }

    // Update tracking
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        for (auto& info : g_Hooks) {
            if (info.pTarget == pTarget) {
                info.enabled = false;
                break;
            }
        }
    }

    return true;
}

bool RemoveHook(void* pTarget)
{
    if (!g_Initialized.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_HookMutex);

    MH_STATUS status = MH_RemoveHook(pTarget);
    if (status != MH_OK) {
        return false;
    }

    // Remove from tracking
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        g_Hooks.erase(
            std::remove_if(g_Hooks.begin(), g_Hooks.end(),
                [pTarget](const HookInfo& info) { return info.pTarget == pTarget; }),
            g_Hooks.end()
        );
    }

    return true;
}

bool HookCOMInterface(void** ppVTableEntry, void* pDetour, void** ppOriginal)
{
    if (!ppVTableEntry || !pDetour) {
        return false;
    }

    void* pTarget = *ppVTableEntry;
    if (!pTarget) {
        HookLog("SafeHook: VTable entry is null");
        return false;
    }

    // Validate the target points to executable memory
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(pTarget, &mbi, sizeof(mbi))) {
        HookLog("SafeHook: VirtualQuery failed for %p", pTarget);
        return false;
    }

    if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        HookLog("SafeHook: Target %p is not executable (protect=0x%X)", pTarget, mbi.Protect);
        return false;
    }

    return CreateHook(pTarget, pDetour, ppOriginal);
}

// ScopedHookDisable implementation
ScopedHookDisable::ScopedHookDisable(void* pTarget) 
    : m_pTarget(pTarget), m_WasEnabled(false)
{
    if (!pTarget) return;

    // Check if hook is currently enabled
    {
        std::lock_guard<std::mutex> hooksLock(g_HooksMutex);
        for (const auto& info : g_Hooks) {
            if (info.pTarget == pTarget && info.enabled) {
                m_WasEnabled = true;
                break;
            }
        }
    }

    if (m_WasEnabled) {
        DisableHook(pTarget);
    }
}

ScopedHookDisable::~ScopedHookDisable()
{
    if (m_WasEnabled && m_pTarget) {
        EnableHook(m_pTarget);
    }
}

} // namespace SafeHook
