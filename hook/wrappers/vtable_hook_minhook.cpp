#include "vtable_hook_minhook.h"
#include "../common/hook_common.h"
#include "hook_system.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace VTableHookMH {

// Track hooked entries for restoration
struct HookInfo {
    void* vtableEntry;
    void* originalFunc;
    void* detourFunc;
    void* trampoline;
    std::atomic<bool> enabled{false};
};

static std::unordered_map<void*, std::shared_ptr<HookInfo>> g_Hooks;
static std::mutex g_HooksMutex;
static std::atomic<bool> g_Initialized{false};

Status Initialize()
{
    if (g_Initialized.load()) {
        return ErrorAlreadyInitialized;
    }

    // Initialize MinHook through HookSystem
    if (!HookSystem::Initialize()) {
        return ErrorMemoryAlloc;
    }

    g_Initialized.store(true);
    return Success;
}

Status Shutdown()
{
    if (!g_Initialized.load()) {
        return ErrorNotInitialized;
    }

    std::lock_guard<std::mutex> lock(g_HooksMutex);

    // Remove all hooks
    for (auto& pair : g_Hooks) {
        auto& info = pair.second;
        if (info->enabled.load()) {
            HookSystem::RemoveHook(info->originalFunc);
        }
    }
    g_Hooks.clear();

    g_Initialized.store(false);
    return Success;
}

Status Create(void* pVTableEntry, void* pDetour, void** ppOriginal)
{
    if (!pVTableEntry || !pDetour) {
        return ErrorNotExecutable;
    }

    // Get the actual function address from the vtable
    void* targetFunc = *(void**)pVTableEntry;
    if (!targetFunc) {
        return ErrorNotExecutable;
    }

    // Ensure MinHook is initialized
    if (!g_Initialized.load()) {
        Initialize();
    }

    std::lock_guard<std::mutex> lock(g_HooksMutex);

    // Check if already hooked
    if (g_Hooks.find(pVTableEntry) != g_Hooks.end()) {
        auto& info = g_Hooks[pVTableEntry];
        if (ppOriginal) {
            *ppOriginal = info->trampoline;
        }
        return ErrorAlreadyCreated;  // Already hooked, return existing trampoline
    }

    // Create the hook using MinHook
    void* trampoline = nullptr;
    if (!HookSystem::CreateFunctionHook(targetFunc, pDetour, &trampoline)) {
        HookLog("VTableHookMH: Failed to create hook for %p", targetFunc);
        return ErrorMemoryProtect;
    }

    // Store hook info
    auto info = std::make_shared<HookInfo>();
    info->vtableEntry = pVTableEntry;
    info->originalFunc = targetFunc;
    info->detourFunc = pDetour;
    info->trampoline = trampoline;
    info->enabled.store(true);

    g_Hooks[pVTableEntry] = info;

    if (ppOriginal) {
        *ppOriginal = trampoline;
    }

    HookLog("VTableHookMH: Hooked vtable entry %p -> %p (trampoline: %p)", pVTableEntry, targetFunc, trampoline);

    return Success;
}

Status Enable(void* pTarget)
{
    // MinHook hooks are enabled on creation
    // We could support enabling/disabling if needed
    std::lock_guard<std::mutex> lock(g_HooksMutex);

    auto it = g_Hooks.find(pTarget);
    if (it == g_Hooks.end()) {
        return ErrorNotCreated;
    }

    if (it->second->enabled.load()) {
        return ErrorEnabled;
    }

    HookSystem::EnableHook(it->second->originalFunc);
    it->second->enabled.store(true);

    return Success;
}

Status Disable(void* pTarget)
{
    std::lock_guard<std::mutex> lock(g_HooksMutex);

    auto it = g_Hooks.find(pTarget);
    if (it == g_Hooks.end()) {
        return ErrorNotCreated;
    }

    if (!it->second->enabled.load()) {
        return ErrorDisabled;
    }

    HookSystem::DisableHook(it->second->originalFunc);
    it->second->enabled.store(false);

    return Success;
}

Status Remove(void* pVTableEntry)
{
    std::lock_guard<std::mutex> lock(g_HooksMutex);

    auto it = g_Hooks.find(pVTableEntry);
    if (it == g_Hooks.end()) {
        return ErrorNotCreated;
    }

    HookSystem::RemoveHook(it->second->originalFunc);
    g_Hooks.erase(it);

    return Success;
}

bool IsHooked(void* pVTableEntry)
{
    std::lock_guard<std::mutex> lock(g_HooksMutex);
    return g_Hooks.find(pVTableEntry) != g_Hooks.end();
}

const char* StatusToString(Status status)
{
    switch (status) {
        case Success:
            return "Success";
        case ErrorAlreadyInitialized:
            return "Already initialized";
        case ErrorNotInitialized:
            return "Not initialized";
        case ErrorAlreadyCreated:
            return "Already created";
        case ErrorNotCreated:
            return "Not created";
        case ErrorEnabled:
            return "Enabled";
        case ErrorDisabled:
            return "Disabled";
        case ErrorNotExecutable:
            return "Not executable";
        case ErrorUnsupportedFunction:
            return "Unsupported function";
        case ErrorMemoryAlloc:
            return "Memory allocation failed";
        case ErrorMemoryProtect:
            return "Memory protection failed";
        case ErrorModuleNotFound:
            return "Module not found";
        case ErrorFunctionNotFound:
            return "Function not found";
        default:
            return "Unknown error";
    }
}

}  // namespace VTableHookMH

// Backwards compatibility - create namespace alias
namespace VTableHook {
using namespace VTableHookMH;
}
