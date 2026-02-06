/**
 * Hook System - Unified hooking API
 * 
 * Now using CustomHook (VTable + IAT patching) instead of MinHook.
 * Maintains API compatibility with existing code.
 */

#include "hook_system.h"
#include "custom_hook.h"
#include "vtable_hook.h"
#include "../common/hook_common.h"
#include <unordered_map>
#include <shared_mutex>
#include <memory>

namespace HookSystem {

static std::atomic<int> g_InitCount{0};
static std::shared_mutex g_HookMutex;
static std::unordered_map<void*, std::unique_ptr<HookHandle>> g_Hooks;

bool Initialize() {
    int count = g_InitCount.fetch_add(1);
    if (count == 0) {
        if (!CustomHook::Initialize()) {
            HookLog("HookSystem: Initialize failed");
            g_InitCount.fetch_sub(1);
            return false;
        }
        HookLog("HookSystem: Initialized (CustomHook backend)");
    }
    return true;
}

void Shutdown() {
    int count = g_InitCount.fetch_sub(1);
    if (count == 1) {
        // Clear our tracking
        {
            std::unique_lock<std::shared_mutex> lock(g_HookMutex);
            g_Hooks.clear();
        }
        
        CustomHook::Shutdown();
        HookLog("HookSystem: Shutdown complete");
    }
}

const char* GetStatusString(CustomHook::Status status) {
    return CustomHook::StatusToString(status);
}

bool CreateFunctionHook(void* target, void* detour, void** original) {
    if (!target || !detour) return false;
    
    // For function hooks, delegate to CustomHook
    CustomHook::Status status = CustomHook::HookFunction(target, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateFunctionHook failed - %s", CustomHook::StatusToString(status));
        return false;
    }
    
    // Store hook info
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto handle = std::make_unique<HookHandle>();
        handle->target = target;
        handle->detour = detour;
        handle->original = original ? *original : nullptr;
        handle->enabled.store(true);
        g_Hooks[target] = std::move(handle);
    }
    
    HookLog("HookSystem: Created function hook at %p", target);
    return true;
}

bool CreateExportHook(const char* moduleName, const char* functionName, 
                     void* detour, void** original) {
    if (!moduleName || !functionName || !detour) return false;
    
    // Use CustomHook's export hooking (IAT-based)
    CustomHook::Status status = CustomHook::HookExport(moduleName, functionName, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateExportHook failed for %s!%s - %s", 
                moduleName, functionName, CustomHook::StatusToString(status));
        return false;
    }
    
    // Track the hook
    HMODULE hModule = GetModuleHandleA(moduleName);
    void* target = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, functionName)) : nullptr;
    
    if (target) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto handle = std::make_unique<HookHandle>();
        handle->target = target;
        handle->detour = detour;
        handle->original = original ? *original : nullptr;
        handle->enabled.store(true);
        g_Hooks[target] = std::move(handle);
    }
    
    HookLog("HookSystem: Created export hook for %s!%s", moduleName, functionName);
    return true;
}

bool CreateExportHookW(const wchar_t* moduleName, const char* functionName,
                       void* detour, void** original) {
    if (!moduleName || !functionName || !detour) return false;
    
    CustomHook::Status status = CustomHook::HookExportW(moduleName, functionName, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateExportHookW failed - %s", CustomHook::StatusToString(status));
        return false;
    }
    
    // Get the target address for tracking
    HMODULE hModule = GetModuleHandleW(moduleName);
    void* target = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, functionName)) : nullptr;
    
    if (target) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto handle = std::make_unique<HookHandle>();
        handle->target = target;
        handle->detour = detour;
        handle->original = original ? *original : nullptr;
        handle->enabled.store(true);
        g_Hooks[target] = std::move(handle);
    }
    
    return true;
}

bool CreateCOMHook(void** vtableEntry, void* detour, void** original) {
    if (!vtableEntry) {
        HookLog("HookSystem: Invalid vtable entry");
        return false;
    }
    
    // Use VTable hooking directly - this is the preferred method for COM
    CustomHook::Status status = CustomHook::HookVTableEntry(vtableEntry, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateCOMHook failed - %s", CustomHook::StatusToString(status));
        return false;
    }
    
    // Track using the vtable entry address as key
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto handle = std::make_unique<HookHandle>();
        handle->target = vtableEntry;
        handle->detour = detour;
        handle->original = original ? *original : nullptr;
        handle->enabled.store(true);
        g_Hooks[vtableEntry] = std::move(handle);
    }
    
    HookLog("HookSystem: Created COM hook at vtable entry %p", vtableEntry);
    return true;
}

bool EnableHook(void* target) {
    if (!target) return false;
    
    CustomHook::Status status = CustomHook::EnableHook(target);
    if (status == CustomHook::Status::Success) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        if (auto it = g_Hooks.find(target); it != g_Hooks.end()) {
            it->second->enabled.store(true);
        }
        return true;
    }
    return false;
}

bool DisableHook(void* target) {
    if (!target) return false;
    
    CustomHook::Status status = CustomHook::DisableHook(target);
    if (status == CustomHook::Status::Success) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        if (auto it = g_Hooks.find(target); it != g_Hooks.end()) {
            it->second->enabled.store(false);
        }
        return true;
    }
    return false;
}

void RemoveHook(void* target) {
    if (!target) return;
    
    // Get original from our tracking
    void* original = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto it = g_Hooks.find(target);
        if (it != g_Hooks.end()) {
            original = it->second->original;
            g_Hooks.erase(it);
        }
    }
    
    // Unhook based on type - try VTable first
    CustomHook::UnhookVTableEntry(reinterpret_cast<void**>(target), original);
}

bool EnableAllHooks() {
    CustomHook::Status status = CustomHook::EnableAllHooks();
    if (status == CustomHook::Status::Success) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        for (auto& pair : g_Hooks) {
            pair.second->enabled.store(true);
        }
        return true;
    }
    return false;
}

bool DisableAllHooks() {
    CustomHook::Status status = CustomHook::DisableAllHooks();
    if (status == CustomHook::Status::Success) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        for (auto& pair : g_Hooks) {
            pair.second->enabled.store(false);
        }
        return true;
    }
    return false;
}

// ScopedInitializer implementation
ScopedInitializer::ScopedInitializer() {
    m_initialized = Initialize();
}

ScopedInitializer::~ScopedInitializer() {
    if (m_initialized) {
        Shutdown();
    }
}

} // namespace HookSystem
