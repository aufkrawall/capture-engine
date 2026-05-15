/**
 * IAT (Import Address Table) Patching Implementation
 *
 * Provides MinHook-free API hooking by modifying import tables.
 */

#include "iat_hook.h"
#include <d3d12.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "../apis/dx11_hook.h"
#include "../apis/lod_helper.h"
#include "../common/overlay_compat.h"
#include "../common/sampler_override_utils.h"
#include "hook_common.h"
#include "wrapper_hooks.h"

#pragma comment(lib, "psapi.lib")

// ============================================================================
// D3D12 Sampler Override Callback
// ============================================================================
// This callback is registered with d3d12_wrappers.dll to apply AF/mip bias
// overrides. It has access to hook_common for config.

extern "C" BOOL WINAPI ApplyDX12SamplerOverridesCallback(D3D12_SAMPLER_DESC* pDesc) {
    if (!pDesc)
        return FALSE;

    // Skip overrides for samplers that have no mipmapping (MipLevels == 1 equivalent):
    // MaxLOD == 0.0f means the sampler is clamped to the base mip level only.
    // MinLOD == MaxLOD means a single fixed LOD is selected.
    // Applying mip bias or AF to single-mip textures can cause GPU driver corruption
    // on some hardware (e.g., Nvidia). D3D12 decouples samplers from textures, so we
    // cannot check the texture's actual MipLevels here; this LOD-based heuristic covers
    // the standard case where games set MaxLOD=0 for non-mipmapped samplers.
    if (pDesc->MaxLOD == 0.0f)
        return FALSE;
    if (pDesc->MinLOD == pDesc->MaxLOD)
        return FALSE;

    const auto& gfx = GetActiveGraphicsConfig();

    D3D12_FILTER origFilter = pDesc->Filter;
    UINT origAniso = pDesc->MaxAnisotropy;
    float origBias = pDesc->MipLODBias;

    // Anisotropic Filtering override
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
        if (af == "off") {
            if (ce::sampler_override::IsD3D12AnisotropicFilter(pDesc->Filter)) {
                bool wasComparison = ce::sampler_override::IsD3D12ComparisonFilter(pDesc->Filter);
                pDesc->Filter =
                    wasComparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                pDesc->MaxAnisotropy = 1;
            }
        } else {
            UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);

            if (pDesc->AddressU != D3D12_TEXTURE_ADDRESS_MODE_BORDER &&
                pDesc->AddressV != D3D12_TEXTURE_ADDRESS_MODE_BORDER &&
                pDesc->AddressW != D3D12_TEXTURE_ADDRESS_MODE_BORDER) {
                pDesc->Filter = ce::sampler_override::GetForcedAnisotropicFilter(pDesc->Filter);
                pDesc->MaxAnisotropy = maxAniso;
            }
        }
    }

    // Mip Bias override
    std::string biasStr = gfx.mipBias;
    if (biasStr != "default" || gfx.forceMipBiasClamp) {
        pDesc->MipLODBias = ApplyConfiguredMipBias(gfx, pDesc->MipLODBias);
        pDesc->MipLODBias = FinalizeMipBias(gfx, pDesc->MipLODBias);
    }

    if (origFilter != pDesc->Filter || origAniso != pDesc->MaxAnisotropy || origBias != pDesc->MipLODBias) {
        HookLog(
            "ApplyDX12SamplerOverridesCallback: MODIFIED, Filter=0x%X->0x%X, "
            "Aniso=%d->%d, Bias=%.2f->%.2f",
            origFilter, pDesc->Filter, origAniso, pDesc->MaxAnisotropy, origBias, pDesc->MipLODBias);
    }

    return TRUE;
}

