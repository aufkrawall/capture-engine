/**
 * IAT/EAT patching primitives
 *
 * Import- and export-table walking and patching, plus the tables that track
 * what has been patched so it can be restored. The per-API installation that
 * uses these lives in iat_hook_init.cpp.
 */

#include "iat_hook.h"
#include <d3d12.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../apis/dx11_hook.h"
#include "../apis/dx12_sampler_hooks.h"
#include "../apis/lod_helper.h"
#include "../common/module_enumeration.h"
#include "../common/overlay_compat.h"
#include "../common/sampler_override_utils.h"
#include "hook_common.h"
#include "iat_import_table.h"
#include "wrapper_hooks.h"
#include "iat_hook_internal.h"

namespace IATHook {

// Registries declared in iat_hook_internal.h.
std::mutex g_DynamicHookLock;
std::unordered_map<std::string, DynamicHookEntry> g_DynamicHooks;
std::mutex g_PatchLock;
std::vector<PatchedEntry> g_PatchedEntries;

static bool TryGetTrackedOriginalForPatchedEntry(HMODULE targetModule, const char* sourceModule,
                                                 const char* functionName, void* hookFunction, void** iatEntry,
                                                 void** outOriginal) {
    if (!targetModule || !sourceModule || !functionName || !hookFunction || !iatEntry) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_PatchLock);
    for (const auto& entry : g_PatchedEntries) {
        if (entry.targetModule == targetModule && _stricmp(entry.sourceModule.c_str(), sourceModule) == 0 &&
            entry.functionName == functionName && entry.hookFunction == hookFunction && entry.iatEntry == iatEntry) {
            if (outOriginal && entry.originalFunction) {
                *outOriginal = entry.originalFunction;
            }
            return true;
        }
    }
    return false;
}

// ============================================================================
// Module Validation
// ============================================================================

// Validates that a module handle is still valid and loaded
// This prevents crashes when a module is unloaded between EnumProcessModules
// and the actual memory access (race condition).
static bool IsModuleValid(HMODULE hModule) {
    if (!hModule)
        return false;

    // Use GetModuleInformation to verify the module is still loaded
    // This is safer than directly accessing the DOS header
    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) {
        return false;
    }

    // Additional check: verify the module base matches
    return (modInfo.lpBaseOfDll == hModule);
}

// Check if memory is readable using VirtualQuery
static bool IsMemoryReadable(const void* ptr, size_t size) {
    if (!ptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    // Check if memory is committed and readable
    if (mbi.State != MEM_COMMIT) {
        return false;
    }

    // Check protection flags - allow read, read-write, execute-read, etc.
    const DWORD protect = mbi.Protect;
    const DWORD access = protect & 0xFF;
    if ((protect & PAGE_GUARD) || access == PAGE_NOACCESS || access == PAGE_EXECUTE) {
        return false;
    }

    // Check if the entire range is within this region
    const uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (address < regionBase) {
        return false;
    }
    const SIZE_T offset = address - regionBase;
    return offset <= mbi.RegionSize && size <= mbi.RegionSize - offset;
}

// A caller-owned IAT replacement cannot be chained correctly from a process-
// wide detour because the detour has no call-site identity. Preserve that slot
// instead of stealing it: CE can still attach at the export body or graphics
// object vtable, while the foreign injector keeps its exact predecessor.
static bool IsForeignIATOwner(void* currentFunction, const char* sourceModule, const char* functionName) {
    HMODULE source = GetModuleHandleA(sourceModule);
    void* expectedFunction = source ? reinterpret_cast<void*>(GetProcAddress(source, functionName)) : nullptr;
    if (!currentFunction || !expectedFunction || currentFunction == expectedFunction)
        return false;

    HMODULE expectedOwner = nullptr;
    HMODULE currentOwner = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(expectedFunction), &expectedOwner))
        return false;
    if (!GetModuleHandleExA(flags, reinterpret_cast<LPCSTR>(currentFunction), &currentOwner))
        return true;
    return currentOwner != expectedOwner;
}

