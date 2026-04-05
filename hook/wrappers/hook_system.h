#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

// Custom hook system (replaces MinHook)
#include "custom_hook.h"

namespace HookSystem {

// Initialize global hooking subsystem
bool Initialize();
void Shutdown();

// Hook types
enum class HookType {
    Function,
    Export,     // Function exported from DLL
    COMVTable,  // COM interface method
};

// Opaque hook handle
struct HookHandle {
    void* target = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
    HookType type = HookType::COMVTable;
    std::string moduleName;
    std::string functionName;
    std::atomic<bool> enabled{false};
};

// Create a hook
// Returns true on success, false on failure
bool CreateExportHook(const char* moduleName, const char* functionName, void* detour, void** original);

// Create hook using wide char module name
bool CreateExportHookW(const wchar_t* moduleName, const char* functionName, void* detour, void** original);

// Hook a COM vtable method directly
// target: address of the vtable entry (&vtable[index])
bool CreateCOMHook(void** vtableEntry, void* detour, void** original);

// Create hook on a specific function address
bool CreateFunctionHook(void* target, void* detour, void** original);

// Enable/disable hook
bool EnableHook(void* target);
bool DisableHook(void* target);

// Remove hook
void RemoveHook(void* target);

// Enable/disable all hooks
bool EnableAllHooks();
bool DisableAllHooks();

// Get hook status string (for compatibility)
const char* GetStatusString(CustomHook::Status status);

// RAII guard for hook initialization
class ScopedInitializer {
public:
    ScopedInitializer();
    ~ScopedInitializer();
    bool IsInitialized() const {
        return m_initialized;
    }

private:
    bool m_initialized = false;
};

struct HookBackendOps {
    bool (*initialize)();
    void (*shutdown)();
    const char* (*getStatusString)(CustomHook::Status status);
    CustomHook::Status (*hookFunction)(void* target, void* detour, void** original);
    CustomHook::Status (*hookExport)(const char* moduleName, const char* functionName, void* detour, void** original);
    CustomHook::Status (*hookExportW)(const wchar_t* moduleName, const char* functionName, void* detour,
                                      void** original);
    CustomHook::Status (*hookVTableEntry)(void** vtableEntry, void* detour, void** original);
    CustomHook::Status (*unhookFunction)(void* target, void* original);
    CustomHook::Status (*unhookExport)(const char* moduleName, const char* functionName, void* original);
    CustomHook::Status (*unhookVTableEntry)(void** vtableEntry, void* original);
};

#ifdef CE_UNIT_TESTS
void SetHookBackendOpsForTesting(const HookBackendOps& ops);
void ResetHookBackendOpsForTesting();
#endif

// Helper template for type-safe hook management
template <typename FuncType>
class TypedHook {
    void* m_Target = nullptr;
    FuncType m_Original = nullptr;
    std::atomic<bool> m_Created{false};
    std::atomic<bool> m_Enabled{false};

public:
    TypedHook() = default;
    ~TypedHook() {
        if (m_Created.load()) {
            RemoveHook(m_Target);
        }
    }

    // Disable copy/move
    TypedHook(const TypedHook&) = delete;
    TypedHook& operator=(const TypedHook&) = delete;
    TypedHook(TypedHook&&) = delete;
    TypedHook& operator=(TypedHook&&) = delete;

    bool Create(void* target, void* detour) {
        if (m_Created.load())
            return false;

        void* orig = nullptr;
        if (!CreateFunctionHook(target, detour, &orig)) {
            return false;
        }

        m_Target = target;
        m_Original = reinterpret_cast<FuncType>(orig);
        m_Created.store(true);
        m_Enabled.store(true);
        return true;
    }

    bool CreateExport(const char* moduleName, const char* functionName, void* detour) {
        if (m_Created.load())
            return false;

        void* orig = nullptr;
        if (!CreateExportHook(moduleName, functionName, detour, &orig)) {
            return false;
        }

        HMODULE hMod = GetModuleHandleA(moduleName);
        void* target = hMod ? reinterpret_cast<void*>(GetProcAddress(hMod, functionName)) : nullptr;
        m_Target = target ? target : detour;
        m_Original = reinterpret_cast<FuncType>(orig);
        m_Created.store(true);
        m_Enabled.store(true);
        return true;
    }

    bool Enable() {
        if (!m_Created.load() || m_Enabled.load())
            return false;
        if (EnableHook(m_Target)) {
            m_Enabled.store(true);
            return true;
        }
        return false;
    }

    bool Disable() {
        if (!m_Created.load() || !m_Enabled.load())
            return false;
        if (DisableHook(m_Target)) {
            m_Enabled.store(false);
            return true;
        }
        return false;
    }

    FuncType Original() const {
        return m_Original;
    }
    bool IsCreated() const {
        return m_Created.load();
    }
    bool IsEnabled() const {
        return m_Enabled.load();
    }

    // Call original function
    template <typename... Args>
    auto operator()(Args&&... args) -> decltype(auto) {
        return m_Original(std::forward<Args>(args)...);
    }
};

}  // namespace HookSystem
