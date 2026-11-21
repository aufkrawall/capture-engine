#pragma once

#include <cstring>

#include "hook/overlay/overlay_compat_detail/module_table.h"

namespace ce::dx12_overlay_policy {

// Native D3D12 runtime command-queue implementations (D3D12Core.dll / d3d12.dll)
// should be intercepted via an inline hook on the method body instead of patching
// ID3D12CommandQueue vtable slots [10] (ExecuteCommandLists) and [14] (Signal)
// in place. Co-resident frame-pacing tools such as ReLimiter 4.0 verify that
// vtable[10]/[14]/[15] remain inside D3D12Core.dll/d3d12.dll and disable their
// queue-based pacer and Streamline GPU handoff if another layer overwrites those
// vtable slots. Proxy, wrapper, debug-layer, and synthetic test queues keep
// per-vtable hooks.
inline bool IsNativeD3D12RuntimeModulePath(const char* modulePath) {
    if (!modulePath || !*modulePath)
        return false;
    const char* base = std::strrchr(modulePath, '\\');
    base = base ? base + 1 : modulePath;
    return ce::overlay_compat::detail::EqualsInsensitive(base, "d3d12core.dll") ||
           ce::overlay_compat::detail::EqualsInsensitive(base, "d3d12.dll");
}

inline bool ShouldInlineHookCommandQueueMethod(bool vtableModuleResolved, const char* vtableModulePath,
                                               bool methodModuleResolved, const char* methodModulePath) {
    return vtableModuleResolved && methodModuleResolved && IsNativeD3D12RuntimeModulePath(vtableModulePath) &&
           IsNativeD3D12RuntimeModulePath(methodModulePath);
}

}  // namespace ce::dx12_overlay_policy
