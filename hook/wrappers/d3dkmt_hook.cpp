/**
 * D3DKMT (Display Driver Kernel Mode) Hook Implementation
 *
 * This hooks the kernel-mode driver interface that games use to query VRAM
 * independently of DXGI. This is the universal solution used by SpecialK and
 * RTSS.
 */

#include <windows.h>
#include <algorithm>
#include "../common/hook_common.h"
#include "../common/logging.h"
#include "iat_hook.h"

// NTSTATUS definitions
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef STATUS_PROCEDURE_NOT_FOUND
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007A)
#endif

// D3DKMT structures and constants (normally from d3dkmthk.h)
// These are kernel-mode driver interfaces for GPU queries

typedef UINT64 D3DKMT_HANDLE;

typedef enum _KMT_MEMORY_SEGMENT_GROUP {
    KMT_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    KMT_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} KMT_MEMORY_SEGMENT_GROUP;

typedef struct _D3DKMT_QUERYVIDEOMEMORYINFO {
    D3DKMT_HANDLE hProcess;
    D3DKMT_HANDLE hAdapter;
    KMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 CurrentReservation;
    UINT64 AvailableForReservation;
    UINT64 PhysicalUsage;
} D3DKMT_QUERYVIDEOMEMORYINFO;

typedef enum _KMTQUERYADAPTERINFOTYPE {
    KMTQAITYPE_UMDRIVERPRIVATE = 0,
    KMTQAITYPE_UMDRIVERNAME = 1,
    KMTQAITYPE_UMOPENGLINFO = 2,
    KMTQAITYPE_GETSEGMENTSIZE = 3,
    KMTQAITYPE_ADAPTERGUID = 4,
    KMTQAITYPE_FLIPQUEUEINFO = 5,
    KMTQAITYPE_ADAPTERADDRESS = 6,
    KMTQAITYPE_SETWORKINGSETINFO = 7,
    KMTQAITYPE_ADAPTERREGISTRYINFO = 8,
    KMTQAITYPE_CURRENTDISPLAYMODE = 9,
    KMTQAITYPE_MODELIST = 10,
    KMTQAITYPE_CHECKDRIVERUPDATESTATUS = 11,
    KMTQAITYPE_VIRTUALADDRESSINFO = 12,
    KMTQAITYPE_DRIVERVERSION = 13,
    KMTQAITYPE_ADAPTERTYPE = 15,
    KMTQAITYPE_OUTPUTDUPLCONTEXTSCOUNT = 16,
    KMTQAITYPE_WDDM_1_2_CAPS = 17,
    KMTQAITYPE_UMD_DRIVER_VERSION = 18,
    KMTQAITYPE_DIRECTFLIP_SUPPORT = 19,
    KMTQAITYPE_MULTIPLANOVERLAY_SUPPORT = 20,
    KMTQAITYPE_DLIST_DRIVER_NAME = 21,
    KMTQAITYPE_WDDM_1_3_CAPS = 22,
    KMTQAITYPE_MULTIPLANOVERLAY_HUD_SUPPORT = 23,
    KMTQAITYPE_WDDM_2_0_CAPS = 24,
    KMTQAITYPE_NODEMETADATA = 25,
    KMTQAITYPE_CPDRIVERNAME = 26,
    KMTQAITYPE_XBOX = 27,
    KMTQAITYPE_INDEPENDENTFLIP_SUPPORT = 28,
    KMTQAITYPE_MIRACASTCOMPANIONDRIVERNAME = 29,
    KMTQAITYPE_PHYSICALADAPTERCOUNT = 30,
    KMTQAITYPE_PHYSICALADAPTERDEVICEIDS = 31,
    KMTQAITYPE_DRIVERCAPS_EXT = 32,
    KMTQAITYPE_QUERY_MIRACAST_DRIVER_TYPE = 33,
    KMTQAITYPE_QUERY_GPUMMU_CAPS = 34,
    KMTQAITYPE_QUERY_MULTIPLANOVERLAY_DECODE_SUPPORT = 35,
    KMTQAITYPE_QUERY_HW_PROTECTION_TEARDOWN_COUNT = 36,
    KMTQAITYPE_QUERY_ISBADDRIVER = 37,
    KMTQAITYPE_QUERY_MULTIPLANOVERLAY_SECONDARY_SUPPORT = 38,
    KMTQAITYPE_QUERY_DISPLAY_ADAPTER_INFO = 39,
    KMTQAITYPE_PHYSICALADAPTERCOUNT_FROM_ID = 40,
    KMTQAITYPE_GET_DEVICE_STATE = 41,
    KMTQAITYPE_QUERY_DMA_REMAPPING_SUPPORT = 42,
} KMTQUERYADAPTERINFOTYPE;

