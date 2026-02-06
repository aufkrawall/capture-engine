#include "hook_system.h"
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
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK) {
            HookLog("MinHook: Initialize failed - %s", MH_StatusToString(status));
            g_InitCount.fetch_sub(1);
            return false;
        }
        HookLog("MinHook: Initialized");
    }
    return true;
}

void Shutdown() {
    int count = g_InitCount.fetch_sub(1);
    if (count == 1) {
        // Disable and remove all hooks
        {
            std::unique_lock<std::shared_mutex> lock(g_HookMutex);
            for (auto& pair : g_Hooks) {
                MH_RemoveHook(pair.first);
            }
            g_Hooks.clear();
        }
        
        MH_STATUS status = MH_Uninitialize();
        HookLog("MinHook: Uninitialized - %s", MH_StatusToString(status));
    }
}

const char* GetStatusString(MH_STATUS status) {
    return MH_StatusToString(status);
}

bool CreateFunctionHook(void* target, void* detour, void** original) {
    if (!target || !detour) return false;
    
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        HookLog("MinHook: CreateHook failed - %s", MH_StatusToString(status));
        return false;
    }
    
    // Store hook info
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto handle = std::make_unique<HookHandle>();
        handle->target = target;
        handle->detour = detour;
        handle->original = original ? *original : nullptr;
        handle->enabled.store(false);
        g_Hooks[target] = std::move(handle);
    }
    
    // Enable immediately
    status = MH_EnableHook(target);
    if (status != MH_OK) {
        HookLog("MinHook: EnableHook failed - %s", MH_StatusToString(status));
        MH_RemoveHook(target);
        return false;
    }
    
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        if (auto it = g_Hooks.find(target); it != g_Hooks.end()) {
            it->second->enabled.store(true);
        }
    }
    
    return true;
}

bool CreateExportHook(const char* moduleName, const char* functionName, 
                     void* detour, void** original) {
    HMODULE hModule = GetModuleHandleA(moduleName);
    if (!hModule) {
        HookLog("MinHook: Module %s not loaded", moduleName);
        return false;
    }
    
    FARPROC proc = GetProcAddress(hModule, functionName);
    if (!proc) {
        HookLog("MinHook: Function %s not found in %s", functionName, moduleName);
        return false;
    }
    
    void* target = reinterpret_cast<void*>(proc);
    return CreateFunctionHook(target, detour, original);
}

bool CreateExportHookW(const wchar_t* moduleName, const char* functionName,
                       void* detour, void** original) {
    MH_STATUS status = MH_CreateHookApi(moduleName, functionName, detour, original);
    if (status != MH_OK) {
        HookLog("MinHook: CreateHookApi failed - %s", MH_StatusToString(status));
        return false;
    }
    
    // Get the target address for tracking
    HMODULE hModule = GetModuleHandleW(moduleName);
    FARPROC proc = hModule ? GetProcAddress(hModule, functionName) : nullptr;
    void* target = reinterpret_cast<void*>(proc);
    
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
    if (!vtableEntry || !*vtableEntry) {
        HookLog("MinHook: Invalid vtable entry");
        return false;
    }
    
    void* target = *vtableEntry;
    return CreateFunctionHook(target, detour, original);
}

bool EnableHook(void* target) {
    if (!target) return false;
    
    MH_STATUS status = MH_EnableHook(target);
    if (status == MH_OK) {
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
    
    MH_STATUS status = MH_DisableHook(target);
    if (status == MH_OK) {
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
    
    MH_RemoveHook(target);
    
    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    g_Hooks.erase(target);
}

bool EnableAllHooks() {
    MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK) {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        for (auto& pair : g_Hooks) {
            pair.second->enabled.store(true);
        }
        return true;
    }
    return false;
}

bool DisableAllHooks() {
    MH_STATUS status = MH_DisableHook(MH_ALL_HOOKS);
    if (status == MH_OK) {
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
