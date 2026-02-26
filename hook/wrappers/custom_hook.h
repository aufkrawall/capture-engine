/**
 * Custom Hook System - MinHook-free implementation
 *
 * Provides unified hooking API using:
 * - Direct VTable patching (for COM interfaces)
 * - IAT patching (for API exports)
 * - No trampoline-based hooking (better DX12/FG compatibility)
 */

#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>

namespace CustomHook {

// ============================================================================
// Status Codes
// ============================================================================

enum class Status {
    Success = 0,
    ErrorNotInitialized,
    ErrorAlreadyInitialized,
    ErrorInvalidParameter,
    ErrorModuleNotFound,
    ErrorFunctionNotFound,
    ErrorMemoryProtect,
    ErrorAlreadyHooked,
    ErrorNotHooked,
    ErrorUnknown
};

// Convert status to string
const char* StatusToString(Status status);

// ============================================================================
// Hook Info Structure (for internal tracking)
// ============================================================================

struct HookInfo {
    enum class Type { IAT, VTable, Function };
    void* target = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
    std::atomic<bool> enabled{false};
    Type type = Type::Function;
};

// ============================================================================
// Initialization
// ============================================================================

// Initialize the custom hook system
bool Initialize();

// Shutdown and cleanup all hooks
void Shutdown();

// Check if initialized
bool IsInitialized();

// ============================================================================
// VTable Hooking (for COM interfaces)
// ============================================================================

/**
 * Hook a VTable method by replacing the pointer at vtable[index]
 *
 * @param vtable    Pointer to the vtable (obtained via *(void***)pInterface)
 * @param index     Method index in the vtable
 * @param detour    Your replacement function
 * @param original  [out] Receives the original function pointer
 * @return Status code
 */
Status HookVTableMethod(void** vtable, UINT index, void* detour, void** original);

/**
 * Hook a VTable entry directly (address of vtable[index])
 * This is equivalent to VTableHook::Create
 *
 * @param vtableEntry  Address of the vtable entry (&vtable[index])
 * @param detour       Your replacement function
 * @param original     [out] Receives the original function pointer
 * @return Status code
 */
Status HookVTableEntry(void** vtableEntry, void* detour, void** original);

/**
 * Unhook a VTable method
 *
 * @param vtable    Pointer to the vtable
 * @param index     Method index
 * @param original  Original function to restore
 * @return Status code
 */
Status UnhookVTableMethod(void** vtable, UINT index, void* original);

/**
 * Unhook a VTable entry directly
 */
Status UnhookVTableEntry(void** vtableEntry, void* original);

// ============================================================================
// Export Hooking (via IAT patching)
// ============================================================================

/**
 * Hook an exported function using IAT patching
 * This hooks all modules that import the function
 *
 * @param moduleName    DLL name (e.g., "dxgi.dll")
 * @param functionName  Exported function name
 * @param detour        Your replacement function
 * @param original      [out] Receives the original function pointer
 * @return Status code
 */
Status HookExport(const char* moduleName, const char* functionName, void* detour, void** original);

/**
 * Hook an exported function (wide string module name)
 */
Status HookExportW(const wchar_t* moduleName, const char* functionName, void* detour, void** original);

/**
 * Unhook an exported function
 */
Status UnhookExport(const char* moduleName, const char* functionName, void* original);

// ============================================================================
// Function Hooking (for inline patching - minimal use)
// ============================================================================

/**
 * Hook a function at a specific address
 * Uses VTable-style patching if target is a known vtable entry,
 * otherwise falls back to IAT or returns error
 *
 * @param target    Target function address
 * @param detour    Your replacement function
 * @param original  [out] Receives the original function pointer
 * @return Status code
 */
Status HookFunction(void* target, void* detour, void** original);

/**
 * Unhook a function
 */
Status UnhookFunction(void* target, void* original);

// ============================================================================
// Enable/Disable (no-op for VTable hooks, tracked for reference)
// ============================================================================

// ============================================================================
// Convenience wrappers (API compatibility)
// ============================================================================

// CreateHook - wrapper around HookFunction for API compatibility
inline Status CreateHook(void* target, void* detour, void** original) {
    return HookFunction(target, detour, original);
}

// EnableHook - VTable hooks are always enabled, this is for API compatibility
inline Status EnableHook(void* target) {
    (void)target;
    return Status::Success;
}

// DisableHook - VTable hooks are always enabled, this is for API compatibility
inline Status DisableHook(void* target) {
    (void)target;
    return Status::Success;
}

// ============================================================================
// TypedHook Template
// ============================================================================

class ScopedInitializer {
public:
    ScopedInitializer() {
        m_initialized = Initialize();
    }
    bool IsInitialized() const {
        return m_initialized;
    }

private:
    bool m_initialized = false;
};

// ============================================================================
// Type-safe Hook Template
// ============================================================================

template <typename FuncType>
class TypedHook {
    void* m_target = nullptr;
    FuncType m_original = nullptr;
    std::atomic<bool> m_created{false};

public:
    TypedHook() = default;
    ~TypedHook() {
        if (m_created.load() && m_target) {
            CustomHook::UnhookFunction(m_target, reinterpret_cast<void*>(m_original));
        }
    }

    // Disable copy/move
    TypedHook(const TypedHook&) = delete;
    TypedHook& operator=(const TypedHook&) = delete;

    bool CreateVTable(void** vtableEntry, void* detour) {
        if (m_created.load())
            return false;
        void* orig = nullptr;
        if (CustomHook::HookVTableEntry(vtableEntry, detour, &orig) != CustomHook::Status::Success) {
            return false;
        }
        m_target = vtableEntry;
        m_original = reinterpret_cast<FuncType>(orig);
        m_created.store(true);
        return true;
    }

    bool CreateExport(const char* moduleName, const char* functionName, void* detour) {
        if (m_created.load())
            return false;
        void* orig = nullptr;
        if (CustomHook::HookExport(moduleName, functionName, detour, &orig) != CustomHook::Status::Success) {
            return false;
        }
        HMODULE hMod = GetModuleHandleA(moduleName);
        m_target = hMod ? reinterpret_cast<void*>(GetProcAddress(hMod, functionName)) : nullptr;
        m_original = reinterpret_cast<FuncType>(orig);
        m_created.store(true);
        return true;
    }

    FuncType Original() const {
        return m_original;
    }
    bool IsCreated() const {
        return m_created.load();
    }

    template <typename... Args>
    auto operator()(Args&&... args) -> decltype(auto) {
        return m_original(std::forward<Args>(args)...);
    }
};

}  // namespace CustomHook