typedef struct _D3DKMT_QUERYADAPTERINFO {
    D3DKMT_HANDLE hAdapter;
    KMTQUERYADAPTERINFOTYPE Type;
    VOID* pPrivateDriverData;
    UINT PrivateDriverDataSize;
} D3DKMT_QUERYADAPTERINFO;

typedef struct _D3DKMT_ADAPTERINFO {
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE AdapterLuid;
    ULONG VidPnSourceId;
    ULONG NodeCount;
} D3DKMT_ADAPTERINFO;

typedef struct _D3DKMT_ENUMADAPTERS {
    ULONG NumAdapters;
    D3DKMT_ADAPTERINFO Adapters[16];
} D3DKMT_ENUMADAPTERS;

typedef struct _D3DKMT_ENUMADAPTERS2 {
    ULONG NumAdapters;
    D3DKMT_ADAPTERINFO* pAdapters;
} D3DKMT_ENUMADAPTERS2;

// Function prototypes
typedef NTSTATUS(WINAPI* PFN_D3DKMTQueryVideoMemoryInfo)(const D3DKMT_QUERYVIDEOMEMORYINFO*);
typedef NTSTATUS(WINAPI* PFN_D3DKMTQueryAdapterInfo)(const D3DKMT_QUERYADAPTERINFO*);
typedef NTSTATUS(WINAPI* PFN_D3DKMTEnumAdapters)(const D3DKMT_ENUMADAPTERS*);
typedef NTSTATUS(WINAPI* PFN_D3DKMTEnumAdapters2)(const D3DKMT_ENUMADAPTERS2*);

// Original function pointers
static PFN_D3DKMTQueryVideoMemoryInfo o_D3DKMTQueryVideoMemoryInfo = nullptr;
static PFN_D3DKMTQueryAdapterInfo o_D3DKMTQueryAdapterInfo = nullptr;
static PFN_D3DKMTEnumAdapters o_D3DKMTEnumAdapters = nullptr;
static PFN_D3DKMTEnumAdapters2 o_D3DKMTEnumAdapters2 = nullptr;

// VRAM override configuration
static struct {
    bool enabled = false;
    UINT64 dedicatedVramBytes = 0;
    UINT64 sharedVramBytes = 0;
    float scaleFactor = 1.0f;
} g_VramConfig;

