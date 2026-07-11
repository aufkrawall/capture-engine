#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace ce::ffx_cached_pointer_router {

struct Route {
    const char* exportName = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
};

struct RefreshResult {
    size_t modulesScanned = 0;
    size_t writableSectionsScanned = 0;
    size_t pointerSlotsPatched = 0;
    // Includes newly patched slots and previously tracked slots which still contain the CE replacement.
    std::uint64_t routedRouteMask = 0;
};

namespace detail {

inline int FindMatchingRoute(void* value, const Route* routes, size_t routeCount) {
    if (!value || !routes) {
        return -1;
    }
    for (size_t i = 0; i < routeCount; ++i) {
        if (routes[i].original && routes[i].replacement && routes[i].original != routes[i].replacement &&
            value == routes[i].original) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline bool IsWritableNonExecutableSection(DWORD characteristics) {
    return (characteristics & IMAGE_SCN_MEM_WRITE) != 0 && (characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 &&
           (characteristics & IMAGE_SCN_MEM_DISCARDABLE) == 0;
}

}  // namespace detail

// Routes FFX export addresses which a client module cached before CE installed its GetProcAddress/IAT hooks.
// Only pointer-aligned slots in writable, non-executable PE sections are considered; AMD's runtime image,
// system DLLs, overlay DLLs, Streamline, and CE modules are excluded. The official runtime code is untouched.
RefreshResult Refresh(HMODULE sourceModule, const Route* routes, size_t routeCount);

// Restores still-live routed slots when CE detaches while the client and source module remain loaded.
void Shutdown();

}  // namespace ce::ffx_cached_pointer_router
