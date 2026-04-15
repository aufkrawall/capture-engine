#pragma once

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "ipc_client.h"

// Legacy globals - still used during gradual migration to HookContext
// New code should prefer HOOK_CTX->member access patterns
extern IPCClient* g_IPC;
extern std::atomic<bool> g_ShuttingDown;
extern std::atomic<bool> g_GraphicsOverridesActive;
struct SharedMemoryLayout;
extern SharedMemoryLayout* g_pSharedMem;

inline bool HookIsShuttingDown() {
    return g_ShuttingDown.load(std::memory_order_acquire);
}

inline void RequestHookShutdown() {
    g_ShuttingDown.store(true, std::memory_order_release);
}

// Forward declare HookContext for accessor
namespace ce {
struct HookContext;
}

class PerformanceMetrics;

// Logging Helper
void HookLog(const char* fmt, ...);
void HookLog(LogLevel level, const char* fmt, ...);
void EarlyLog(const char* fmt, ...);
// Logs to hook_debug.log (respects the debugLogging flag, same as HookLog)
void HookLogImportant(const char* fmt, ...);
void NVNGXLog(const char* fmt, ...);
// Build a session-aware log file path using DiscoveryInfo.logsPath with fallback
// to {moduleDir}\logs. Returns false if the path could not be constructed.
bool BuildLogFilePathForModuleAddress(const void* address, const char* fileName, char* outPath, size_t outPathLen);
void ReportLUID(uint32_t low, uint32_t high);
inline std::atomic<ULONGLONG>& LastLargePresentGapTickStorage() {
    static std::atomic<ULONGLONG> tick{0};
    return tick;
}

inline void MarkLargePresentGap() {
    LastLargePresentGapTickStorage().store(GetTickCount64(), std::memory_order_release);
}

inline bool HasRecentLargePresentGap(uint32_t maxAgeMs) {
    const ULONGLONG lastTick = LastLargePresentGapTickStorage().load(std::memory_order_acquire);
    if (lastTick == 0) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    return now >= lastTick && (now - lastTick) <= maxAgeMs;
}
extern char g_ProcessName[260];
// Debug log independent of IPC

// Returns true once DllMain(DLL_PROCESS_DETACH, lpReserved != NULL) has been called.
// GPU resource cleanup (Unmap, Release, etc.) must be skipped when this returns true
// because other DLLs (e.g. NVIDIA driver) may already be partially torn down.
bool IsProcessTerminating();

// Constants
#define HOOK_LOG_FILE "hook.log"

// Local Config (Loaded by Hook for Per-App Overrides)
#include "../../common/config.h"
extern AppConfig* g_pLocalConfig;

inline bool HookDebugLoggingEnabled() {
    if (g_IPC && g_IPC->GetSharedMem())
        return g_IPC->GetSharedMem()->GetDebugLogging();
#ifndef VK_LAYER_CE_OVERLAY
    if (g_pSharedMem)
        return g_pSharedMem->GetDebugLogging();
    if (g_pLocalConfig)
        return IsDebugLoggingEnabled(g_pLocalConfig->logLevel);
#endif
    return false;
}

inline bool HookTraceLoggingEnabled() {
    if (g_IPC && g_IPC->GetSharedMem())
        return static_cast<int>(g_IPC->GetSharedMem()->GetLogLevel()) >= static_cast<int>(LogLevel::Trace);
#ifndef VK_LAYER_CE_OVERLAY
    if (g_pSharedMem)
        return static_cast<int>(g_pSharedMem->GetLogLevel()) >= static_cast<int>(LogLevel::Trace);
    if (g_pLocalConfig)
        return IsTraceLoggingEnabled(g_pLocalConfig->logLevel);
#endif
    return false;
}

inline OverlayConfig GetHookOverlayConfig() {
    if (g_IPC && g_IPC->GetSharedMem()) {
        return g_IPC->GetSharedMem()->ReadOverlayConfig();
    }
#ifndef VK_LAYER_CE_OVERLAY
    if (g_pSharedMem) {
        return g_pSharedMem->ReadOverlayConfig();
    }
    if (g_pLocalConfig) {
        return g_pLocalConfig->overlay;
    }
#endif
    return OverlayConfig{};
}