// Initialize D3DKMT hooks
namespace D3DKMTHooks {

void InitializeConfig() {
    // Check if VRAM override is configured
    const auto& gfx = GetActiveGraphicsConfig();

    // IMPORTANT: Following SpecialK's approach - do NOT override VRAM values.
    // Games need real VRAM values for proper memory management and feature
    // detection. Overriding VRAM can cause:
    // - Games thinking there's insufficient VRAM
    // - DLSS/FSR framegen refusing to activate
    // - Texture streaming issues
    // - Memory allocation failures

    // VRAM override is disabled by default. Only enable via explicit config.
    g_VramConfig.enabled = false;
    g_VramConfig.dedicatedVramBytes = 0;
    g_VramConfig.sharedVramBytes = 0;
    g_VramConfig.scaleFactor = 1.0f;

    // Check if user explicitly wants VRAM override (not recommended)
    // Config key: d3dkmt_vram_override_mb = 8192
    // Only override if explicitly requested
    int explicitOverrideMB = 0;
    // TODO: Read from config if needed
    if (explicitOverrideMB > 0) {
        g_VramConfig.enabled = true;
        g_VramConfig.dedicatedVramBytes = static_cast<UINT64>(explicitOverrideMB) * 1024 * 1024;
        HookLog("D3DKMT: VRAM override ENABLED (user requested) - Dedicated: %d MB", explicitOverrideMB);
    } else {
        HookLog(
            "D3DKMT: VRAM override disabled - passing through real values "
            "(SpecialK-style)");
    }
}

// Hook for D3DKMTQueryVideoMemoryInfo
static NTSTATUS WINAPI Hook_D3DKMTQueryVideoMemoryInfo(const D3DKMT_QUERYVIDEOMEMORYINFO* pInfo) {
    if (!o_D3DKMTQueryVideoMemoryInfo) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    // Call original first
    NTSTATUS status = o_D3DKMTQueryVideoMemoryInfo(pInfo);

    if (!NT_SUCCESS(status) || !pInfo) {
        return status;
    }

    // Log the query for debugging
    HookLog("D3DKMT: QueryVideoMemoryInfo - Process=%u, Segment=%s",
            pInfo->hProcess ? (ULONG)(pInfo->hProcess) : GetCurrentProcessId(),
            pInfo->MemorySegmentGroup == KMT_MEMORY_SEGMENT_GROUP_LOCAL ? "LOCAL" : "NON_LOCAL");

    // Cast away const to modify the output struct (if needed)
    D3DKMT_QUERYVIDEOMEMORYINFO* pMutableInfo = const_cast<D3DKMT_QUERYVIDEOMEMORYINFO*>(pInfo);

    if (pInfo->MemorySegmentGroup == KMT_MEMORY_SEGMENT_GROUP_LOCAL) {
        // Local/Dedicated VRAM
        if (g_VramConfig.enabled) {
            UINT64 originalBudget = pInfo->Budget;
            pMutableInfo->Budget = g_VramConfig.dedicatedVramBytes;
            pMutableInfo->CurrentUsage =
                std::min(pInfo->CurrentUsage, g_VramConfig.dedicatedVramBytes - (512 * 1024 * 1024));
            pMutableInfo->AvailableForReservation = g_VramConfig.dedicatedVramBytes / 2;
            pMutableInfo->CurrentReservation =
                std::min(pInfo->CurrentReservation, pMutableInfo->AvailableForReservation);

            HookLog("D3DKMT: OVERRIDE Local VRAM - Budget: %llu->%llu MB", originalBudget / (1024 * 1024),
                    g_VramConfig.dedicatedVramBytes / (1024 * 1024));
        } else {
            // Pass-through mode: log real values for debugging
            static UINT queryCount = 0;
            if (++queryCount <= 5) {  // Only log first 5 queries to avoid spam
                HookLog("D3DKMT: Local VRAM PASSTHROUGH - Budget: %llu MB, Usage: %llu MB",
                        pInfo->Budget / (1024 * 1024), pInfo->CurrentUsage / (1024 * 1024));
            }
        }
    } else {
        // Non-local/Shared VRAM (system memory)
        if (g_VramConfig.enabled) {
            UINT64 originalBudget = pInfo->Budget;
            pMutableInfo->Budget = g_VramConfig.sharedVramBytes;
            pMutableInfo->CurrentUsage =
                std::min(pInfo->CurrentUsage, g_VramConfig.sharedVramBytes - (256 * 1024 * 1024));
            pMutableInfo->AvailableForReservation = g_VramConfig.sharedVramBytes / 2;
            pMutableInfo->CurrentReservation =
                std::min(pInfo->CurrentReservation, pMutableInfo->AvailableForReservation);

            HookLog("D3DKMT: OVERRIDE Shared VRAM - Budget: %llu->%llu MB", originalBudget / (1024 * 1024),
                    g_VramConfig.sharedVramBytes / (1024 * 1024));
        } else {
            // Pass-through mode: log real values for debugging
            static UINT sharedQueryCount = 0;
            if (++sharedQueryCount <= 3) {  // Only log first 3 queries to avoid spam
                HookLog("D3DKMT: Shared VRAM PASSTHROUGH - Budget: %llu MB, Usage: %llu MB",
                        pInfo->Budget / (1024 * 1024), pInfo->CurrentUsage / (1024 * 1024));
            }
        }
    }

    return status;
}

// Hook for D3DKMTQueryAdapterInfo
static NTSTATUS WINAPI Hook_D3DKMTQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO* pInfo) {
    if (!o_D3DKMTQueryAdapterInfo) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    // Call original first
    NTSTATUS status = o_D3DKMTQueryAdapterInfo(pInfo);

    if (!NT_SUCCESS(status) || !pInfo || !pInfo->pPrivateDriverData) {
        return status;
    }

    // Log adapter queries for debugging
    HookLog("D3DKMT: QueryAdapterInfo - Type=%u, Size=%u", pInfo->Type, pInfo->PrivateDriverDataSize);

    // Handle specific query types that report VRAM
    switch (pInfo->Type) {
        case KMTQAITYPE_ADAPTERREGISTRYINFO: {
            // Adapter registry info sometimes contains VRAM info
            // D3DKMT_ADAPTERREGISTRYINFO* pRegInfo =
            // (D3DKMT_ADAPTERREGISTRYINFO*)pInfo->pPrivateDriverData; Could modify
            // reported memory here if needed
            break;
        }
        case KMTQAITYPE_PHYSICALADAPTERDEVICEIDS: {
            // Physical adapter device IDs
            break;
        }
        case KMTQAITYPE_UMDRIVERNAME: {
            // UMD driver name
            break;
        }
        default:
            break;
    }

    return status;
}

