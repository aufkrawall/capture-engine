#include "streamline_interposer_takeover.h"

#include "hook/hooking/iat_hook.h"
#include "hook/runtime/graphics_runtime_module_policy.h"
#include "hook/runtime/hook_common.h"
#include "hook/runtime/module_replacement.h"

// GetModuleHandle* redirection. Two owners answer here, never loading or waiting (see
// RuntimePublication): a custom DLL replacing a resident module (module_replacement.h) for the
// callers its policy names, and the sl.interposer.dll takeover - once the override is published,
// a query for "sl.interposer.dll" (including the interposer's own plugin-directory lookup) names
// the override, never the dormant game-directory image.
namespace ce::streamline_interposer_takeover {

namespace {

// x64: the takeover may arm at any time, so the hooks are always in place. x86 has no
// Streamline; only an adopted replacement needs them there.
bool ModuleQueryHooksNeeded() {
#if defined(_M_X64) || defined(__x86_64__)
    return true;
#else
    return ce::module_replacement::IsActive();
#endif
}

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

template <typename CharT>
HMODULE RedirectedModuleForQuery(const CharT* nameOrPath, const void* callerAddress) {
    if (IsProcessTerminating() || HookIsShuttingDown()) {
        return nullptr;
    }
    if (HMODULE replacement = ce::module_replacement::ReplacementForModuleQuery(nameOrPath, callerAddress)) {
        return replacement;
    }
    return ShouldRedirectModuleHandleQuery(nameOrPath) ? PublishedCustomInterposerForModuleQuery() : nullptr;
}

}  // namespace

void InstallModuleHandleHooksForAllModules() {
    if (!ModuleQueryHooksNeeded()) {
        return;
    }
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
    if (!module || !ModuleQueryHooksNeeded()) {
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
    if (HMODULE redirected = RedirectedModuleForQuery(lpModuleName, __builtin_return_address(0))) {
        return redirected;
    }
    return ::GetModuleHandleA(lpModuleName);
}

HMODULE WINAPI HookedGetModuleHandleW(LPCWSTR lpModuleName) {
    if (HMODULE redirected = RedirectedModuleForQuery(lpModuleName, __builtin_return_address(0))) {
        return redirected;
    }
    return ::GetModuleHandleW(lpModuleName);
}

BOOL WINAPI HookedGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule) {
    if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        if (HMODULE redirected = RedirectedModuleForQuery(lpModuleName, __builtin_return_address(0))) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(redirected), phModule);
        }
    }
    return ::GetModuleHandleExA(dwFlags, lpModuleName, phModule);
}

BOOL WINAPI HookedGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule) {
    if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0) {
        if (HMODULE redirected = RedirectedModuleForQuery(lpModuleName, __builtin_return_address(0))) {
            return ::GetModuleHandleExW(dwFlags | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                        reinterpret_cast<LPCWSTR>(redirected), phModule);
        }
    }
    return ::GetModuleHandleExW(dwFlags, lpModuleName, phModule);
}

}  // namespace ce::streamline_interposer_takeover
