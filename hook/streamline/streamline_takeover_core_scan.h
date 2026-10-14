#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "hook/runtime/graphics_runtime_module_policy.h"

namespace ce::streamline_interposer_takeover {

enum class CorePresence { Absent, Loaded, Unresolved };

// A cached NGX core has a hashed basename, so GetModuleHandle("sl.common.dll")
// cannot establish dormancy. Inspect every mapped path using the same semantic
// classifier as the runtime redirect policy, and fail closed on incomplete data.
// `modulePath(module)` returns the full UTF-8 path, or "" when it is unreadable;
// it has no MAX_PATH cap, so a deep install folder is still a complete inventory.
template <typename Enumerate, typename ModulePath>
CorePresence ScanLoadedStreamlineCore(Enumerate enumerate, ModulePath modulePath) {
    std::vector<HMODULE> modules;
    if (!enumerate(modules))
        return CorePresence::Unresolved;
    bool unresolved = false;
    for (HMODULE module : modules) {
        const std::string path = modulePath(module);
        if (path.empty()) {
            unresolved = true;
            continue;
        }
        char providedName[MAX_PATH] = {};
        if (ce::graphics_runtime::ResolveStreamlineProvidedDllName(path.c_str(), providedName, sizeof(providedName)) &&
            ce::graphics_runtime::IsStreamlineCoreProvidedDllName(providedName)) {
            return CorePresence::Loaded;
        }
    }
    return unresolved ? CorePresence::Unresolved : CorePresence::Absent;
}

inline constexpr size_t kSlInitTakeoverExportIndex = 0;
inline constexpr size_t kFirstNonSlTakeoverExportIndex = 27;

inline bool IsStreamlineCoreModulePath(const char* resolvedPath) {
    char providedName[MAX_PATH] = {};
    return ce::graphics_runtime::ResolveStreamlineProvidedDllName(resolvedPath, providedName, sizeof(providedName)) &&
           ce::graphics_runtime::IsStreamlineCoreProvidedDllName(providedName);
}

// Whether an early takeover thunk call for `exportIndex` may activate and load
// the configured sl.interposer.dll when takeover is armed but not yet active.
//
// In Streamline's interposer, only `slInit` (index 0) initializes the runtime
// and loads `sl.common.dll`; every other `sl*` export (indices 1..26:
// `slShutdown`, `slIsFeatureSupported`, `slGetFeatureFunction`, `slSetD3DDevice`,
// ...) requires `slInit` to have already succeeded and returns
// `sl::Result::eErrorNotInitialized` (21) on an uninitialized interposer.
// If a post-`slInit` `sl*` export is the FIRST thunk reached while takeover is
// still inactive, the application's main thread already entered the resident
// `sl.interposer.dll!slInit` before `TryArmTakeover` patched the IAT (Witcher 3
// session 20261011_113848). Activating a second custom interposer on that call
// would route all subsequent `sl*` calls into an uninitialized duplicate while
// the resident interposer owns the initialized `sl.common.dll` core.
inline constexpr bool IsTakeoverEntryExportIndex(size_t exportIndex) {
    return exportIndex == kSlInitTakeoverExportIndex || exportIndex >= kFirstNonSlTakeoverExportIndex;
}

inline constexpr bool CanActivateTakeoverForExport(size_t exportIndex, CorePresence corePresence) {
    return corePresence == CorePresence::Absent && IsTakeoverEntryExportIndex(exportIndex);
}

}  // namespace ce::streamline_interposer_takeover
