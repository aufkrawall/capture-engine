#pragma once

// Whether a graphics API DLL in the process belongs to a translation runtime, by its version
// resource (DXUP's DLLs say ProductName "DXUP"). The verdict is cached per loaded image, so the
// overlay may ask every frame; a reloaded DLL is read again.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "hook/runtime/dll_utils.h"

namespace ce::api_translator {

inline bool IsProjectModuleCached(const char* dllName, const char* versionNeedle, std::atomic<uintptr_t>& cache) {
    constexpr uintptr_t kUnevaluated = ~static_cast<uintptr_t>(0);
    const HMODULE module = GetModuleHandleA(dllName);
    const uintptr_t moduleBits = reinterpret_cast<uintptr_t>(module);
    const uintptr_t cached = cache.load(std::memory_order_acquire);
    if (cached != kUnevaluated && (cached & ~static_cast<uintptr_t>(1)) == moduleBits)
        return (cached & 1u) != 0;
    const bool answer = module && IsDllFromProject(dllName, versionNeedle);
    cache.store(moduleBits | (answer ? 1u : 0u), std::memory_order_release);
    return answer;
}

// DXUP's frontend and backend DLLs: ddraw.dll, d3d8.dll, d3d9.dll, d3d11.dll, dxgi.dll.
inline bool IsDxupModuleLoaded(const char* dllName) {
    static const char* const kNames[] = {"ddraw.dll", "d3d8.dll", "d3d9.dll", "d3d11.dll", "dxgi.dll"};
    static std::atomic<uintptr_t> s_cache[5] = {~uintptr_t{0}, ~uintptr_t{0}, ~uintptr_t{0}, ~uintptr_t{0},
                                                ~uintptr_t{0}};
    for (size_t i = 0; i < 5; ++i) {
        if (_stricmp(kNames[i], dllName) == 0)
            return IsProjectModuleCached(dllName, "dxup", s_cache[i]);
    }
    return false;
}

// Whether `module` is one of DXUP's DLLs (its frontends call its own d3d11 like any application).
inline bool IsDxupModuleHandle(HMODULE module) {
    if (!module)
        return false;
    for (const char* name : {"ddraw.dll", "d3d8.dll", "d3d9.dll", "d3d11.dll", "dxgi.dll"}) {
        if (GetModuleHandleA(name) == module)
            return IsDxupModuleLoaded(name);
    }
    return false;
}

}  // namespace ce::api_translator