inline bool HookOverlayObserverOnlyEnabled() {
    return IsOverlayObserverOnly(GetHookOverlayConfig());
}

inline bool HookOverlayObserverPolicyOnlyEnabled() {
    return IsOverlayObserverPolicyOnly(GetHookOverlayConfig());
}

inline bool HookOverlayObserverStartupPresentOnlyEnabled() {
    return IsOverlayObserverStartupPresentOnly(GetHookOverlayConfig());
}

// DX12 exports this so shared routing code can distinguish a fully confirmed
// PostSL path from the still-fragile post-activation warm-up phase.
bool HookIsPostSLOverlayActiveButUnconfirmed();

// DX12 exports this so shared routing code can keep the pure-DLSS startup
// family off the synthetic/bypass Present path for a few confirmed PostSL
// frames after the very first successful submit.
bool HookIsPostSLOverlayConfirmedButStartupSettling();

// Helper to get active config (Local > IPC)
GraphicsConfig GetActiveGraphicsConfig();
float GetActivePrerenderLimit();

// Helper to apply VSync override (reduces duplication across DX9/11/12)
// Returns the presentation interval value to use
// For DX9: D3DPRESENT_INTERVAL_* constants
// For DX11/12: DXGI_SWAP_EFFECT and sync interval
struct VSyncOverride {
    bool shouldOverride = false;
    int presentInterval = 0;  // DX9: D3DPRESENT_INTERVAL_*, DX11/12: sync interval (0 or 1)
    bool useMailbox = false;  // DX11/12: use DXGI_SWAP_EFFECT_FLIP_DISCARD for mailbox
};
VSyncOverride GetVSyncOverride();
void ProcessVSyncOverride(UINT& SyncInterval, UINT& Flags);

// Redirect all IAT entries for `fromDllBaseName` inside `hTarget` to the
// corresponding exports of `hSystemReplacement`. Used to ensure that a
// freshly-loaded system d3d11.dll uses the system dxgi.dll even when DXVK's
// dxgi.dll was already loaded under the bare name "dxgi.dll" and would
// otherwise be picked up by the Windows import-resolution mechanism.
inline void RedirectModuleImports(HMODULE hTarget, const char* fromDllBaseName, HMODULE hSystemReplacement) {
    auto* pDOS = (PIMAGE_DOS_HEADER)hTarget;
    if (pDOS->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto* pNT = (PIMAGE_NT_HEADERS)((uint8_t*)hTarget + pDOS->e_lfanew);
    auto& importDir = pNT->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress)
        return;

    auto* desc = (PIMAGE_IMPORT_DESCRIPTOR)((uint8_t*)hTarget + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = (const char*)((uint8_t*)hTarget + desc->Name);
        if (_stricmp(name, fromDllBaseName) != 0)
            continue;

        auto* thunk = (PIMAGE_THUNK_DATA)((uint8_t*)hTarget + desc->FirstThunk);
        auto* orig =
            desc->OriginalFirstThunk ? (PIMAGE_THUNK_DATA)((uint8_t*)hTarget + desc->OriginalFirstThunk) : thunk;

        for (; thunk->u1.Function; ++thunk, ++orig) {
            FARPROC replacement = nullptr;
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                replacement = GetProcAddress(hSystemReplacement, (LPCSTR)(orig->u1.Ordinal & 0xFFFF));
            } else {
                auto* info = (PIMAGE_IMPORT_BY_NAME)((uint8_t*)hTarget + orig->u1.AddressOfData);
                replacement = GetProcAddress(hSystemReplacement, (LPCSTR)info->Name);
            }
            if (replacement && (FARPROC)thunk->u1.Function != replacement) {
                DWORD oldProt;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt);
                thunk->u1.Function = (ULONG_PTR)replacement;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProt, &oldProt);
            }
        }
        break;
    }
}
