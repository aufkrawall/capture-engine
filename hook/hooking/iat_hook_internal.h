/**
 * IAT/EAT hook engine — shared registries (internal)
 *
 * iat_hook.cpp owns the patching primitives and the tracking tables;
 * iat_hook_init.cpp drives the per-API installation on top of them. The tables
 * are shared, so they are declared here rather than duplicated.
 */

#pragma once

#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "iat_hook.h"

namespace IATHook {

// Track patched entries for restoration. Defined in iat_hook.cpp.
struct PatchedEntry {
    HMODULE targetModule;
    std::string sourceModule;
    std::string functionName;
    void* hookFunction;
    void* originalFunction;
    // void* originalFunction; // Duplicate removed
    void** iatEntry;
};

// Dynamic hook registry
struct DynamicHookEntry {
    void* hookFunction;
    void** outOriginal;
    DynamicHookModuleFilter moduleFilter;
};

extern std::mutex g_DynamicHookLock;
extern std::unordered_map<std::string, DynamicHookEntry> g_DynamicHooks;

extern std::mutex g_PatchLock;
extern std::vector<PatchedEntry> g_PatchedEntries;

}  // namespace IATHook