// ============================================================================
// PE Parsing Helpers
// ============================================================================

struct ModuleImportView {
    BYTE* imageBase = nullptr;
    size_t imageSize = 0;
    DWORD directoryRva = 0;
    DWORD directorySize = 0;
};

static bool GetModuleImportView(HMODULE module, ModuleImportView* view) {
    if (!view)
        return false;

    // Validate module is still loaded before accessing memory
    if (!IsModuleValid(module))
        return false;

    MODULEINFO moduleInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), module, &moduleInfo, sizeof(moduleInfo)) ||
        moduleInfo.lpBaseOfDll != module || moduleInfo.SizeOfImage == 0)
        return false;

    auto* imageBase = reinterpret_cast<BYTE*>(module);
    const size_t imageSize = moduleInfo.SizeOfImage;

    // Check if DOS header memory is readable
    if (!detail::ImageRangeContains(imageSize, 0, sizeof(IMAGE_DOS_HEADER)) ||
        !IsMemoryReadable(module, sizeof(IMAGE_DOS_HEADER)))
        return false;

    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE || dosHeader->e_lfanew < 0 ||
        !detail::ImageRangeContains(imageSize, static_cast<uintptr_t>(dosHeader->e_lfanew),
                                    sizeof(IMAGE_NT_HEADERS)))
        return false;

    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(imageBase + dosHeader->e_lfanew);
    if (!IsMemoryReadable(ntHeaders, sizeof(*ntHeaders)) || ntHeaders->Signature != IMAGE_NT_SIGNATURE ||
        ntHeaders->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER) ||
        ntHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        ntHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return false;

    auto importDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir->VirtualAddress == 0 || importDir->Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !detail::ImageRangeContains(imageSize, importDir->VirtualAddress, importDir->Size))
        return false;

    *view = {imageBase, imageSize, importDir->VirtualAddress, importDir->Size};
    return true;
}

static IMAGE_EXPORT_DIRECTORY* GetExportDirectory(HMODULE module, DWORD* exportSize = nullptr) {
    // Validate module is still loaded before accessing memory
    if (!IsModuleValid(module))
        return nullptr;

    // Check if DOS header memory is readable
    if (!IsMemoryReadable(module, sizeof(IMAGE_DOS_HEADER)))
        return nullptr;

    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    auto exportDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exportDir->VirtualAddress == 0)
        return nullptr;

    if (exportSize)
        *exportSize = exportDir->Size;

    return reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(reinterpret_cast<BYTE*>(module) + exportDir->VirtualAddress);
}

// ============================================================================
// Core IAT Patching
// ============================================================================

struct TrackedImportContext {
    HMODULE targetModule = nullptr;
    const char* sourceModule = nullptr;
    const char* functionName = nullptr;
    void* hookFunction = nullptr;
};

static bool LookupTrackedImport(void* opaqueContext, void** iatEntry, void** originalFunction) {
    const auto* context = static_cast<const TrackedImportContext*>(opaqueContext);
    return context && TryGetTrackedOriginalForPatchedEntry(context->targetModule, context->sourceModule,
                                                            context->functionName, context->hookFunction, iatEntry,
                                                            originalFunction);
}

