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

// Which runtime the process's d3d9.dll is. Any d3d9.dll outside System32 is a translation runtime
// CE must not probe with a synthetic device (ShouldBootstrapD3D9Hooks in vulkan_renderer_policy.h);
// which one is for the log and the label. Until 2026-10-18 every such DLL was called DXVK: DXUP's
// d3d9.dll in Mirror's Edge logged `dxvkD3D9=1` (20261018_151515).
enum class D3D9RuntimeKind : unsigned char { kNotLoaded, kSystem, kDXVK, kDXUP, kOtherReplacement };

inline D3D9RuntimeKind SelectD3D9RuntimeKind(bool loaded, bool outsideSystem32, bool dxvkVersion,
                                             bool dxupVersion) {
    if (!loaded)
        return D3D9RuntimeKind::kNotLoaded;
    if (!outsideSystem32)
        return D3D9RuntimeKind::kSystem;
    if (dxupVersion)
        return D3D9RuntimeKind::kDXUP;
    return dxvkVersion ? D3D9RuntimeKind::kDXVK : D3D9RuntimeKind::kOtherReplacement;
}

inline bool IsNonSystemD3D9Runtime(D3D9RuntimeKind kind) {
    return kind == D3D9RuntimeKind::kDXVK || kind == D3D9RuntimeKind::kDXUP ||
           kind == D3D9RuntimeKind::kOtherReplacement;
}

inline const char* D3D9RuntimeKindName(D3D9RuntimeKind kind) {
    switch (kind) {
        case D3D9RuntimeKind::kSystem:
            return "system";
        case D3D9RuntimeKind::kDXVK:
            return "DXVK";
        case D3D9RuntimeKind::kDXUP:
            return "DXUP";
        case D3D9RuntimeKind::kOtherReplacement:
            return "non-system";
        default:
            return "none";
    }
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
