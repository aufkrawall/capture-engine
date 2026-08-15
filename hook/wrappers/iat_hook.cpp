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
static bool IsMemoryReadable(void* ptr, size_t size) {
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
    DWORD protect = mbi.Protect;
    if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) {
        return false;
    }

    // Check if the entire range is within this region
    SIZE_T regionSize = (char*)ptr + size - (char*)mbi.BaseAddress;
    if (regionSize > mbi.RegionSize) {
        return false;
    }

    return true;
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

static IMAGE_IMPORT_DESCRIPTOR* GetImportDescriptor(HMODULE module) {
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

    auto importDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir->VirtualAddress == 0)
        return nullptr;

    return reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<BYTE*>(module) + importDir->VirtualAddress);
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

bool PatchIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* hookFunction,
              void** outOriginal) {
    if (!targetModule)
        targetModule = GetModuleHandle(nullptr);
    if (!sourceModule || !functionName || !hookFunction)
        return false;

    auto importDesc = GetImportDescriptor(targetModule);
    if (!importDesc)
        return false;

    // Find the import descriptor for the source module
    bool moduleFound = false;
    while (importDesc->Name != 0) {
        auto moduleName = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(targetModule) + importDesc->Name);

        // Logs every module we encounter during search for target
        // WrapperLog("IAT Debug: Module found in IAT: %s", moduleName);

        if (_stricmp(moduleName, sourceModule) == 0) {
            moduleFound = true;
            // Found the module - now find the function
            auto thunkData = reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(targetModule) +
                                                                 importDesc->OriginalFirstThunk);
            auto iatEntry =
                reinterpret_cast<IMAGE_THUNK_DATA*>(reinterpret_cast<BYTE*>(targetModule) + importDesc->FirstThunk);

            // Check for OriginalFirstThunk == 0 case (Borland/Old Linkers)
            if (importDesc->OriginalFirstThunk == 0) {
                thunkData = iatEntry;
            }

            if (!thunkData) {
                WrapperLog("IAT: INT is null for %s in %p", sourceModule, targetModule);
                break;
            }

            while (thunkData->u1.AddressOfData != 0) {
                if (!(thunkData->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    auto importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<BYTE*>(targetModule) +
                                                                                thunkData->u1.AddressOfData);

                    if (strcmp(importByName->Name, functionName) == 0) {
                        // Found the function - patch the IAT entry
                        const auto currentFunction = reinterpret_cast<void*>(iatEntry->u1.Function);
                        if (currentFunction == hookFunction) {
                            void* originalFunction = nullptr;
                            if (TryGetTrackedOriginalForPatchedEntry(
                                    targetModule, sourceModule, functionName, hookFunction,
                                    reinterpret_cast<void**>(&iatEntry->u1.Function), &originalFunction)) {
                                if (outOriginal && originalFunction) {
                                    *outOriginal = originalFunction;
                                }
                                static std::atomic<uint32_t> alreadyPatchedLogs{0};
                                const uint32_t logIndex =
                                    alreadyPatchedLogs.fetch_add(1, std::memory_order_relaxed);
                                if (logIndex < 16 || (logIndex % 1000) == 0) {
                                    WrapperLog("IAT: %s!%s in module %p already patched", sourceModule,
                                               functionName, targetModule);
                                }
                                return true;
                            }
                            WrapperLog("IAT: %s!%s in module %p already points at hook but original is not tracked",
                                       sourceModule, functionName, targetModule);
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
                            WrapperLog("IAT: Could not allocate ownership record for %s!%s in module %p",
                                       sourceModule, functionName, targetModule);
                            return false;
                        }

                        DWORD oldProtect;
                        if (VirtualProtect(&iatEntry->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            // Save original
                            void* previousOutOriginal = outOriginal ? *outOriginal : nullptr;
                            if (outOriginal) {
                                *outOriginal = currentFunction;
                            }
                            MemoryBarrier();

                            std::unique_lock<std::mutex> trackingLock(g_PatchLock);
                            try {
                                g_PatchedEntries.push_back(std::move(trackingEntry));
                            } catch (...) {
                                VirtualProtect(&iatEntry->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                                if (outOriginal)
                                    *outOriginal = previousOutOriginal;
                                WrapperLog("IAT: Could not publish ownership record for %s!%s in module %p",
                                           sourceModule, functionName, targetModule);
                                return false;
                            }

                            // Claim the slot only if no foreign injector changed
                            // it after our initial read.
                            void* replaced = InterlockedCompareExchangePointer(
                                reinterpret_cast<PVOID volatile*>(&iatEntry->u1.Function), hookFunction,
                                currentFunction);

                            VirtualProtect(&iatEntry->u1.Function, sizeof(void*), oldProtect, &oldProtect);

                            if (replaced != currentFunction) {
                                g_PatchedEntries.pop_back();
                                WrapperLog("IAT: Preserving concurrent replacement %p for %s!%s in module %p",
                                           replaced, sourceModule, functionName, targetModule);
                                if (outOriginal)
                                    *outOriginal = previousOutOriginal;
                                return false;
                            }

                            WrapperLog("IAT: Successfully patched %s!%s in module %p", sourceModule, functionName,
                                       targetModule);

                            WrapperLog("IAT: Patched %s!%s in module %p", sourceModule, functionName, targetModule);
                            return true;
                        }
                    }
                }
                ++thunkData;
                ++iatEntry;
            }
        }
        ++importDesc;
    }

    if (!moduleFound && targetModule == GetModuleHandle(nullptr)) {
        // WrapperLog("IAT Debug: Module %s not imported by EXE", sourceModule);
    }

    return false;
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