namespace IATHook {

// Track patched entries for restoration
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

static std::mutex g_DynamicHookLock;
static std::unordered_map<std::string, DynamicHookEntry> g_DynamicHooks;
static FARPROC(WINAPI* oGetProcAddress)(HMODULE, LPCSTR) = nullptr;

static std::mutex g_PatchLock;
static std::vector<PatchedEntry> g_PatchedEntries;

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
                                WrapperLog("IAT: %s!%s in module %p already patched", sourceModule, functionName,
                                           targetModule);
                                return true;
                            }
                            WrapperLog("IAT: %s!%s in module %p already points at hook but original is not tracked",
                                       sourceModule, functionName, targetModule);
                            return false;
                        }

                        DWORD oldProtect;
                        if (VirtualProtect(&iatEntry->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                            // Save original
                            if (outOriginal) {
                                *outOriginal = currentFunction;
                            }

                            // Patch
                            iatEntry->u1.Function = reinterpret_cast<ULONG_PTR>(hookFunction);

                            VirtualProtect(&iatEntry->u1.Function, sizeof(void*), oldProtect, &oldProtect);

                            WrapperLog("IAT: Successfully patched %s!%s in module %p", sourceModule, functionName,
                                       targetModule);

                            // Track for restoration
                            std::lock_guard<std::mutex> lock(g_PatchLock);
                            g_PatchedEntries.push_back({targetModule, sourceModule, functionName, hookFunction,
                                                        outOriginal ? *outOriginal : nullptr,
                                                        reinterpret_cast<void**>(&iatEntry->u1.Function)});

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

bool PatchIATAllModules(const char* sourceModule, const char* functionName, void* hookFunction, void** outOriginal) {
    bool anyPatched = false;
    void* firstOriginal = nullptr;

    // Get list of all loaded modules
    HMODULE modules[1024];
    DWORD needed;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        int count = needed / sizeof(HMODULE);

        for (int i = 0; i < count; ++i) {
            void* orig = nullptr;

            // helpful for debugging - see which modules we actually scan
            WCHAR szModName[MAX_PATH];
            if (GetModuleFileNameExW(GetCurrentProcess(), modules[i], szModName, MAX_PATH)) {
                std::wstring wsModName(szModName);
                if (wsModName.find(L"capture_hook") != std::wstring::npos ||
                    wsModName.find(L"d3d12_wrappers") != std::wstring::npos ||
                    ce::overlay_compat::IsThirdPartyOverlayModulePath(szModName)) {
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

bool RestoreIAT(HMODULE targetModule, const char* sourceModule, const char* functionName, void* originalFunction) {
    std::lock_guard<std::mutex> lock(g_PatchLock);

    for (auto it = g_PatchedEntries.begin(); it != g_PatchedEntries.end(); ++it) {
        if (it->targetModule == targetModule && _stricmp(it->sourceModule.c_str(), sourceModule) == 0 &&
            it->functionName == functionName) {
            DWORD oldProtect;
            if (VirtualProtect(it->iatEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                *it->iatEntry = originalFunction ? originalFunction : it->originalFunction;
                VirtualProtect(it->iatEntry, sizeof(void*), oldProtect, &oldProtect);

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
                WrapperLog("EAT: hookFunction %p outside exporting module range [%p, %p) for %s",
                           hookFunction, modBase, modBase + modInfo.SizeOfImage, functionName);
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

// ============================================================================
// DXGI/D3D Hook Initialization
// ============================================================================

}  // namespace IATHook

// Forward declarations for wrapped functions (from wrapper_hooks.cpp)
// These are in global namespace, not IATHook namespace
extern HRESULT WINAPI Wrapped_CreateDXGIFactory(REFIID riid, void** ppFactory);
extern HRESULT WINAPI Wrapped_CreateDXGIFactory1(REFIID riid, void** ppFactory);
extern HRESULT WINAPI Wrapped_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory);

#ifdef ENABLE_D3D12_WRAPPER
extern HRESULT WINAPI Wrapped_D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid,
                                                void** ppDevice);
#endif

// Original function pointers (defined in wrapper_hooks.cpp)
extern PFN_CreateDXGIFactory oCreateDXGIFactory;
extern PFN_CreateDXGIFactory1 oCreateDXGIFactory1;
extern PFN_CreateDXGIFactory2 oCreateDXGIFactory2;

// D3D12 Root Signature hooks (from dx12_hook.cpp)
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*,
                                                                      ID3DBlob**, ID3DBlob**);

extern PFN_D3D12_SERIALIZE_ROOT_SIGNATURE oSerializeRootSignature;
extern PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE oSerializeVersionedRootSignature;

extern HRESULT WINAPI DetourSerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
                                                   D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob,
                                                   ID3DBlob** ppErrorBlob);

extern HRESULT WINAPI DetourSerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature,
                                                            ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);

namespace IATHook {

bool InitializeDXGIHooks() {
    WrapperLog("IAT: Initializing DXGI hooks...");
    bool success = true;

    // Get dxgi.dll - if not loaded, we'll hook when it loads
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");

    if (hDXGI) {
        // Get original functions from dxgi.dll
        oCreateDXGIFactory = reinterpret_cast<PFN_CreateDXGIFactory>(GetProcAddress(hDXGI, "CreateDXGIFactory"));
        oCreateDXGIFactory1 = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(hDXGI, "CreateDXGIFactory1"));
        oCreateDXGIFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(hDXGI, "CreateDXGIFactory2"));

        // Patch IAT in all modules
        void* dummy;
        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory", (void*)Wrapped_CreateDXGIFactory, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory not found in IAT (may not be imported)");
        }

        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory1", (void*)Wrapped_CreateDXGIFactory1, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory1 not found in IAT");
        }

        if (!PatchIATAllModules("dxgi.dll", "CreateDXGIFactory2", (void*)Wrapped_CreateDXGIFactory2, &dummy)) {
            WrapperLog("IAT: CreateDXGIFactory2 not found in IAT");
        }

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        // Games like Strange Brigade may load dxgi.dll dynamically at runtime
        // and use GetProcAddress to get factory creation functions. Without
        // dynamic hooks, we won't intercept these calls.
        RegisterDynamicHook("CreateDXGIFactory", (void*)Wrapped_CreateDXGIFactory, (void**)&oCreateDXGIFactory);
        RegisterDynamicHook("CreateDXGIFactory1", (void*)Wrapped_CreateDXGIFactory1, (void**)&oCreateDXGIFactory1);
        RegisterDynamicHook("CreateDXGIFactory2", (void*)Wrapped_CreateDXGIFactory2, (void**)&oCreateDXGIFactory2);
        WrapperLog("IAT: Registered DXGI factory functions for dynamic hooking");

        WrapperLog("IAT: DXGI hooks initialized");
    } else {
        WrapperLog("IAT: dxgi.dll not loaded");
        success = false;
    }

    return success;
}

