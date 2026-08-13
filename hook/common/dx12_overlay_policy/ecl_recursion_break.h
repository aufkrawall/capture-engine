#pragma once

#include <cstring>

#include "../overlay_compat_detail/module_table.h"

// Selecting the recursion-break target for the ExecuteCommandLists detour.
//
// When a third-party overlay (ReShade) or an FG engine hooks
// ExecuteCommandLists, its saved "original" can loop back into CE's own
// detour. The detour then has to forward somewhere that provably does not
// loop back. ReShade additionally throws
// std::system_error(resource_deadlock_would_occur) when its proxy hook is
// re-entered with the wrapped real queue (Talos + ReShade-only, session
// 20260813_041416), so the break target must never be a foreign overlay
// proxy hook. Pure and unit-testable: all module-path resolution happens in
// the caller.
namespace ce::dx12_overlay_policy {

enum class EclBreakTargetClass {
    kUnresolved,
    kNativeD3D12,
    kForeignOverlayHook,
    kSelfHook,
    kOtherModule,
};

// Classifies a candidate ExecuteCommandLists address by its owning module
// path. `modulePathResolved`/`modulePath` come from
// TryGetModulePathFromCodeAddress.
inline EclBreakTargetClass ClassifyEclBreakTargetCandidate(bool modulePathResolved, const char* modulePath) {
    if (!modulePathResolved || !modulePath || !*modulePath) {
        return EclBreakTargetClass::kUnresolved;
    }

    const char* base = std::strrchr(modulePath, '\\');
    base = base ? base + 1 : modulePath;

    if (ce::overlay_compat::detail::EqualsInsensitive(base, "d3d12.dll") ||
        ce::overlay_compat::detail::EqualsInsensitive(base, "d3d12core.dll")) {
        return EclBreakTargetClass::kNativeD3D12;
    }
    if (ce::overlay_compat::detail::EqualsInsensitive(base, "capture_hook_x64.dll") ||
        ce::overlay_compat::detail::EqualsInsensitive(base, "capture_hook_x86.dll")) {
        return EclBreakTargetClass::kSelfHook;
    }
    if (ce::overlay_compat::IsThirdPartyOverlayModulePath(modulePath)) {
        return EclBreakTargetClass::kForeignOverlayHook;
    }
    return EclBreakTargetClass::kOtherModule;
}

enum class EclBreakSelection {
    kNone,
    kPerQueueOriginal,
    kRealD3D12Ecl,
    kGlobalOriginal,
};

// Selects which candidate to forward to. The selection is type-safe by
// construction: a native D3D12 runtime ECL is only ever used for a queue
// whose vtable is native, and a proxy queue is only ever forwarded through
// the original that was taken from its exact vtable. A known foreign overlay
// hook or CE's own detour is never selected for a native queue (it either
// throws, like ReShade's proxy mutex, or loops back immediately); the
// caller's recursion-depth bound terminates any loop-back of the remaining
// fail-open unknown-module targets.
inline EclBreakSelection SelectEclRecursionBreakTarget(EclBreakTargetClass queueVtable,
                                                       EclBreakTargetClass perQueueOriginal,
                                                       EclBreakTargetClass realD3D12Ecl,
                                                       EclBreakTargetClass globalOriginal) {
    if (queueVtable == EclBreakTargetClass::kNativeD3D12) {
        // Native queue object. The per-vtable original matches the layout and
        // is preferred when it is the runtime implementation; when an FG
        // engine overwrote the slot (the loop that created the recursion),
        // the resolved native runtime ECL remains type-safe.
        if (perQueueOriginal == EclBreakTargetClass::kNativeD3D12) {
            return EclBreakSelection::kPerQueueOriginal;
        }
        if (realD3D12Ecl == EclBreakTargetClass::kNativeD3D12) {
            return EclBreakSelection::kRealD3D12Ecl;
        }
        if (globalOriginal == EclBreakTargetClass::kNativeD3D12) {
            return EclBreakSelection::kGlobalOriginal;
        }
        if (perQueueOriginal == EclBreakTargetClass::kOtherModule) {
            return EclBreakSelection::kPerQueueOriginal;
        }
        if (globalOriginal == EclBreakTargetClass::kOtherModule) {
            return EclBreakSelection::kGlobalOriginal;
        }
        return EclBreakSelection::kNone;
    }

    if (queueVtable == EclBreakTargetClass::kForeignOverlayHook ||
        queueVtable == EclBreakTargetClass::kOtherModule) {
        // Proxy queue object: the native runtime ECL does not understand the
        // proxy layout. Only an original taken from this exact vtable is
        // type-safe.
        if (perQueueOriginal == EclBreakTargetClass::kForeignOverlayHook ||
            perQueueOriginal == EclBreakTargetClass::kOtherModule) {
            return EclBreakSelection::kPerQueueOriginal;
        }
        return EclBreakSelection::kNone;
    }

    return EclBreakSelection::kNone;
}

}  // namespace ce::dx12_overlay_policy