bool PatchIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* hookFunction,
              void** outOriginal) {
    if (!targetModule)
        targetModule = GetModuleHandle(nullptr);
    if (!sourceModule || !functionName || !hookFunction)
        return false;

    ModuleImportView importView;
    if (!GetModuleImportView(targetModule, &importView))
        return false;

    HMODULE source = GetModuleHandleA(sourceModule);
    void* expectedFunction = source ? reinterpret_cast<void*>(GetProcAddress(source, functionName)) : nullptr;
    TrackedImportContext trackingContext{targetModule, sourceModule, functionName, hookFunction};
    const detail::ImportEntryLookup lookup = detail::FindImportTableEntry(
        importView.imageBase, importView.imageSize, importView.directoryRva, importView.directorySize, sourceModule,
        functionName, expectedFunction, hookFunction, &IsMemoryReadable, &LookupTrackedImport, &trackingContext);
    if (lookup.issue != detail::ImportTableIssue::None) {
        static std::atomic<uint32_t> malformedImportLogs{0};
        const uint32_t logIndex = malformedImportLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 16 || (logIndex % 512) == 0) {
            WrapperLog("IAT: Skipping malformed import table in module %p while finding %s!%s (reason=%s)",
                       targetModule, sourceModule, functionName, detail::ImportTableIssueName(lookup.issue));
        }
        return false;
    }
    if (!lookup.iatEntry)
        return false;

    auto* iatEntry = lookup.iatEntry;
    const auto currentFunction = reinterpret_cast<void*>(static_cast<uintptr_t>(iatEntry->u1.Function));
    if (currentFunction == hookFunction) {
        void* originalFunction = lookup.trackedOriginal;
        if (originalFunction ||
            TryGetTrackedOriginalForPatchedEntry(targetModule, sourceModule, functionName, hookFunction,
                                                 reinterpret_cast<void**>(&iatEntry->u1.Function),
                                                 &originalFunction)) {
            if (outOriginal && originalFunction)
                *outOriginal = originalFunction;
            static std::atomic<uint32_t> alreadyPatchedLogs{0};
            const uint32_t logIndex = alreadyPatchedLogs.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 16 || (logIndex % 1000) == 0) {
                WrapperLog("IAT: %s!%s in module %p already patched", sourceModule, functionName, targetModule);
            }
            return true;
        }
        WrapperLog("IAT: %s!%s in module %p already points at hook but original is not tracked", sourceModule,
                   functionName, targetModule);
        return false;
    }
    if (IsForeignIATOwner(currentFunction, sourceModule, functionName)) {
        static std::atomic<uint32_t> preservedOwnerLogs{0};
        const uint32_t logIndex = preservedOwnerLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 64 || (logIndex % 512) == 0) {
            WrapperLog(
                "IAT: Preserving foreign owner %p for %s!%s in module %p; CE will attach "
                "through export/vtable routes",
                currentFunction, sourceModule, functionName, targetModule);
        }
        return false;
    }

    PatchedEntry trackingEntry{targetModule, {}, {}, hookFunction, currentFunction,
                               reinterpret_cast<void**>(&iatEntry->u1.Function)};
    try {
        trackingEntry.sourceModule = sourceModule;
        trackingEntry.functionName = functionName;
    } catch (...) {
        WrapperLog("IAT: Could not allocate ownership record for %s!%s in module %p", sourceModule, functionName,
                   targetModule);
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(&iatEntry->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;

    void* previousOutOriginal = outOriginal ? *outOriginal : nullptr;
    if (outOriginal)
        *outOriginal = currentFunction;
    MemoryBarrier();

    std::unique_lock<std::mutex> trackingLock(g_PatchLock);
    try {
        g_PatchedEntries.push_back(std::move(trackingEntry));
    } catch (...) {
        VirtualProtect(&iatEntry->u1.Function, sizeof(void*), oldProtect, &oldProtect);
        if (outOriginal)
            *outOriginal = previousOutOriginal;
        WrapperLog("IAT: Could not publish ownership record for %s!%s in module %p", sourceModule, functionName,
                   targetModule);
        return false;
    }

    // Claim the slot only if no foreign injector changed it after our initial
    // read.
    void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&iatEntry->u1.Function),
                                                       hookFunction, currentFunction);
    VirtualProtect(&iatEntry->u1.Function, sizeof(void*), oldProtect, &oldProtect);

    if (replaced != currentFunction) {
        g_PatchedEntries.pop_back();
        WrapperLog("IAT: Preserving concurrent replacement %p for %s!%s in module %p", replaced, sourceModule,
                   functionName, targetModule);
        if (outOriginal)
            *outOriginal = previousOutOriginal;
        return false;
    }

    if (lookup.usedResolvedAddress) {
        WrapperLog("IAT: Successfully patched name-less %s!%s in module %p by resolved address", sourceModule,
                   functionName, targetModule);
    } else {
        WrapperLog("IAT: Successfully patched %s!%s in module %p", sourceModule, functionName, targetModule);
    }
    WrapperLog("IAT: Patched %s!%s in module %p", sourceModule, functionName, targetModule);
    return true;
}