bool InitializeD3D12Hooks() {
    WrapperLog("IAT: Initializing D3D12 hooks...");

    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hD3D12) {
        WrapperLog("IAT: d3d12.dll not loaded");
        return false;
    }

    // Hook D3D12SerializeRootSignature and D3D12SerializeVersionedRootSignature
    // These handle static samplers for AF/mip bias overrides
    // CRITICAL: These hooks are needed even without ENABLE_D3D12_WRAPPER
    oSerializeRootSignature =
        reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(hD3D12, "D3D12SerializeRootSignature"));

    oSerializeVersionedRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(
        GetProcAddress(hD3D12, "D3D12SerializeVersionedRootSignature"));

    void* dummy;
    if (oSerializeRootSignature) {
        PatchIATAllModules("d3d12.dll", "D3D12SerializeRootSignature", (void*)DetourSerializeRootSignature, &dummy);
        WrapperLog("IAT: Hooked D3D12SerializeRootSignature");
    }

    if (oSerializeVersionedRootSignature) {
        PatchIATAllModules("d3d12.dll", "D3D12SerializeVersionedRootSignature",
                           (void*)DetourSerializeVersionedRootSignature, &dummy);
        WrapperLog("IAT: Hooked D3D12SerializeVersionedRootSignature");
    }

