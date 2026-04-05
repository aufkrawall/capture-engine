/**
 * Hook System - Unified hooking API
 *
 * Now using CustomHook (VTable + IAT patching) instead of MinHook.
 * Maintains API compatibility with existing code.
 */

#include "hook_system.h"
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include "../common/hook_common.h"
#include "custom_hook.h"
#include "vtable_hook.h"

namespace HookSystem {

static std::atomic<int> g_InitCount{0};
static std::shared_mutex g_HookMutex;
static std::unordered_map<void*, std::unique_ptr<HookHandle>> g_Hooks;

namespace {

#ifdef CE_UNIT_TESTS

const char* TestStatusToString(CustomHook::Status status) {
    switch (status) {
        case CustomHook::Status::Success:
            return "Success";
        case CustomHook::Status::ErrorNotInitialized:
            return "Not initialized";
        case CustomHook::Status::ErrorAlreadyInitialized:
            return "Already initialized";
        case CustomHook::Status::ErrorInvalidParameter:
            return "Invalid parameter";
        case CustomHook::Status::ErrorModuleNotFound:
            return "Module not found";
        case CustomHook::Status::ErrorFunctionNotFound:
            return "Function not found";
        case CustomHook::Status::ErrorMemoryProtect:
            return "Memory protection failed";
        case CustomHook::Status::ErrorAlreadyHooked:
            return "Already hooked";
        case CustomHook::Status::ErrorNotHooked:
            return "Not hooked";
        case CustomHook::Status::ErrorUnknown:
        default:
            return "Unknown error";
    }
}

bool TestInitialize() {
    return true;
}

void TestShutdown() {}

const char* TestGetStatusString(CustomHook::Status status) {
    return TestStatusToString(status);
}

CustomHook::Status TestHookFunction(void* target, void* detour, void** original) {
    if (!target || !detour) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    if (original) {
        *original = target;
    }
    return CustomHook::Status::Success;
}

CustomHook::Status TestHookExport(const char* moduleName, const char* functionName, void* detour, void** original) {
    if (!moduleName || !functionName || !detour) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    if (original) {
        *original = detour;
    }
    return CustomHook::Status::Success;
}

CustomHook::Status TestHookExportW(const wchar_t* moduleName, const char* functionName, void* detour, void** original) {
    if (!moduleName || !functionName || !detour) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    if (original) {
        *original = detour;
    }
    return CustomHook::Status::Success;
}

CustomHook::Status TestHookVTableEntry(void** vtableEntry, void* detour, void** original) {
    if (!vtableEntry || !detour) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    if (original) {
        *original = *vtableEntry;
    }
    *vtableEntry = detour;
    return CustomHook::Status::Success;
}

CustomHook::Status TestUnhookFunction(void* target, void* original) {
    (void)target;
    (void)original;
    return CustomHook::Status::Success;
}

CustomHook::Status TestUnhookExport(const char* moduleName, const char* functionName, void* original) {
    (void)moduleName;
    (void)functionName;
    (void)original;
    return CustomHook::Status::Success;
}

CustomHook::Status TestUnhookVTableEntry(void** vtableEntry, void* original) {
    if (!vtableEntry || !original) {
        return CustomHook::Status::ErrorInvalidParameter;
    }
    *vtableEntry = original;
    return CustomHook::Status::Success;
}

#else

bool DefaultInitialize() {
    return CustomHook::Initialize();
}

void DefaultShutdown() {
    CustomHook::Shutdown();
}

const char* DefaultGetStatusString(CustomHook::Status status) {
    return CustomHook::StatusToString(status);
}

CustomHook::Status DefaultHookFunction(void* target, void* detour, void** original) {
    return CustomHook::HookFunction(target, detour, original);
}

CustomHook::Status DefaultHookExport(const char* moduleName, const char* functionName, void* detour, void** original) {
    return CustomHook::HookExport(moduleName, functionName, detour, original);
}

CustomHook::Status DefaultHookExportW(const wchar_t* moduleName, const char* functionName, void* detour,
                                      void** original) {
    return CustomHook::HookExportW(moduleName, functionName, detour, original);
}

CustomHook::Status DefaultHookVTableEntry(void** vtableEntry, void* detour, void** original) {
    return CustomHook::HookVTableEntry(vtableEntry, detour, original);
}

CustomHook::Status DefaultUnhookFunction(void* target, void* original) {
    return CustomHook::UnhookFunction(target, original);
}

CustomHook::Status DefaultUnhookExport(const char* moduleName, const char* functionName, void* original) {
    return CustomHook::UnhookExport(moduleName, functionName, original);
}

CustomHook::Status DefaultUnhookVTableEntry(void** vtableEntry, void* original) {
    return CustomHook::UnhookVTableEntry(vtableEntry, original);
}

#endif

HookBackendOps MakeDefaultBackendOps() {
#ifdef CE_UNIT_TESTS
    return HookBackendOps{TestInitialize,   TestShutdown,         TestGetStatusString, TestHookFunction,
                          TestHookExport,   TestHookExportW,      TestHookVTableEntry, TestUnhookFunction,
                          TestUnhookExport, TestUnhookVTableEntry};
#else
    return HookBackendOps{DefaultInitialize,   DefaultShutdown,         DefaultGetStatusString, DefaultHookFunction,
                          DefaultHookExport,   DefaultHookExportW,      DefaultHookVTableEntry, DefaultUnhookFunction,
                          DefaultUnhookExport, DefaultUnhookVTableEntry};
#endif
}

HookBackendOps& BackendOps() {
    static HookBackendOps ops = MakeDefaultBackendOps();
    return ops;
}

void StoreHookHandle(void* key, HookType type, void* detour, void* original, const char* moduleName = nullptr,
                     const char* functionName = nullptr) {
    auto handle = std::make_unique<HookHandle>();
    handle->target = key;
    handle->detour = detour;
    handle->original = original;
    handle->type = type;
    if (moduleName) {
        handle->moduleName = moduleName;
    }
    if (functionName) {
        handle->functionName = functionName;
    }
    handle->enabled.store(true);
    g_Hooks[key] = std::move(handle);
}

}  // namespace

#ifdef CE_UNIT_TESTS
void SetHookBackendOpsForTesting(const HookBackendOps& ops) {
    BackendOps() = ops;
}

void ResetHookBackendOpsForTesting() {
    BackendOps() = MakeDefaultBackendOps();
}
#endif

bool Initialize() {
    int count = g_InitCount.fetch_add(1);
    if (count == 0) {
        if (!BackendOps().initialize()) {
            HookLog("HookSystem: Initialize failed");
            g_InitCount.fetch_sub(1);
            return false;
        }
        HookLog("HookSystem: Initialized (CustomHook backend)");
    }
    return true;
}

void Shutdown() {
    int count = g_InitCount.load(std::memory_order_acquire);
    while (count > 0) {
        if (g_InitCount.compare_exchange_weak(count, count - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (count <= 0) {
        return;
    }

    if (count == 1) {
        // Clear our tracking
        {
            std::unique_lock<std::shared_mutex> lock(g_HookMutex);
            g_Hooks.clear();
        }

        BackendOps().shutdown();
        HookLog("HookSystem: Shutdown complete");
    }
}

const char* GetStatusString(CustomHook::Status status) {
    return BackendOps().getStatusString(status);
}

bool CreateFunctionHook(void* target, void* detour, void** original) {
    if (!target || !detour)
        return false;

    // For function hooks, delegate to CustomHook
    CustomHook::Status status = BackendOps().hookFunction(target, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateFunctionHook failed - %s", BackendOps().getStatusString(status));
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        if (g_Hooks.find(target) != g_Hooks.end()) {
            HookLog("HookSystem: Duplicate function hook ignored at %p", target);
            return false;
        }
        StoreHookHandle(target, HookType::Function, detour, original ? *original : nullptr);
    }

    HookLog("HookSystem: Created function hook at %p", target);
    return true;
}

bool CreateExportHook(const char* moduleName, const char* functionName, void* detour, void** original) {
    if (!moduleName || !functionName || !detour)
        return false;

    // Use CustomHook's export hooking (IAT-based)
    CustomHook::Status status = BackendOps().hookExport(moduleName, functionName, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateExportHook failed for %s!%s - %s", moduleName, functionName,
                BackendOps().getStatusString(status));
        return false;
    }

    HMODULE hModule = GetModuleHandleA(moduleName);
    void* target = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, functionName)) : nullptr;

    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    void* key = target ? target : reinterpret_cast<void*>(detour);
    if (g_Hooks.find(key) != g_Hooks.end()) {
        HookLog("HookSystem: Duplicate export hook ignored for %s!%s", moduleName, functionName);
        return false;
    }
    StoreHookHandle(key, HookType::Export, detour, original ? *original : nullptr, moduleName, functionName);

    HookLog("HookSystem: Created export hook for %s!%s", moduleName, functionName);
    return true;
}

bool CreateExportHookW(const wchar_t* moduleName, const char* functionName, void* detour, void** original) {
    if (!moduleName || !functionName || !detour)
        return false;

    CustomHook::Status status = BackendOps().hookExportW(moduleName, functionName, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateExportHookW failed - %s", BackendOps().getStatusString(status));
        return false;
    }

    // Get the target address for tracking
    HMODULE hModule = GetModuleHandleW(moduleName);
    void* target = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, functionName)) : nullptr;

    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    void* key = target ? target : reinterpret_cast<void*>(detour);
    if (g_Hooks.find(key) != g_Hooks.end()) {
        HookLog("HookSystem: Duplicate export hook ignored for %ls!%s", moduleName, functionName);
        return false;
    }
    StoreHookHandle(key, HookType::Export, detour, original ? *original : nullptr, nullptr, functionName);

    return true;
}

