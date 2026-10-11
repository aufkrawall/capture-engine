#pragma once

#include <vector>
#include <windows.h>

#include "hook/runtime/graphics_runtime_module_policy.h"

namespace ce::streamline_interposer_takeover {

enum class CorePresence { Absent, Loaded, Unresolved };

// A cached NGX core has a hashed basename, so GetModuleHandle("sl.common.dll")
// cannot establish dormancy. Inspect every mapped path using the same semantic
// classifier as the runtime redirect policy, and fail closed on incomplete data.
template <typename Enumerate, typename ModulePath>
CorePresence ScanLoadedStreamlineCore(Enumerate enumerate, ModulePath modulePath) {
    std::vector<HMODULE> modules;
    if (!enumerate(modules))
        return CorePresence::Unresolved;
    bool unresolved = false;
    for (HMODULE module : modules) {
        char path[MAX_PATH] = {};
        const DWORD length = modulePath(module, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            unresolved = true;
            continue;
        }
        char providedName[MAX_PATH] = {};
        if (ce::graphics_runtime::ResolveStreamlineProvidedDllName(path, providedName, sizeof(providedName)) &&
            ce::graphics_runtime::IsStreamlineCoreProvidedDllName(providedName)) {
            return CorePresence::Loaded;
        }
    }
    return unresolved ? CorePresence::Unresolved : CorePresence::Absent;
}

}  // namespace ce::streamline_interposer_takeover