#ifdef ENABLE_D3D12_WRAPPER
    // D3D12CreateDevice wrapper requires d3d12_wrappers.dll which may not exist
    WrapperLog("IAT: Initializing D3D12CreateDevice wrapper...");

    // CRITICAL: Pre-load d3d12_wrappers.dll to avoid delay-load race condition
    // When the game calls D3D12CreateDevice, Wrapped_D3D12CreateDevice will
    // call D3D12Wrapper_WrapDevice which is in d3d12_wrappers.dll. If we don't
    // preload it here, the delay-load mechanism could crash due to thread
    // safety issues.
    static bool s_WrappersPreloaded = false;
    if (!s_WrappersPreloaded) {
        s_WrappersPreloaded = true;
        HMODULE hWrappers = LoadLibraryA("d3d12_wrappers.dll");
        if (!hWrappers) {
            hWrappers = LoadLibraryA("d3d12_wrappers_x86.dll");
        }
        if (hWrappers) {
            WrapperLog("IAT: Pre-loaded d3d12_wrappers.dll at %p", hWrappers);

            // Register sampler override callback for AF/mip bias support
            typedef void(WINAPI * PFN_SetSamplerOverrideCallback)(void* callback);
            auto* setCallback =
                (PFN_SetSamplerOverrideCallback)GetProcAddress(hWrappers, "D3D12Wrapper_SetSamplerOverrideCallback");
            if (setCallback) {
                setCallback((void*)ApplyDX12SamplerOverridesCallback);
                WrapperLog("IAT: Registered D3D12 sampler override callback");
            }
        } else {
            WrapperLog(
                "IAT: WARNING - Could not pre-load d3d12_wrappers.dll, "
                "delay-load will be used. Err=%d",
                GetLastError());
        }
    }

    oD3D12CreateDevice = reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(hD3D12, "D3D12CreateDevice"));

    bool patchResult = PatchIATAllModules("d3d12.dll", "D3D12CreateDevice", (void*)Wrapped_D3D12CreateDevice, &dummy);
    if (!patchResult) {
        WrapperLog("IAT: D3D12CreateDevice not found in IAT");
    }

    WrapperLog("IAT: D3D12 hooks initialized (patchResult=%d)", patchResult);
#else
    WrapperLog("IAT: D3D12 hooks initialized (root signature hooks only)");
#endif

    return true;
}

bool InitializeD3D11Hooks() {
    WrapperLog("IAT: Initializing D3D11 hooks...");

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");

    if (hD3D11) {
        // Get original D3D11CreateDeviceAndSwapChain
        // oD3D11CreateDeviceAndSwapChain and DX11_DetourCreateDeviceAndSwapChain
        // are declared in dx11_hook.h

        ::oD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(
            GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain"));

        ::oD3D11CreateDevice = reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(hD3D11, "D3D11CreateDevice"));

        void* dummy;
        bool patched = false;

        // Patch D3D11CreateDeviceAndSwapChain
        if (PatchIATAllModules("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                               (void*)::Wrapped_D3D11CreateDeviceAndSwapChain,
                               &dummy)) {  // Use Wrapped_, not DX11_Detour
            WrapperLog("IAT: Patched D3D11CreateDeviceAndSwapChain");
            patched = true;
        } else {
            WrapperLog("IAT: D3D11CreateDeviceAndSwapChain not found in IAT");
        }

        // Patch D3D11CreateDevice
        if (PatchIATAllModules("d3d11.dll", "D3D11CreateDevice", (void*)::Wrapped_D3D11CreateDevice, &dummy)) {
            WrapperLog("IAT: Patched D3D11CreateDevice");
            patched = true;
        } else {
            WrapperLog("IAT: D3D11CreateDevice not found in IAT");
        }

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        // Games may load d3d11.dll dynamically at runtime and use GetProcAddress
        // to get device creation functions. Without dynamic hooks, we won't
        // intercept these calls.
        RegisterDynamicHook("D3D11CreateDeviceAndSwapChain", (void*)::Wrapped_D3D11CreateDeviceAndSwapChain,
                            (void**)&::oD3D11CreateDeviceAndSwapChain);
        RegisterDynamicHook("D3D11CreateDevice", (void*)::Wrapped_D3D11CreateDevice, (void**)&::oD3D11CreateDevice);
        WrapperLog("IAT: Registered D3D11 functions for dynamic hooking");

        WrapperLog("IAT: D3D11 hooks initialized");
        return true;
    }

    return false;
}

