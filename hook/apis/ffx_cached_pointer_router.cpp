#include "ffx_cached_pointer_router.h"

#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/overlay_compat.h"

namespace ce::ffx_cached_pointer_router {
namespace {

struct TrackedPatch {
    void** slot = nullptr;
    void* original = nullptr;
    void* replacement = nullptr;
    HMODULE ownerModule = nullptr;
    HMODULE sourceModule = nullptr;
};

std::mutex g_RouterMutex;
std::unordered_map<void**, TrackedPatch> g_TrackedPatches;

bool PinExpectedModuleFromAddress(const void* address, HMODULE expectedModule, HMODULE* pinnedModuleOut) {
    if (!address || !expectedModule || !pinnedModuleOut) {
        return false;
    }
    *pinnedModuleOut = nullptr;
    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(address), &pinnedModule)) {
        return false;
    }
    if (pinnedModule != expectedModule) {
        FreeLibrary(pinnedModule);
        return false;
    }
    *pinnedModuleOut = pinnedModule;
    return true;
}

bool IsWritableProtection(DWORD protection) {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xffu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

std::string LowercasePath(HMODULE module) {
    char path[MAX_PATH] = {};
    if (!module || !GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)))) {
        return {};
    }
    std::string result(path);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool ShouldScanClientModule(HMODULE module, HMODULE sourceModule, HMODULE mainModule) {
    if (!module || module == sourceModule) {
        return false;
    }

    const std::string path = LowercasePath(module);
    if (path.empty()) {
        return module == mainModule;
    }
    if (path.find("\\system32\\") != std::string::npos || path.find("\\syswow64\\") != std::string::npos ||
        path.find("capture_hook") != std::string::npos || path.find("d3d12_wrappers") != std::string::npos ||
        overlay_compat::IsThirdPartyOverlayModulePath(path.c_str()) ||
        overlay_compat::IsStreamlineFrameGenerationModulePath(path.c_str()) ||
        overlay_compat::IsFFXFrameGenerationModulePath(path.c_str())) {
        return false;
    }
    return true;
}

bool GetImageLayout(HMODULE module, MODULEINFO* moduleInfoOut, IMAGE_NT_HEADERS** ntHeadersOut,
                    IMAGE_SECTION_HEADER** sectionsOut) {
    if (!module || !moduleInfoOut || !ntHeadersOut || !sectionsOut ||
        !GetModuleInformation(GetCurrentProcess(), module, moduleInfoOut, sizeof(*moduleInfoOut)) ||
        moduleInfoOut->lpBaseOfDll != module || moduleInfoOut->SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }

    auto* base = static_cast<std::uint8_t*>(moduleInfoOut->lpBaseOfDll);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) > moduleInfoOut->SizeOfImage - sizeof(IMAGE_NT_HEADERS)) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.NumberOfSections == 0) {
        return false;
    }
    auto* sections = IMAGE_FIRST_SECTION(nt);
    const auto sectionEnd = reinterpret_cast<std::uintptr_t>(sections + nt->FileHeader.NumberOfSections);
    const auto imageEnd = reinterpret_cast<std::uintptr_t>(base) + moduleInfoOut->SizeOfImage;
    if (sectionEnd > imageEnd) {
        return false;
    }

    *ntHeadersOut = nt;
    *sectionsOut = sections;
    return true;
}

size_t RouteWritableRange(std::uint8_t* begin, std::uint8_t* end, HMODULE ownerModule, HMODULE sourceModule,
                          const Route* routes, size_t routeCount, std::uint64_t* patchedRouteMask) {
    if (!begin || !end || begin >= end) {
        return 0;
    }

    size_t patched = 0;
    std::uintptr_t cursorValue = reinterpret_cast<std::uintptr_t>(begin);
    cursorValue = (cursorValue + alignof(void*) - 1) & ~(static_cast<std::uintptr_t>(alignof(void*) - 1));
    for (auto* cursor = reinterpret_cast<std::uint8_t*>(cursorValue); cursor + sizeof(void*) <= end;) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(cursor, &memory, sizeof(memory))) {
            break;
        }
        auto* regionEnd = static_cast<std::uint8_t*>(memory.BaseAddress) + memory.RegionSize;
        auto* scanEnd = (std::min)(end, regionEnd);
        if (memory.State == MEM_COMMIT && IsWritableProtection(memory.Protect)) {
            for (; cursor + sizeof(void*) <= scanEnd; cursor += sizeof(void*)) {
                auto** slot = reinterpret_cast<void**>(cursor);
                void* current =
                    InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), nullptr, nullptr);
                const int routeIndex = detail::FindMatchingRoute(current, routes, routeCount);
                if (routeIndex < 0) {
                    continue;
                }
                const Route& route = routes[static_cast<size_t>(routeIndex)];
                void* observed = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot),
                                                                   route.replacement, route.original);
                if (observed != route.original) {
                    continue;
                }
                g_TrackedPatches[slot] = {slot, route.original, route.replacement, ownerModule, sourceModule};
                if (patchedRouteMask && static_cast<size_t>(routeIndex) < 64) {
                    *patchedRouteMask |= (std::uint64_t{1} << static_cast<size_t>(routeIndex));
                }
                ++patched;
            }
        }
        if (cursor < scanEnd) {
            cursor = scanEnd;
        }
        const auto aligned = (reinterpret_cast<std::uintptr_t>(cursor) + alignof(void*) - 1) &
                             ~(static_cast<std::uintptr_t>(alignof(void*) - 1));
        cursor = reinterpret_cast<std::uint8_t*>(aligned);
    }
    return patched;
}

}  // namespace