// Hook for D3DKMTEnumAdapters
static NTSTATUS WINAPI Hook_D3DKMTEnumAdapters(const D3DKMT_ENUMADAPTERS* pEnumAdapters) {
    if (!o_D3DKMTEnumAdapters) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    NTSTATUS status = o_D3DKMTEnumAdapters(pEnumAdapters);

    if (NT_SUCCESS(status) && pEnumAdapters) {
        HookLog("D3DKMT: EnumAdapters - Count=%u", pEnumAdapters->NumAdapters);

        for (UINT i = 0; i < pEnumAdapters->NumAdapters; i++) {
            HookLog("D3DKMT:   Adapter[%u] - VidPnSourceId=%u, NodeCount=%u", i,
                    pEnumAdapters->Adapters[i].VidPnSourceId, pEnumAdapters->Adapters[i].NodeCount);
        }
    }

    return status;
}

// Hook for D3DKMTEnumAdapters2
static NTSTATUS WINAPI Hook_D3DKMTEnumAdapters2(const D3DKMT_ENUMADAPTERS2* pEnumAdapters) {
    if (!o_D3DKMTEnumAdapters2) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    NTSTATUS status = o_D3DKMTEnumAdapters2(pEnumAdapters);

    if (NT_SUCCESS(status) && pEnumAdapters) {
        HookLog("D3DKMT: EnumAdapters2 - Count=%u", pEnumAdapters->NumAdapters);
    }

    return status;
}