bool InitializeD3D10Hooks() {
    WrapperLog("IAT: Initializing D3D10 hooks...");

    HMODULE hD3D10 = GetModuleHandleA("d3d10.dll");

    if (hD3D10) {
        ::oD3D10CreateDevice = (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
        ::oD3D10CreateDeviceAndSwapChain =
            (PFN_D3D10CreateDeviceAndSwapChain)GetProcAddress(hD3D10, "D3D10CreateDeviceAndSwapChain");

        void* dummy;
        PatchIATAllModules("d3d10.dll", "D3D10CreateDevice", (void*)::Wrapped_D3D10CreateDevice, &dummy);
        PatchIATAllModules("d3d10.dll", "D3D10CreateDeviceAndSwapChain", (void*)::Wrapped_D3D10CreateDeviceAndSwapChain,
                           &dummy);

        // CRITICAL FIX: Register dynamic hooks for GetProcAddress interception
        RegisterDynamicHook("D3D10CreateDevice", (void*)::Wrapped_D3D10CreateDevice, (void**)&::oD3D10CreateDevice);
        RegisterDynamicHook("D3D10CreateDeviceAndSwapChain", (void*)::Wrapped_D3D10CreateDeviceAndSwapChain,
                            (void**)&::oD3D10CreateDeviceAndSwapChain);

        // D3D10.1
        HMODULE hD3D10_1 = GetModuleHandleA("d3d10_1.dll");
        if (hD3D10_1) {
            ::oD3D10CreateDevice1 = (PFN_D3D10CreateDevice1)GetProcAddress(hD3D10_1, "D3D10CreateDevice1");
            PatchIATAllModules("d3d10_1.dll", "D3D10CreateDevice1", (void*)::Wrapped_D3D10CreateDevice1, &dummy);
            RegisterDynamicHook("D3D10CreateDevice1", (void*)::Wrapped_D3D10CreateDevice1,
                                (void**)&::oD3D10CreateDevice1);
        }

        WrapperLog("IAT: D3D10 hooks initialized");
        return true;
    }
    return false;
}

bool InitializeD3D9Hooks() {
    WrapperLog("IAT: Initializing D3D9 hooks...");

    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");

    if (hD3D9) {
        // Get original functions
        oDirect3DCreate9 = reinterpret_cast<PFN_Direct3DCreate9>(GetProcAddress(hD3D9, "Direct3DCreate9"));
        oDirect3DCreate9Ex = reinterpret_cast<PFN_Direct3DCreate9Ex>(GetProcAddress(hD3D9, "Direct3DCreate9Ex"));

        void* dummy;
        // Patch Direct3DCreate9
        if (PatchIATAllModules("d3d9.dll", "Direct3DCreate9", (void*)Wrapped_Direct3DCreate9, &dummy)) {
            WrapperLog("IAT: Patched Direct3DCreate9");
        } else {
            // Also try explicit GetProcAddress target for late binding
            if (oDirect3DCreate9) {
                // If IAT search failed, it might be due to ordinal-only or forwarded
                // export. But generally "Direct3DCreate9" is by name.
                WrapperLog("IAT: Direct3DCreate9 not found in IAT");
            }
        }

        // Patch Direct3DCreate9Ex
        if (PatchIATAllModules("d3d9.dll", "Direct3DCreate9Ex", (void*)Wrapped_Direct3DCreate9Ex, &dummy)) {
            WrapperLog("IAT: Patched Direct3DCreate9Ex");
        } else {
            WrapperLog("IAT: Direct3DCreate9Ex not found in IAT");
        }

        WrapperLog("IAT: D3D9 hooks initialized");
        return true;
    }

    WrapperLog("IAT: d3d9.dll not loaded");
    return false;
}

bool InitializeDDrawHooks() {
    WrapperLog("IAT: Initializing DirectDraw hooks...");

    HMODULE hDDraw = GetModuleHandleA("ddraw.dll");
    if (!hDDraw) {
        WrapperLog("IAT: ddraw.dll not loaded");
        return false;
    }

    oDirectDrawCreateEx = reinterpret_cast<PFN_DirectDrawCreateEx>(GetProcAddress(hDDraw, "DirectDrawCreateEx"));

    void* dummy = nullptr;
    if (PatchIATAllModules("ddraw.dll", "DirectDrawCreateEx", (void*)Wrapped_DirectDrawCreateEx, &dummy)) {
        WrapperLog("IAT: Patched DirectDrawCreateEx");
    } else {
        WrapperLog("IAT: DirectDrawCreateEx not found in IAT");
    }

    RegisterDynamicHook("DirectDrawCreateEx", (void*)Wrapped_DirectDrawCreateEx, (void**)&oDirectDrawCreateEx);
    WrapperLog("IAT: DirectDraw hooks initialized");
    return true;
}

// Note: InitializeVulkanHooks removed - Vulkan is now handled by
// VK_LAYER_CE_overlay The Vulkan layer approach provides better compatibility
// and doesn't require IAT patching

bool InitializeKernel32Hooks(void* LoadLibraryAHook, void** pOriginalLoadLibraryA, void* LoadLibraryWHook,
                             void** pOriginalLoadLibraryW, void* LoadLibraryExAHook, void** pOriginalLoadLibraryExA,
                             void* LoadLibraryExWHook, void** pOriginalLoadLibraryExW, void* CreateProcessAHook,
                             void** pOriginalCreateProcessA, void* CreateProcessWHook, void** pOriginalCreateProcessW) {
    WrapperLog("IAT: Initializing kernel32 hooks...");

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32) {
        WrapperLog("IAT: kernel32.dll not loaded (unexpected!)");
        return false;
    }

    bool success = true;

    // Get original function addresses
    if (pOriginalLoadLibraryA) {
        *pOriginalLoadLibraryA = (void*)GetProcAddress(hKernel32, "LoadLibraryA");
    }
    if (pOriginalLoadLibraryW) {
        *pOriginalLoadLibraryW = (void*)GetProcAddress(hKernel32, "LoadLibraryW");
    }
    if (pOriginalLoadLibraryExA) {
        *pOriginalLoadLibraryExA = (void*)GetProcAddress(hKernel32, "LoadLibraryExA");
    }
    if (pOriginalLoadLibraryExW) {
        *pOriginalLoadLibraryExW = (void*)GetProcAddress(hKernel32, "LoadLibraryExW");
    }
    if (pOriginalCreateProcessA) {
        *pOriginalCreateProcessA = (void*)GetProcAddress(hKernel32, "CreateProcessA");
    }
    if (pOriginalCreateProcessW) {
        *pOriginalCreateProcessW = (void*)GetProcAddress(hKernel32, "CreateProcessW");
    }

    // Patch LoadLibrary* in all modules
    void* dummy;
    if (LoadLibraryAHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryA", LoadLibraryAHook, &dummy);
    }
    if (LoadLibraryWHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryW", LoadLibraryWHook, &dummy);
    }
    if (LoadLibraryExAHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryExA", LoadLibraryExAHook, &dummy);
    }
    if (LoadLibraryExWHook) {
        PatchIATAllModules("kernel32.dll", "LoadLibraryExW", LoadLibraryExWHook, &dummy);
    }

    // Patch CreateProcess* in all modules
    if (CreateProcessAHook) {
        PatchIATAllModules("kernel32.dll", "CreateProcessA", CreateProcessAHook, &dummy);
    }
    if (CreateProcessWHook) {
        PatchIATAllModules("kernel32.dll", "CreateProcessW", CreateProcessWHook, &dummy);
    }

    WrapperLog("IAT: kernel32 hooks initialized");
    return success;
}