RefreshResult Refresh(HMODULE sourceModule, const Route* routes, size_t routeCount) {
    RefreshResult result = {};
    if (!sourceModule || !routes || routeCount == 0) {
        return result;
    }

    std::lock_guard<std::mutex> lock(g_RouterMutex);

    // A runtime reload can change the original export while a client-owned slot remains permanently routed to
    // the same CE replacement. Preserve that durable-route proof instead of needlessly re-enabling the protected
    // entry breakpoint just because there was no new original->replacement exchange during this refresh.
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - route-mask aggregation is order-independent
    for (const auto& entry : g_TrackedPatches) {
        const TrackedPatch& patch = entry.second;
        HMODULE pinnedOwner = nullptr;
        if (!PinExpectedModuleFromAddress(reinterpret_cast<const void*>(patch.slot), patch.ownerModule, &pinnedOwner)) {
            continue;
        }
        void* current =
            InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(patch.slot), nullptr, nullptr);
        FreeLibrary(pinnedOwner);
        if (current != patch.replacement) {
            continue;
        }
        for (size_t i = 0; i < routeCount && i < 64; ++i) {
            if (routes[i].replacement == patch.replacement) {
                result.routedRouteMask |= (std::uint64_t{1} << i);
            }
        }
    }

    std::vector<HMODULE> modules(256);
    DWORD bytesNeeded = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                            &bytesNeeded)) {
        return result;
    }
    if (bytesNeeded > modules.size() * sizeof(HMODULE)) {
        modules.resize((bytesNeeded + sizeof(HMODULE) - 1) / sizeof(HMODULE));
        if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                                static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytesNeeded)) {
            return result;
        }
    }
    modules.resize((std::min)(modules.size(), static_cast<size_t>(bytesNeeded / sizeof(HMODULE))));

    const HMODULE mainModule = GetModuleHandleW(nullptr);
    for (HMODULE module : modules) {
        if (!ShouldScanClientModule(module, sourceModule, mainModule)) {
            continue;
        }

        HMODULE pinnedModule = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module),
                                &pinnedModule) ||
            pinnedModule != module) {
            continue;
        }

        MODULEINFO moduleInfo = {};
        IMAGE_NT_HEADERS* nt = nullptr;
        IMAGE_SECTION_HEADER* sections = nullptr;
        if (GetImageLayout(module, &moduleInfo, &nt, &sections)) {
            ++result.modulesScanned;
            auto* imageBase = static_cast<std::uint8_t*>(moduleInfo.lpBaseOfDll);
            auto* imageEnd = imageBase + moduleInfo.SizeOfImage;
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                const IMAGE_SECTION_HEADER& section = sections[i];
                if (!detail::IsWritableNonExecutableSection(section.Characteristics) ||
                    section.VirtualAddress >= moduleInfo.SizeOfImage) {
                    continue;
                }
                const size_t requestedSize =
                    section.Misc.VirtualSize ? section.Misc.VirtualSize : section.SizeOfRawData;
                auto* sectionBegin = imageBase + section.VirtualAddress;
                auto* sectionEnd =
                    sectionBegin + (std::min)(requestedSize, static_cast<size_t>(imageEnd - sectionBegin));
                ++result.writableSectionsScanned;
                result.pointerSlotsPatched += RouteWritableRange(sectionBegin, sectionEnd, module, sourceModule, routes,
                                                                 routeCount, &result.routedRouteMask);
            }
        }
        FreeLibrary(pinnedModule);
    }
    return result;
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_RouterMutex);
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - each patch is restored independently
    for (const auto& entry : g_TrackedPatches) {
        const TrackedPatch& patch = entry.second;
        HMODULE pinnedOwner = nullptr;
        if (!PinExpectedModuleFromAddress(reinterpret_cast<const void*>(patch.slot), patch.ownerModule, &pinnedOwner)) {
            continue;
        }
        HMODULE pinnedSource = nullptr;
        if (!PinExpectedModuleFromAddress(patch.original, patch.sourceModule, &pinnedSource)) {
            FreeLibrary(pinnedOwner);
            continue;
        }
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(patch.slot), patch.original,
                                          patch.replacement);
        FreeLibrary(pinnedSource);
        FreeLibrary(pinnedOwner);
    }
    g_TrackedPatches.clear();
}

}  // namespace ce::ffx_cached_pointer_router