bool CreateCOMHook(void** vtableEntry, void* detour, void** original) {
    if (!vtableEntry) {
        HookLog("HookSystem: Invalid vtable entry");
        return false;
    }

    // Use VTable hooking directly - this is the preferred method for COM
    CustomHook::Status status = BackendOps().hookVTableEntry(vtableEntry, detour, original);
    if (status != CustomHook::Status::Success) {
        HookLog("HookSystem: CreateCOMHook failed - %s", BackendOps().getStatusString(status));
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        if (g_Hooks.find(vtableEntry) != g_Hooks.end()) {
            HookLog("HookSystem: Duplicate COM hook ignored at %p", vtableEntry);
            return false;
        }
        StoreHookHandle(vtableEntry, HookType::COMVTable, detour, original ? *original : nullptr);
    }

    HookLog("HookSystem: Created COM hook at vtable entry %p", vtableEntry);
    return true;
}

bool EnableHook(void* target) {
    // VTable hooks are always enabled after creation
    // This function is for API compatibility
    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    auto it = g_Hooks.find(target);
    if (it != g_Hooks.end()) {
        it->second->enabled.store(true);
        return true;
    }
    return false;
}

bool DisableHook(void* target) {
    // VTable hooks cannot be truly disabled, but we can mark them as disabled
    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    auto it = g_Hooks.find(target);
    if (it != g_Hooks.end()) {
        it->second->enabled.store(false);
        return true;
    }
    return false;
}

void RemoveHook(void* target) {
    if (!target)
        return;

    HookType type = HookType::COMVTable;
    std::string moduleName;
    std::string functionName;
    void* original = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto it = g_Hooks.find(target);
        if (it != g_Hooks.end()) {
            original = it->second->original;
            type = it->second->type;
            moduleName = it->second->moduleName;
            functionName = it->second->functionName;
            g_Hooks.erase(it);
        } else {
            return;
        }
    }

    switch (type) {
        case HookType::Function:
            BackendOps().unhookFunction(target, original);
            break;
        case HookType::Export:
            if (!moduleName.empty() && !functionName.empty()) {
                BackendOps().unhookExport(moduleName.c_str(), functionName.c_str(), original);
            }
            break;
        case HookType::COMVTable:
            BackendOps().unhookVTableEntry(reinterpret_cast<void**>(target), original);
            break;
    }
}

bool EnableAllHooks() {
    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    for (auto& pair : g_Hooks) {
        pair.second->enabled.store(true);
    }
    return true;
}

bool DisableAllHooks() {
    std::unique_lock<std::shared_mutex> lock(g_HookMutex);
    for (auto& pair : g_Hooks) {
        pair.second->enabled.store(false);
    }
    return true;
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

}  // namespace HookSystem