bool InitializeAdvapi32Hooks(void* RegQueryValueExWHook, void** pOriginalRegQueryValueExW) {
    WrapperLog("IAT: Initializing advapi32 hooks...");

    HMODULE hAdvapi32 = GetModuleHandleA("advapi32.dll");
    if (!hAdvapi32) {
        WrapperLog("IAT: advapi32.dll not loaded");
        return false;
    }

    // Get original function address
    if (pOriginalRegQueryValueExW) {
        *pOriginalRegQueryValueExW = (void*)GetProcAddress(hAdvapi32, "RegQueryValueExW");
    }

    // Patch in all modules
    void* dummy;
    if (RegQueryValueExWHook) {
        PatchIATAllModules("advapi32.dll", "RegQueryValueExW", RegQueryValueExWHook, &dummy);
    }

    WrapperLog("IAT: advapi32 hooks initialized");
    return true;
}

void ShutdownIATHooks() {
    std::lock_guard<std::mutex> lock(g_PatchLock);

    // Restore all patched entries
    for (auto& entry : g_PatchedEntries) {
        DWORD oldProtect;
        if (VirtualProtect(entry.iatEntry, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *entry.iatEntry = entry.originalFunction;
            VirtualProtect(entry.iatEntry, sizeof(void*), oldProtect, &oldProtect);
        }
    }

    g_PatchedEntries.clear();

    // CRITICAL FIX: Clear dynamic hooks map to prevent memory leak
    // and stale pointers on DLL unload
    {
        std::lock_guard<std::mutex> dynLock(g_DynamicHookLock);
        g_DynamicHooks.clear();
    }

    WrapperLog("IAT: All hooks restored");
}

// ============================================================================
// Dynamic Hooking Implementation (GetProcAddress)
// ============================================================================

FARPROC WINAPI DetourGetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    // CRITICAL: During process termination, static data may be invalid
    // External DLLs (e.g., opengl32.dll) may call GetProcAddress during their
    // atexit destructors. We must not access any static data that may have
    // been destroyed.
    if (IsProcessTerminating()) {
        if (oGetProcAddress)
            return oGetProcAddress(hModule, lpProcName);
        return nullptr;
    }

    // Call original first to get the real address
    FARPROC proc = nullptr;
    if (oGetProcAddress) {
        proc = oGetProcAddress(hModule, lpProcName);
    }

    // If getting address failed, or if name is invalid (ordinal), return result
    // immediately
    if (!proc || (uintptr_t)lpProcName < 0x10000) {
        return proc;
    }

    // CRITICAL: Resolve EAT-patching pollution. When the EAT (Export Address Table)
    // of a module like d3d11.dll has been patched via PatchEAT(), the original
    // GetProcAddress now returns our wrapper function instead of the real function.
    // This is intentional for non-system callers (the game), but causes infinite
    // recursion when system DLLs (SysWOW64\SysWOW64) or overlay DLLs internally call
    // GetProcAddress on the same function — they get our wrapper, call it, wrapper
    // calls the original, original calls GetProcAddress on itself → loop.
    //
    // Fix: if `proc` matches one of our registered hook functions, resolve it to
    // the stored original address. This ensures system/overlay callers always get
    // the real function even when the EAT is patched.
    {
        // Check all registered hooks for a match against this proc address.
        // We hold g_DynamicHookLock, so the map is stable.
        std::lock_guard<std::mutex> lock(g_DynamicHookLock);
        for (const auto& hookEntry : g_DynamicHooks) {
            if ((void*)proc == hookEntry.second.hookFunction && hookEntry.second.outOriginal &&
                *hookEntry.second.outOriginal) {
                proc = (FARPROC)*hookEntry.second.outOriginal;
                break;
            }
        }
    }

    // Get module name for logging
    char moduleName[MAX_PATH] = {0};
    if (hModule) {
        GetModuleFileNameA(hModule, moduleName, MAX_PATH);
        // Extract just the filename
        char* p = moduleName + strlen(moduleName);
        while (p > moduleName && *(p - 1) != '\\' && *(p - 1) != '/') {
            p--;
        }
        if (p > moduleName) {
            memmove(moduleName, p, strlen(p) + 1);
        }
    }

    // Check if we have a hook for this function name
    std::lock_guard<std::mutex> lock(g_DynamicHookLock);
    auto it = g_DynamicHooks.find(lpProcName);
    if (it != g_DynamicHooks.end()) {
        // Don't intercept GetProcAddress calls originating from Windows system DLLs or known
        // overlay DLLs. Those DLLs call GetProcAddress internally for their own implementation
        // purposes. Intercepting such calls causes mutual recursion with third-party overlays
        // (e.g., Steam's gameoverlayrenderer64) that also hook DXGI/D3D factory functions:
        //   Wrapped_CreateDXGIFactory2 -> oCreateDXGIFactory2 (Steam's hook) ->
        //   Steam reads a stored "original" that received our wrapper -> infinite loop.
        {
            void* callerAddr = __builtin_return_address(0);
            HMODULE callerMod = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)callerAddr, &callerMod) &&
                callerMod) {
                char callerPath[MAX_PATH] = {};
                if (GetModuleFileNameA(callerMod, callerPath, sizeof(callerPath))) {
                    for (char* p = callerPath; *p; ++p)
                        *p = (char)tolower((unsigned char)*p);
                    if (strstr(callerPath, "\\system32\\") || strstr(callerPath, "\\syswow64\\") ||
                        ce::overlay_compat::IsThirdPartyOverlayModulePath(callerPath) ||
                        strstr(callerPath, "capture_hook") || strstr(callerPath, "d3d12_wrappers") ||
                        ce::overlay_compat::IsStreamlineFrameGenerationModulePath(callerPath) ||
                        ce::overlay_compat::IsFFXFrameGenerationModulePath(callerPath)) {
                        return proc;
                    }
                }
            }
        }

        if (!ShouldApplyDynamicHookForModule(it->second.moduleFilter, moduleName[0] ? moduleName : nullptr, hModule)) {
            return proc;
        }

        // We found a hook!
        // Store the original address if requested
        if (it->second.outOriginal && *it->second.outOriginal == nullptr) {
            *it->second.outOriginal = (void*)proc;
        }

        // Return our hook address
        WrapperLog("GetProcAddress: Intercepting %s from %s (orig=%p, hook=%p)", lpProcName,
                   moduleName[0] ? moduleName : "unknown", proc, it->second.hookFunction);
        // CRITICAL: Log D3D11CreateDevice intercept at high visibility
        if (strcmp(lpProcName, "D3D11CreateDevice") == 0 || strcmp(lpProcName, "D3D11CreateDeviceAndSwapChain") == 0) {
            HookLogImportant("GetProcAddress: Intercepted %s -> Wrapped_%s (game=%s) — "
                             "preventing UE3 vtable-cache bypass",
                             lpProcName, lpProcName, moduleName[0] ? moduleName : "unknown");
        }
        return (FARPROC)it->second.hookFunction;
    }

    // Debug logging for DXGI/D3D functions that might be looked up
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount < 50) {
        if (strstr(lpProcName, "D3D11") || strstr(lpProcName, "DXGI") || strstr(lpProcName, "D3D12") ||
            strstr(lpProcName, "D3D10")) {
            WrapperLog("GetProcAddress: %s from %s (no hook registered)", lpProcName,
                       moduleName[0] ? moduleName : "unknown");
            s_LogCount++;
        }
    }

    return proc;
}

void RegisterDynamicHook(const char* functionName, void* hookFunction, void** outOriginal) {
    RegisterDynamicHookFiltered(functionName, hookFunction, outOriginal, nullptr);
}

void RegisterDynamicHookFiltered(const char* functionName, void* hookFunction, void** outOriginal,
                                 DynamicHookModuleFilter moduleFilter) {
    std::lock_guard<std::mutex> lock(g_DynamicHookLock);
    g_DynamicHooks[functionName] = {hookFunction, outOriginal, moduleFilter};
}

void InitializeGetProcAddressHook() {
    WrapperLog("IAT: Initializing GetProcAddress hook for dynamic interception...");

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (!hKernel32)
        return;

    oGetProcAddress = (FARPROC(WINAPI*)(HMODULE, LPCSTR))GetProcAddress(hKernel32, "GetProcAddress");

    if (oGetProcAddress) {
        void* dummy;
        PatchIATAllModules("kernel32.dll", "GetProcAddress", (void*)DetourGetProcAddress, &dummy);
        WrapperLog("IAT: GetProcAddress hook initialized");
    } else {
        WrapperLog("IAT: Failed to get GetProcAddress address");
    }
}

}  // namespace IATHook