bool PatchIATAllModulesFiltered(const char* sourceModule, const char* functionName, void* hookFunction,
                                void** outOriginal, IATTargetModuleFilter targetFilter) {
    bool anyPatched = false;
    void* firstOriginal = nullptr;

    // Get list of all loaded modules
    std::vector<HMODULE> modules;
    if (ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
        for (size_t i = 0; i < modules.size(); ++i) {
            void* orig = nullptr;

            // helpful for debugging - see which modules we actually scan
            WCHAR szModName[MAX_PATH];
            if (GetModuleFileNameExW(GetCurrentProcess(), modules[i], szModName, MAX_PATH)) {
                if (targetFilter && !targetFilter(modules[i], szModName)) {
                    continue;
                }
                std::wstring wsModName(szModName);
                if (wsModName.find(L"capture_hook") != std::wstring::npos ||
                    wsModName.find(L"d3d12_wrappers") != std::wstring::npos ||
                    ce::overlay_compat::IsThirdPartyOverlayModulePath(szModName) ||
                    IsNonSystemGraphicsProxyModulePath(szModName)) {
                    // Skip our own modules and third-party overlay DLLs that also hook
                    // graphics APIs. Patching their GetProcAddress IAT causes them to
                    // receive our hook address as the "original" function, creating a
                    // mutual infinite recursion: our wrapper -> overlay hook -> our wrapper...
                    continue;
                }
                // Skip Streamline modules.  SL creates its own internal DXGI
                // factories during DllMain.  Patching their IAT causes CE to
                // wrap those factories and install Present hooks, which triggers
                // Steam's overlay to call Present on the SL worker thread where
                // Steam is uninitialized → null pointer crash at RIP=0.
                if (ce::overlay_compat::IsStreamlineFrameGenerationModulePath(szModName)) {
                    continue;
                }
                if (_stricmp(functionName, "nvapi_QueryInterface") == 0 &&
                    (ce::overlay_compat::IsStreamlineFrameGenerationModulePath(szModName) ||
                     ce::overlay_compat::IsFFXFrameGenerationModulePath(szModName))) {
                    continue;
                }
            } else if (targetFilter) {
                continue;
            }

            if (PatchIAT(modules[i], sourceModule, functionName, hookFunction, &orig)) {
                if (!firstOriginal && orig) {
                    firstOriginal = orig;
                }
                anyPatched = true;
            }
        }
    }

    // Also patch main exe
    if (!anyPatched) {
        void* orig = nullptr;
        anyPatched = PatchIAT(nullptr, sourceModule, functionName, hookFunction, &orig);
        if (anyPatched && orig) {
            firstOriginal = orig;
        }
    }

    if (outOriginal && firstOriginal) {
        *outOriginal = firstOriginal;
    }

    return anyPatched;
}

bool PatchIATAllModules(const char* sourceModule, const char* functionName, void* hookFunction, void** outOriginal) {
    return PatchIATAllModulesFiltered(sourceModule, functionName, hookFunction, outOriginal, nullptr);
}

