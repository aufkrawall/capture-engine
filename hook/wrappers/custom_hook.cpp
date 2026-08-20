/**
 * Custom Hook System - MinHook-free implementation
 *
 * Uses existing VTableHook and IATHook implementations for maximum
 * compatibility.
 */

#include "custom_hook.h"
#include "../common/hook_common.h"
#include "../common/module_pin.h"
#include "iat_hook.h"
#include "vtable_hook.h"

#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace CustomHook {

// ============================================================================
// Internal State
// ============================================================================

static std::atomic<bool> g_Initialized{false};
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
static std::shared_mutex g_HookMutex;
static std::unordered_map<void*, std::unique_ptr<HookInfo>> g_Hooks;

// ============================================================================
// Status String Conversion
// ============================================================================

const char* StatusToString(Status status) {
    switch (status) {
        case Status::Success:
            return "Success";
        case Status::ErrorNotInitialized:
            return "Not initialized";
        case Status::ErrorAlreadyInitialized:
            return "Already initialized";
        case Status::ErrorInvalidParameter:
            return "Invalid parameter";
        case Status::ErrorModuleNotFound:
            return "Module not found";
        case Status::ErrorFunctionNotFound:
            return "Function not found";
        case Status::ErrorMemoryProtect:
            return "Memory protection failed";
        case Status::ErrorAlreadyHooked:
            return "Already hooked";
        case Status::ErrorNotHooked:
            return "Not hooked";
        case Status::ErrorUnknown:
            return "Unknown error";
        default:
            return "Unknown status";
    }
}

// ============================================================================
// Initialization
// ============================================================================

bool Initialize() {
    if (g_Initialized.load()) {
        HookLog("CustomHook: Already initialized");
        return true;
    }

    // Initialize underlying systems
    VTableHook::Initialize();
    IATHook::InitializeGetProcAddressHook();

    g_Initialized.store(true);
    HookLog("CustomHook: Initialized (VTable + IAT patching)");
    return true;
}

void Shutdown() {
    if (!g_Initialized.load()) {
        return;
    }

    HookLog("CustomHook: Shutting down...");

    // Unhook all registered hooks
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - hook teardown order is independent
        for (auto& [target, info] : g_Hooks) {
            if (info->type == HookInfo::Type::IAT) {
                // IAT hooks are cleaned up by IATHook::ShutdownIATHooks
            } else if (info->type == HookInfo::Type::VTable && info->original) {
                VTableHook::Remove(target, info->original);
            }
        }
        g_Hooks.clear();
    }

    // Shutdown underlying systems
    IATHook::ShutdownIATHooks();
    VTableHook::Shutdown();

    g_Initialized.store(false);
    HookLog("CustomHook: Shutdown complete");
}

bool IsInitialized() {
    return g_Initialized.load();
}

// ============================================================================
// VTable Hooking
// ============================================================================

Status HookVTableMethod(void** vtable, UINT index, void* detour, void** original) {
    if (!g_Initialized.load()) {
        return Status::ErrorNotInitialized;
    }
    if (!vtable || !detour) {
        return Status::ErrorInvalidParameter;
    }

    void** vtableEntry = &vtable[index];
    return HookVTableEntry(vtableEntry, detour, original);
}

Status HookVTableEntry(void** vtableEntry, void* detour, void** original) {
    if (!g_Initialized.load()) {
        return Status::ErrorNotInitialized;
    }
    if (!vtableEntry || !detour) {
        return Status::ErrorInvalidParameter;
    }

    // Use existing VTableHook implementation
    VTableHook::Status vStatus = VTableHook::Create(reinterpret_cast<void*>(vtableEntry), detour, original);

    if (vStatus == VTableHook::Success) {
        // Register in our tracking
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto info = std::make_unique<HookInfo>();
        info->target = reinterpret_cast<void*>(vtableEntry);
        info->detour = detour;
        info->original = original ? *original : nullptr;
        info->type = HookInfo::Type::VTable;
        info->enabled.store(true);
        g_Hooks[reinterpret_cast<void*>(vtableEntry)] = std::move(info);
        return Status::Success;
    }

    switch (vStatus) {
        case VTableHook::ErrorMemoryProtect:
            return Status::ErrorMemoryProtect;
        case VTableHook::ErrorNotExecutable:
            return Status::ErrorInvalidParameter;
        default:
            return Status::ErrorUnknown;
    }
}

Status UnhookVTableMethod(void** vtable, UINT index, void* original) {
    if (!vtable)
        return Status::ErrorInvalidParameter;
    return UnhookVTableEntry(&vtable[index], original);
}

