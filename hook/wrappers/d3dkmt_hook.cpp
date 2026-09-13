/**
 * D3DKMT (Display Driver Kernel Mode) Hook Implementation
 *
 * This hooks the kernel-mode driver interface that games use to query VRAM
 * independently of DXGI. This is a universal VRAM-reporting override technique.
 */

#include <windows.h>
#include <algorithm>
#include "../common/hook_common.h"
#include "../common/logging.h"
#include "d3dkmt_abi.h"
#include "iat_hook.h"

// NTSTATUS definitions
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef STATUS_PROCEDURE_NOT_FOUND
#define STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007A)
#endif

// The D3DKMT structures these hooks read and write are mirrored, with their
// offsets pinned, in d3dkmt_abi.h. Do not re-declare them here.
using D3DKMT_QUERYVIDEOMEMORYINFO = ce::d3dkmt::QueryVideoMemoryInfo;
using D3DKMT_QUERYADAPTERINFO = ce::d3dkmt::QueryAdapterInfo;
using D3DKMT_ENUMADAPTERS = ce::d3dkmt::EnumAdapters;
using D3DKMT_ENUMADAPTERS2 = ce::d3dkmt::EnumAdapters2;

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
    HookLog(
        "D3DKMT: VRAM override disabled - passing through real values "
        "(SpecialK-style)");
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

    // Log the query for debugging. hProcess is a process HANDLE, not a PID, and
    // a null handle means "this process" - resolve it to a PID either way so the
    // line names one identity.
    HookLog("D3DKMT: QueryVideoMemoryInfo - Process=%lu, Segment=%s, PhysicalAdapterIndex=%u",
            static_cast<unsigned long>(pInfo->hProcess ? GetProcessId(pInfo->hProcess) : GetCurrentProcessId()),
            pInfo->MemorySegmentGroup == ce::d3dkmt::kSegmentGroupLocal ? "LOCAL" : "NON_LOCAL",
            pInfo->PhysicalAdapterIndex);

    // Cast away const to modify the output struct (if needed)
    D3DKMT_QUERYVIDEOMEMORYINFO* pMutableInfo = const_cast<D3DKMT_QUERYVIDEOMEMORYINFO*>(pInfo);

    if (pInfo->MemorySegmentGroup == ce::d3dkmt::kSegmentGroupLocal) {
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

    // Log adapter queries for debugging. This hook applies no override of its
    // own - every query is answered by the driver and passed straight back.
    HookLog("D3DKMT: QueryAdapterInfo - Type=%u, Size=%u", static_cast<unsigned>(pInfo->Type),
            static_cast<unsigned>(pInfo->PrivateDriverDataSize));

    return status;
}

// Hook for D3DKMTEnumAdapters
static NTSTATUS WINAPI Hook_D3DKMTEnumAdapters(const D3DKMT_ENUMADAPTERS* pEnumAdapters) {
    if (!o_D3DKMTEnumAdapters) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    NTSTATUS status = o_D3DKMTEnumAdapters(pEnumAdapters);

    if (NT_SUCCESS(status) && pEnumAdapters) {
        HookLog("D3DKMT: EnumAdapters - Count=%lu", static_cast<unsigned long>(pEnumAdapters->NumAdapters));

        // The array is MAX_ENUM_ADAPTERS entries; never read past it on a count
        // the driver reported, however it got there.
        const ULONG logged =
            std::min<ULONG>(pEnumAdapters->NumAdapters, static_cast<ULONG>(ce::d3dkmt::kMaxEnumAdapters));
        for (ULONG i = 0; i < logged; i++) {
            HookLog("D3DKMT:   Adapter[%lu] - Luid=%08lx:%08lx, NumOfSources=%lu, PrecisePresentRegions=%d",
                    static_cast<unsigned long>(i),
                    static_cast<unsigned long>(pEnumAdapters->Adapters[i].AdapterLuid.HighPart),
                    static_cast<unsigned long>(pEnumAdapters->Adapters[i].AdapterLuid.LowPart),
                    static_cast<unsigned long>(pEnumAdapters->Adapters[i].NumOfSources),
                    pEnumAdapters->Adapters[i].bPrecisePresentRegionsPreferred ? 1 : 0);
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
        HookLog("D3DKMT: EnumAdapters2 - Count=%lu", static_cast<unsigned long>(pEnumAdapters->NumAdapters));
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
