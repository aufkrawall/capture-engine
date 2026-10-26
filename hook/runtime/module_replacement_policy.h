#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hook/hooking/iat_import_table.h"
#include "hook/overlay/overlay_compat_detail/module_table.h"

// Decisions for a [ThirdParty] custom_dll_paths entry that is named like a module the process
// has already resolved from another file.
//
// GTA V Enhanced maps System32's dxgi.dll and d3d12.dll before CE's DllMain (session
// 20261026_145243). Loading vkd3d-proton's copies by full path then only added second images
// of the same names: every by-name resolution - import binding, LoadLibrary("d3d12.dll"),
// GetModuleHandle - keeps answering the first image, so the game ran on native D3D12. Such an
// entry therefore *replaces* the resident module for the application, as if it had been placed
// beside the executable. The executor (module_replacement.cpp) owns every loader interaction;
// this header is pure and unit-testable: no loader calls, no globals, no logging.
namespace ce::module_replacement {

// Who is asking for a replaced module. Application code is moved: Windows' own modules keep the
// runtime they were built against (System32's d3d11.dll needs System32's dxgi.dll internals, and
// DriverStore modules are the driver), and CE's own modules keep their deliberate native paths
// (see RequesterScope). A caller that resolves to no module (generated code) counts as
// application code: the process's own trampolines are far more common than Windows'.
enum class Requester : unsigned char { kApplication, kSystem, kCaptureEngine };

template <typename CharT>
inline const CharT* PathBaseName(const CharT* path) {
    if (!path) {
        return nullptr;
    }
    const CharT* base = path;
    for (const CharT* cursor = path; *cursor; ++cursor) {
        if (*cursor == static_cast<CharT>('\\') || *cursor == static_cast<CharT>('/') ||
            *cursor == static_cast<CharT>(':')) {
            base = cursor + 1;
        }
    }
    return base;
}

template <typename CharT>
inline bool StartsWithInsensitive(const CharT* text, const char* prefix) {
    if (!text || !prefix) {
        return false;
    }
    for (; *prefix; ++text, ++prefix) {
        if (!*text || ce::overlay_compat::detail::ToLowerAscii(*text) !=
                          static_cast<CharT>(ce::overlay_compat::detail::ToLowerAscii(*prefix))) {
            return false;
        }
    }
    return true;
}

template <typename CharT>
inline bool PathHasDirectoryInsensitive(const CharT* path, const char* directory) {
    if (!path) {
        return false;
    }
    const auto isSeparator = [](CharT ch) {
        return ch == static_cast<CharT>('\\') || ch == static_cast<CharT>('/');
    };
    for (const CharT* cursor = path; *cursor; ++cursor) {
        if (!isSeparator(*cursor) || !StartsWithInsensitive(cursor + 1, directory)) {
            continue;
        }
        size_t directoryLength = 0;
        while (directory[directoryLength]) {
            ++directoryLength;
        }
        if (isSeparator(cursor[1 + directoryLength])) {
            return true;
        }
    }
    return false;
}

template <typename CharT>
inline Requester ClassifyRequesterPath(const CharT* path) {
    if (!path || !*path) {
        return Requester::kApplication;
    }
    const CharT* base = PathBaseName(path);
    if (StartsWithInsensitive(base, "capture_hook") || StartsWithInsensitive(base, "d3d12_wrappers") ||
        StartsWithInsensitive(base, "vk_layer_ce")) {
        return Requester::kCaptureEngine;
    }
    if (PathHasDirectoryInsensitive(path, "system32") || PathHasDirectoryInsensitive(path, "syswow64") ||
        PathHasDirectoryInsensitive(path, "winsxs")) {
        return Requester::kSystem;
    }
    return Requester::kApplication;
}

// How much of a replacement a requester sees.
//  kNone: Windows' modules.
//  kBareNames: resolves names the way the application does, but a request for the superseded
//    image's own path or handle keeps it. A configured custom DLL: that request is how a proxy
//    reaches the module it wraps (ReShade as dxgi.dll loads System32's dxgi.dll by full path),
//    and handing it the replacement - possibly itself - would recurse. CE's modules: their
//    identity checks must see what the game runs (the Vulkan layer picks its DXVK interop by
//    asking for "d3d11.dll"), while their explicit System32 loads are the native interop.
//  kFull: other application code also has the superseded image's own path and handle moved.
//    Streamline's interposer loads "<system directory>\dxgi.dll" and "...\d3d12.dll" by full
//    path, which would otherwise pair System32's DXGI with vkd3d-proton's D3D12 device.
enum class RequesterScope : unsigned char { kNone, kBareNames, kFull };

inline RequesterScope ScopeFor(Requester requester, bool isConfiguredCustomDll, bool isNativeRuntime = false) {
    if (isNativeRuntime && !isConfiguredCustomDll)
        return RequesterScope::kNone;
    switch (requester) {
        case Requester::kSystem:
            return RequesterScope::kNone;
        case Requester::kCaptureEngine:
            return RequesterScope::kBareNames;
        default:
            return isConfiguredCustomDll ? RequesterScope::kBareNames : RequesterScope::kFull;
    }
}

// Whether a call that reached one of the superseded image's own entry points continues in the
// replacement. A game whose protected import code resolved System32's D3D12CreateDevice before CE
// was injected calls that address for the rest of its life, so the entry point itself forwards
// for callers whose explicit native handle would also be redirected. A configured proxy calls
// the image it wraps there; CE and Windows (the driver's DXGI-based Vulkan WSI) need the native runtime.
inline bool ShouldForwardEntryCall(RequesterScope scope) {
    return scope == RequesterScope::kFull;
}

// A request names a module by name only (no directory, no drive): the loader then resolves it
// against the modules already loaded first, which is the resolution a replacement takes over.
template <typename CharT>
inline bool IsBareModuleName(const CharT* request) {
    return request && *request && PathBaseName(request) == request;
}

// Whether `request` (a bare name) names `baseName`. LoadLibrary("dxgi") means dxgi.dll: a name
// without any '.' gets the default extension, and a trailing '.' means "no extension".
template <typename CharT>
inline bool BareNameMatchesBaseName(const CharT* request, const char* baseName) {
    if (!IsBareModuleName(request) || !baseName || !*baseName) {
        return false;
    }
    size_t length = 0;
    while (request[length]) {
        ++length;
    }
    const bool trailingDot = request[length - 1] == static_cast<CharT>('.');
    if (trailingDot) {
        --length;
    }
    bool hasDot = false;
    for (size_t i = 0; i < length; ++i) {
        if (!baseName[i] || ce::overlay_compat::detail::ToLowerAscii(request[i]) !=
                                static_cast<CharT>(ce::overlay_compat::detail::ToLowerAscii(baseName[i]))) {
            return false;
        }
        hasDot = hasDot || request[i] == static_cast<CharT>('.');
    }
    if (!baseName[length]) {
        return true;
    }
    return !hasDot && !trailingDot && StartsWithInsensitive(baseName + length, ".dll") &&
           baseName[length + 4] == '\0';
}

// Private parts of a runtime that ship beside an application and are loaded only by that
// runtime: the D3D12 Agility SDK's D3D12Core.dll and its debug layer belong to System32's
// d3d12.dll. While d3d12.dll is replaced they still serve CE's own native D3D12 use, so they
// must keep the system dxgi.dll even though they live in the game folder.
template <typename CharT>
inline bool IsRuntimeComponentOf(const char* replacedBaseName, const CharT* moduleBaseName) {
    using ce::overlay_compat::detail::EqualsInsensitive;
    if (!replacedBaseName || !moduleBaseName) {
        return false;
    }
    if (EqualsInsensitive(replacedBaseName, "d3d12.dll")) {
        return BareNameMatchesBaseName(moduleBaseName, "d3d12core.dll") ||
               BareNameMatchesBaseName(moduleBaseName, "d3d12sdklayers.dll");
    }
    return false;
}

struct ImporterFacts {
    Requester requester = Requester::kApplication;
    bool isSuperseded = false;            // one of the images a replacement stands in for
    bool isConfiguredCustomDll = false;   // one of the custom_dll_paths images
    bool belongsToReplacedRuntime = false;  // named like a replaced module or one of its components
};

// Whether a module's imports of a replaced name move to the replacement. A configured custom
// DLL always does (vkd3d-proton's d3d12core.dll imports dxgi.dll and must reach DXVK's), even
// when it is named like a runtime component itself.
inline bool ShouldRebindImporter(const ImporterFacts& facts) {
    if (facts.requester != Requester::kApplication || facts.isSuperseded) {
        return false;
    }
    return facts.isConfiguredCustomDll || !facts.belongsToReplacedRuntime;
}

struct ImageRange {
    uintptr_t base = 0;
    size_t size = 0;
    bool Contains(uintptr_t address) const { return base && address >= base && address - base < size; }
};

struct SlotChange {
    void** entry = nullptr;
    void* expected = nullptr;
    void* replacement = nullptr;
};

struct ImportRebindPlan {
    std::vector<SlotChange> changes;
    size_t alreadyReplaced = 0;  // slot already holds the replacement's export
    size_t notYetBound = 0;      // the loader has not resolved the slot yet
    size_t keptForeign = 0;      // a hook (CE's or another tool's) owns the slot; its target is left alone
    size_t missingExport = 0;    // the replacement does not export the name: the slot stays native
    size_t unnamed = 0;          // ordinal or name-less import of a superseded export: stays native
    const char* firstMissing = nullptr;
    IATHook::detail::ImportTableIssue issue = IATHook::detail::ImportTableIssue::None;
};

// Plans moving every import slot `imageBase` takes from `importName` that still points into
// `superseded` to the export of the same name `resolve(name)` returns. Nothing is written;
// the caller commits `changes` as one transaction.
template <typename ResolveExport>
inline ImportRebindPlan PlanImportRebind(BYTE* imageBase, size_t imageSize, DWORD importDirectoryRva,
                                         DWORD importDirectorySize, const char* importName, ImageRange superseded,
                                         ImageRange replacement, ResolveExport resolve,
                                         IATHook::detail::ImportMemoryReadable memoryReadable) {
    ImportRebindPlan plan;
    plan.issue = IATHook::detail::ForEachImportOf(
        imageBase, imageSize, importDirectoryRva, importDirectorySize, importName,
        [&](IATHook::detail::ImportNameKind kind, const char* name, IMAGE_THUNK_DATA* entry) {
            const ULONG_PTR current = entry->u1.Function;
            if (replacement.Contains(current)) {
                ++plan.alreadyReplaced;
                return;
            }
            if ((current & IMAGE_ORDINAL_FLAG) != 0 || current < imageSize) {
                ++plan.notYetBound;
                return;
            }
            if (!superseded.Contains(current)) {
                ++plan.keptForeign;
                return;
            }
            if (kind != IATHook::detail::ImportNameKind::Named) {
                ++plan.unnamed;
                return;
            }
            void* target = resolve(name);
            if (!target) {
                if (plan.missingExport++ == 0) {
                    plan.firstMissing = name;
                }
                return;
            }
            plan.changes.push_back({reinterpret_cast<void**>(&entry->u1.Function),
                                    reinterpret_cast<void*>(current), target});
        },
        memoryReadable);
    return plan;
}

}  // namespace ce::module_replacement
