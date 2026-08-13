#pragma once

#include <cstddef>
#include <string>

#include "overlay_compat_detail/module_table.h"

// Decision logic for the optional ReShade / OptiScaler / Special K loads the
// injected hook performs on behalf of the user ([ThirdParty] config section).
//
// Pure and unit-testable: no loader calls, no globals, no logging. The
// executor (main_thirdparty_load.cpp) owns every Windows loader interaction.
namespace ce::third_party_load {

// Fixed load order: ReShade first, then OptiScaler, then Special K LAST.
//
// Both orderings deadlock unless the thread-creating tool loads before Special
// K's hooks exist AND Special K's own load waits for the other tools' startup
// loader work to finish:
//  - Special-K-first: OptiScaler's DllMain creates a thread through Special
//    K's thread-creation hook, which waits on a Special K critical section
//    held by its enumerator thread while it blocks on the loader lock CE
//    holds (sessions 20260813_020236 and 20260813_025615 — the quiescence
//    wait cannot fix this because the enumerator starts new loader cycles at
//    any time).
//  - Special-K-last: Special K's DllMain calls LoadLibrary, which re-enters
//    OptiScaler's mutex-guarded loader hook; without a quiescence wait an
//    OptiScaler background thread can hold that mutex while waiting for the
//    loader lock (session 20260813_021731). With
//    ShouldWaitForLoaderQuiescenceBeforeToolLoad draining OptiScaler's
//    startup loader work first, and its later loader activity being
//    startup-only, this order is the safe one.
// Do not reorder without re-checking both cycles; the executor iterates this
// enum in declaration order.
enum class Tool : int { kReShade = 0, kOptiScaler = 1, kSpecialK = 2, kCount = 3 };

inline constexpr size_t kToolCount = static_cast<size_t>(Tool::kCount);

inline const char* ToolName(Tool tool) {
    switch (tool) {
        case Tool::kSpecialK:
            return "SpecialK";
        case Tool::kReShade:
            return "ReShade";
        case Tool::kOptiScaler:
            return "OptiScaler";
        default:
            return "Unknown";
    }
}

inline const char* ToolConfigKey(Tool tool) {
    switch (tool) {
        case Tool::kSpecialK:
            return "specialk_dll_path";
        case Tool::kReShade:
            return "reshade_dll_path";
        case Tool::kOptiScaler:
            return "optiscaler_dll_path";
        default:
            return "";
    }
}

// The per-bitness default file name appended when a configured value is a
// directory. OptiScaler ships one DLL name for both architectures.
inline const char* DefaultDllBaseName(Tool tool, bool is64Bit) {
    switch (tool) {
        case Tool::kSpecialK:
            return is64Bit ? "SpecialK64.dll" : "SpecialK32.dll";
        case Tool::kReShade:
            return is64Bit ? "ReShade64.dll" : "ReShade32.dll";
        case Tool::kOptiScaler:
            return "OptiScaler.dll";
        default:
            return "";
    }
}

struct ToolKnownNames {
    const char* names[3];
    size_t count;
};

// Canonical base names that identify `tool` when already loaded, including the
// generic names these DLLs are sometimes renamed to. Used for the
// skip-if-already-loaded decision and the duplicate-load regression tests.
inline ToolKnownNames KnownBaseNamesForTool(Tool tool) {
    switch (tool) {
        case Tool::kSpecialK:
            return {{"SpecialK64.dll", "SpecialK32.dll", "SpecialK.dll"}, 3};
        case Tool::kReShade:
            return {{"ReShade64.dll", "ReShade32.dll", "ReShade.dll"}, 3};
        case Tool::kOptiScaler:
            return {{"OptiScaler.dll", "OptiScaler.asi"}, 2};
        default:
            return {{}, 0};
    }
}

inline bool IsKnownModuleBaseNameForTool(Tool tool, const char* baseName) {
    if (!baseName || !*baseName) {
        return false;
    }
    const ToolKnownNames known = KnownBaseNamesForTool(tool);
    for (size_t i = 0; i < known.count; ++i) {
        if (ce::overlay_compat::detail::EqualsInsensitive(baseName, known.names[i])) {
            return true;
        }
    }
    return false;
}

inline bool HasAnyThirdPartyLoadConfigured(const char* specialkPath, const char* reshadePath,
                                           const char* optiscalerPath) {
    return (specialkPath && *specialkPath) || (reshadePath && *reshadePath) ||
           (optiscalerPath && *optiscalerPath);
}

// The first tool has no predecessor whose background loader work can collide
// with its DllMain. Every later load must first wait for the loader work queue
// to drain (main_thirdparty_load.cpp waits for a trivial LoadLibrary probe):
// starting the next tool's DllMain while the previous tool's init threads are
// mid-loader-call re-creates the loader-lock deadlocks from sessions
// 20260813_020236 / 20260813_021731.
inline bool ShouldWaitForLoaderQuiescenceBeforeToolLoad(size_t toolIndex) {
    return toolIndex > 0;
}

// Resolves the configured value for one tool and process bitness.
//  - empty input -> empty result (tool disabled)
//  - a value whose final path component contains a '.' is treated as a file
//    path and returned verbatim
//  - anything else is treated as a directory and the per-bitness default base
//    name is appended
inline std::string ResolveThirdPartyDllPath(Tool tool, const std::string& configuredPath, bool is64Bit) {
    if (configuredPath.empty()) {
        return "";
    }
    const size_t lastSeparator = configuredPath.find_last_of("\\/");
    const size_t lastDot = configuredPath.find_last_of('.');
    const bool hasExtension = lastDot != std::string::npos &&
                              (lastSeparator == std::string::npos || lastDot > lastSeparator);
    if (hasExtension) {
        return configuredPath;
    }
    std::string resolved = configuredPath;
    if (resolved.back() != '\\' && resolved.back() != '/') {
        resolved += '\\';
    }
    resolved += DefaultDllBaseName(tool, is64Bit);
    return resolved;
}

}  // namespace ce::third_party_load