Status UnhookVTableEntry(void** vtableEntry, void* original) {
    if (!vtableEntry)
        return Status::ErrorInvalidParameter;

    // Remove from tracking
    {
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto it = g_Hooks.find(reinterpret_cast<void*>(vtableEntry));
        if (it != g_Hooks.end()) {
            if (!original) {
                original = it->second->original;
            }
            g_Hooks.erase(it);
        }
    }

    if (original)
        return VTableHook::Remove(reinterpret_cast<void*>(vtableEntry), original) == VTableHook::Success
                   ? Status::Success
                   : Status::ErrorNotHooked;

    return Status::ErrorNotHooked;
}

// ============================================================================
// Export Hooking (via IAT)
// ============================================================================

Status HookExport(const char* moduleName, const char* functionName, void* detour, void** original) {
    if (!g_Initialized.load()) {
        return Status::ErrorNotInitialized;
    }
    if (!moduleName || !functionName || !detour) {
        return Status::ErrorInvalidParameter;
    }

    // Get module. Pinned, because the procAddr resolved from it is stored in
    // *original and registered with RegisterDynamicHook, so game code can call
    // through it for the rest of the process (see common/module_pin.h).
    HMODULE hMod = ce::module_pin::PinByName(moduleName);
    if (!hMod) {
        HookLog("CustomHook: Module not found: %s", moduleName);
        return Status::ErrorModuleNotFound;
    }

    // Get original function address
    void* procAddr = reinterpret_cast<void*>(GetProcAddress(hMod, functionName));
    if (!procAddr) {
        HookLog("CustomHook: Function not found: %s!%s", moduleName, functionName);
        return Status::ErrorFunctionNotFound;
    }

    // Store original if requested
    if (original) {
        *original = procAddr;
    }

    // Use IAT patching across all modules
    if (IATHook::PatchIATAllModules(moduleName, functionName, detour, original)) {
        // Also register for dynamic hooking (GetProcAddress intercept)
        IATHook::RegisterDynamicHook(functionName, detour, original);

        // Track the hook
        std::unique_lock<std::shared_mutex> lock(g_HookMutex);
        auto info = std::make_unique<HookInfo>();
        info->target = procAddr;
        info->detour = detour;
        info->original = original ? *original : procAddr;
        info->type = HookInfo::Type::IAT;
        info->enabled.store(true);
        g_Hooks[procAddr] = std::move(info);

        HookLog("CustomHook: Hooked export %s!%s", moduleName, functionName);
        return Status::Success;
    }

    HookLog("CustomHook: IAT patching failed for %s!%s (not imported?)", moduleName, functionName);
    // Even if IAT patching fails, dynamic hook is registered
    return Status::Success;
}

Status HookExportW(const wchar_t* moduleName, const char* functionName, void* detour, void** original) {
    if (!moduleName)
        return Status::ErrorInvalidParameter;

    // Convert wide to narrow
    char narrowName[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, moduleName, -1, narrowName, MAX_PATH, nullptr, nullptr);

    return HookExport(narrowName, functionName, detour, original);
}

Status UnhookExport(const char* moduleName, const char* functionName, void* original) {
    if (!moduleName || !functionName)
        return Status::ErrorInvalidParameter;

    // Restore via IAT
    // Note: IATHook::RestoreIAT handles the actual restoration
    if (IATHook::RestoreIAT(nullptr, moduleName, functionName, original)) {
        // Remove from tracking
        HMODULE hMod = GetModuleHandleA(moduleName);
        if (hMod) {
            void* procAddr = reinterpret_cast<void*>(GetProcAddress(hMod, functionName));
            std::unique_lock<std::shared_mutex> lock(g_HookMutex);
            g_Hooks.erase(procAddr);
        }
        return Status::Success;
    }

    return Status::ErrorNotHooked;
}

// ============================================================================
// Function Hooking
// ============================================================================

Status HookFunction(void* target, void* detour, void** original) {
    if (!g_Initialized.load()) {
        return Status::ErrorNotInitialized;
    }
    if (!target || !detour) {
        return Status::ErrorInvalidParameter;
    }

    HookLog("CustomHook: HookFunction called for %p - delegating to VTable patching", target);

    // target is expected to be a pointer to a function pointer (e.g., a vtable entry)
    return HookVTableEntry(reinterpret_cast<void**>(target), detour, original);
}

Status UnhookFunction(void* target, void* original) {
    return UnhookVTableEntry(reinterpret_cast<void**>(target), original);
}

// ============================================================================
// Hook Registry
// ============================================================================

const HookInfo* GetHookInfo(void* target) {
    std::shared_lock<std::shared_mutex> lock(g_HookMutex);
    auto it = g_Hooks.find(target);
    return (it != g_Hooks.end()) ? it->second.get() : nullptr;
}

size_t GetActiveHookCount() {
    std::shared_lock<std::shared_mutex> lock(g_HookMutex);
    return g_Hooks.size();
}

}  // namespace CustomHook
