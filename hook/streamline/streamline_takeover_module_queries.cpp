#include "streamline_interposer_takeover.h"

#include "hook/hooking/iat_hook.h"
#include "hook/runtime/graphics_runtime_module_policy.h"
#include "hook/runtime/hook_common.h"

// GetModuleHandle* redirection for the sl.interposer.dll takeover: once the
// override is published, a query for "sl.interposer.dll" (including the
// interposer's own plugin-directory lookup) names the override, never the
// dormant game-directory image. Queries never load or wait; see
// RuntimePublication.
namespace ce::streamline_interposer_takeover {

#if defined(_M_X64) || defined(__x86_64__)

namespace {

bool ShouldRedirectModuleHandleQuery(const char* nameOrPath) {
    if (!IsTakeoverArmed() || !nameOrPath || !nameOrPath[0]) {
        return false;
    }
    return ce::graphics_runtime::IsStreamlineInterposerModuleQuery(nameOrPath);
}

bool ShouldRedirectModuleHandleQuery(const wchar_t* nameOrPath) {
    if (!IsTakeoverArmed() || !nameOrPath || !nameOrPath[0]) {
        return false;
    }
    return ce::graphics_runtime::IsStreamlineInterposerModuleQuery(nameOrPath);
}

HMODULE ResolveCustomInterposerForModuleQuery() {
    return PublishedCustomInterposerForModuleQuery();
}

}  // namespace

void InstallModuleHandleHooksForAllModules() {
    void* dummy = nullptr;
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&HookedGetModuleHandleA),
                                &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&HookedGetModuleHandleW),
                                &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleExA",
                                reinterpret_cast<void*>(&HookedGetModuleHandleExA), &dummy);
    IATHook::PatchIATAllModules("kernel32.dll", "GetModuleHandleExW",
                                reinterpret_cast<void*>(&HookedGetModuleHandleExW), &dummy);
}

void PatchModuleHandleIatForModule(HMODULE module) {
    if (!module) {
        return;
    }
    void* dummy = nullptr;
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleA", reinterpret_cast<void*>(&HookedGetModuleHandleA),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleW", reinterpret_cast<void*>(&HookedGetModuleHandleW),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleExA", reinterpret_cast<void*>(&HookedGetModuleHandleExA),
                      &dummy);
    IATHook::PatchIAT(module, "kernel32.dll", "GetModuleHandleExW", reinterpret_cast<void*>(&HookedGetModuleHandleExW),
                      &dummy);
}

HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return custom;
        }
    }
    return ::GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return custom;
        }
    }
    return ::GetModuleHandleW(lpModuleName);
}

BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(custom), phModule);
        }
    }
    return ::GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if (!IsProcessTerminating() && !HookIsShuttingDown() &&
        (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 && ShouldRedirectModuleHandleQuery(lpModuleName)) {
        if (HMODULE custom = ResolveCustomInterposerForModuleQuery()) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(custom), phModule);
        }
    }
    return ::GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

#else

void InstallModuleHandleHooksForAllModules() {}
void PatchModuleHandleIatForModule(HMODULE) {}
HMODULE WINAPI HookedGetModuleHandleA(LPCSTR lpModuleName) { return ::GetModuleHandleA(lpModuleName); }
HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName) { return ::GetModuleHandleW(lpModuleName); }
BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    return ::GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}
BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    return ::GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

#endif

}  // namespace ce::streamline_interposer_takeover