bool RestoreIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* originalFunction) {
    std::lock_guard<std::mutex> lock(g_PatchLock);

    for (auto it = g_PatchedEntries.begin(); it != g_PatchedEntries.end(); ++it) {
        if ((!targetModule || it->targetModule == targetModule) &&
            _stricmp(it->sourceModule.c_str(), sourceModule) == 0 &&
            it->functionName == functionName) {
            MEMORY_BASIC_INFORMATION memory = {};
            if (VirtualQuery(reinterpret_cast<const void*>(it->iatEntry), &memory, sizeof(memory)) != sizeof(memory) ||
                memory.State != MEM_COMMIT || memory.Type != MEM_IMAGE ||
                memory.AllocationBase != it->targetModule ||
                (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                WrapperLog("IAT: Dropping unavailable ownership record for %s!%s in module %p", sourceModule,
                           functionName, it->targetModule);
                g_PatchedEntries.erase(it);
                return true;
            }
            void* current = *it->iatEntry;
            if (current != it->hookFunction) {
                WrapperLog("IAT: Preserving foreign replacement %p for %s!%s in module %p (CE hook=%p)",
                           current, sourceModule, functionName, it->targetModule, it->hookFunction);
                g_PatchedEntries.erase(it);
                return true;
            }
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(it->iatEntry), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                void* restoreValue = originalFunction ? originalFunction : it->originalFunction;
                void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(it->iatEntry),
                                                                   restoreValue, it->hookFunction);
                VirtualProtect(reinterpret_cast<void*>(it->iatEntry), sizeof(void*), oldProtect, &oldProtect);

                if (replaced != it->hookFunction) {
                    WrapperLog("IAT: Preserving concurrent replacement %p for %s!%s in module %p", replaced,
                               sourceModule, functionName, it->targetModule);
                }

                g_PatchedEntries.erase(it);
                return true;
            }
            break;
        }
    }

    return false;
}

// ============================================================================
// EAT Patching (for hooking exports)
// ============================================================================

bool PatchEAT(HMODULE exportingModule, const char* functionName, void* hookFunction, void** outOriginal) {
    DWORD exportSize = 0;
    auto exportDir = GetExportDirectory(exportingModule, &exportSize);
    if (!exportDir)
        return false;

    auto names = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(exportingModule) + exportDir->AddressOfNames);
    auto ordinals =
        reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(exportingModule) + exportDir->AddressOfNameOrdinals);
    auto functions = reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(exportingModule) + exportDir->AddressOfFunctions);

    for (DWORD i = 0; i < exportDir->NumberOfNames; ++i) {
        auto name = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(exportingModule) + names[i]);

        if (strcmp(name, functionName) == 0) {
            DWORD funcRVA = functions[ordinals[i]];

            // Save original
            if (outOriginal) {
                *outOriginal = reinterpret_cast<void*>(reinterpret_cast<BYTE*>(exportingModule) + funcRVA);
            }

            // Calculate new RVA with bounds check.
            // hookFunction must be within the exporting module's address range.
            MODULEINFO modInfo = {};
            if (!GetModuleInformation(GetCurrentProcess(), exportingModule, &modInfo, sizeof(modInfo))) {
                WrapperLog("EAT: Failed to get module info for %s", functionName);
                break;
            }
            BYTE* modBase = reinterpret_cast<BYTE*>(exportingModule);
            BYTE* hookAddr = reinterpret_cast<BYTE*>(hookFunction);
            if (hookAddr < modBase || hookAddr >= modBase + modInfo.SizeOfImage) {
                WrapperLog("EAT: hookFunction %p outside exporting module range [%p, %p) for %s", hookFunction, modBase,
                           modBase + modInfo.SizeOfImage, functionName);
                break;
            }
            DWORD newRVA = static_cast<DWORD>(hookAddr - modBase);

            DWORD oldProtect;
            if (VirtualProtect(&functions[ordinals[i]], sizeof(DWORD), PAGE_READWRITE, &oldProtect)) {
                functions[ordinals[i]] = newRVA;
                VirtualProtect(&functions[ordinals[i]], sizeof(DWORD), oldProtect, &oldProtect);

                WrapperLog("EAT: Patched %s", functionName);
                return true;
            }
            break;
        }
    }

    return false;
}

}  // namespace IATHook