// Install D3DKMT hooks
bool Install() {
    HookLog("D3DKMT: Installing hooks...");

    InitializeConfig();

    // Try win32u.dll first (Windows 10/11)
    HMODULE hWin32U = GetModuleHandleA("win32u.dll");
    HMODULE hGdi32 = GetModuleHandleA("gdi32.dll");

    if (!hWin32U && !hGdi32) {
        HookLog("D3DKMT: Neither win32u.dll nor gdi32.dll loaded");
        return false;
    }

    // Get original function addresses from win32u.dll (preferred)
    if (hWin32U) {
        o_D3DKMTQueryVideoMemoryInfo =
            (decltype(o_D3DKMTQueryVideoMemoryInfo))GetProcAddress(hWin32U, "D3DKMTQueryVideoMemoryInfo");
        o_D3DKMTQueryAdapterInfo =
            (decltype(o_D3DKMTQueryAdapterInfo))GetProcAddress(hWin32U, "D3DKMTQueryAdapterInfo");
        o_D3DKMTEnumAdapters = (decltype(o_D3DKMTEnumAdapters))GetProcAddress(hWin32U, "D3DKMTEnumAdapters");
        o_D3DKMTEnumAdapters2 = (decltype(o_D3DKMTEnumAdapters2))GetProcAddress(hWin32U, "D3DKMTEnumAdapters2");

        HookLog("D3DKMT: Found functions in win32u.dll");
    }

    // Fall back to gdi32.dll if not found in win32u.dll
    if (!o_D3DKMTQueryVideoMemoryInfo && hGdi32) {
        o_D3DKMTQueryVideoMemoryInfo =
            (decltype(o_D3DKMTQueryVideoMemoryInfo))GetProcAddress(hGdi32, "D3DKMTQueryVideoMemoryInfo");
        o_D3DKMTQueryAdapterInfo = (decltype(o_D3DKMTQueryAdapterInfo))GetProcAddress(hGdi32, "D3DKMTQueryAdapterInfo");
        o_D3DKMTEnumAdapters = (decltype(o_D3DKMTEnumAdapters))GetProcAddress(hGdi32, "D3DKMTEnumAdapters");
        o_D3DKMTEnumAdapters2 = (decltype(o_D3DKMTEnumAdapters2))GetProcAddress(hGdi32, "D3DKMTEnumAdapters2");

        HookLog("D3DKMT: Found functions in gdi32.dll");
    }

    if (!o_D3DKMTQueryVideoMemoryInfo) {
        HookLog("D3DKMT: ERROR - Could not find D3DKMTQueryVideoMemoryInfo");
        return false;
    }

    // Install IAT hooks
    bool anyHooked = false;
    void* dummy = nullptr;

    // Hook in all loaded modules
    if (IATHook::PatchIATAllModules("win32u.dll", "D3DKMTQueryVideoMemoryInfo", (void*)Hook_D3DKMTQueryVideoMemoryInfo,
                                    &dummy)) {
        HookLog("D3DKMT: Hooked D3DKMTQueryVideoMemoryInfo in win32u.dll");
        anyHooked = true;
    }
    if (IATHook::PatchIATAllModules("gdi32.dll", "D3DKMTQueryVideoMemoryInfo", (void*)Hook_D3DKMTQueryVideoMemoryInfo,
                                    &dummy)) {
        HookLog("D3DKMT: Hooked D3DKMTQueryVideoMemoryInfo in gdi32.dll");
        anyHooked = true;
    }

    // Register for dynamic loading
    if (o_D3DKMTQueryVideoMemoryInfo) {
        IATHook::RegisterDynamicHook("D3DKMTQueryVideoMemoryInfo", (void*)Hook_D3DKMTQueryVideoMemoryInfo,
                                     (void**)&o_D3DKMTQueryVideoMemoryInfo);
    }

    // Hook other functions (optional but good for completeness)
    if (o_D3DKMTQueryAdapterInfo) {
        IATHook::PatchIATAllModules("win32u.dll", "D3DKMTQueryAdapterInfo", (void*)Hook_D3DKMTQueryAdapterInfo, &dummy);
        IATHook::PatchIATAllModules("gdi32.dll", "D3DKMTQueryAdapterInfo", (void*)Hook_D3DKMTQueryAdapterInfo, &dummy);
        IATHook::RegisterDynamicHook("D3DKMTQueryAdapterInfo", (void*)Hook_D3DKMTQueryAdapterInfo,
                                     (void**)&o_D3DKMTQueryAdapterInfo);
    }

    if (o_D3DKMTEnumAdapters) {
        IATHook::PatchIATAllModules("win32u.dll", "D3DKMTEnumAdapters", (void*)Hook_D3DKMTEnumAdapters, &dummy);
        IATHook::PatchIATAllModules("gdi32.dll", "D3DKMTEnumAdapters", (void*)Hook_D3DKMTEnumAdapters, &dummy);
    }

    if (o_D3DKMTEnumAdapters2) {
        IATHook::PatchIATAllModules("win32u.dll", "D3DKMTEnumAdapters2", (void*)Hook_D3DKMTEnumAdapters2, &dummy);
        IATHook::PatchIATAllModules("gdi32.dll", "D3DKMTEnumAdapters2", (void*)Hook_D3DKMTEnumAdapters2, &dummy);
    }

    HookLog("D3DKMT: Hooks installed - D3DKMTQueryVideoMemoryInfo=%p, enabled=%d", o_D3DKMTQueryVideoMemoryInfo,
            g_VramConfig.enabled);

    return anyHooked || o_D3DKMTQueryVideoMemoryInfo != nullptr;
}

// Set VRAM override values
void SetVramOverride(UINT64 dedicatedBytes, UINT64 sharedBytes) {
    g_VramConfig.enabled = true;
    g_VramConfig.dedicatedVramBytes = dedicatedBytes;
    g_VramConfig.sharedVramBytes = sharedBytes;

    HookLog("D3DKMT: VRAM override updated - Dedicated: %llu MB, Shared: %llu MB", dedicatedBytes / (1024 * 1024),
            sharedBytes / (1024 * 1024));
}

// Disable VRAM override
void DisableVramOverride() {
    g_VramConfig.enabled = false;
    HookLog("D3DKMT: VRAM override disabled");
}

}  // namespace D3DKMTHooks
