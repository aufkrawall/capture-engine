#pragma once

#include <windows.h>

#include <cctype>
#include <cstddef>
#include <cwctype>

namespace ce::overlay_compat {
namespace detail {

inline char ToLowerAscii(char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

inline wchar_t ToLowerAscii(wchar_t ch) {
    return static_cast<wchar_t>(std::towlower(ch));
}

template <typename CharT>
inline bool ContainsInsensitive(const CharT* haystack, const CharT* needle) {
    if (!haystack || !needle || !*haystack || !*needle) {
        return false;
    }

    for (const CharT* cursor = haystack; *cursor; ++cursor) {
        const CharT* lhs = cursor;
        const CharT* rhs = needle;
        while (*lhs && *rhs && ToLowerAscii(*lhs) == ToLowerAscii(*rhs)) {
            ++lhs;
            ++rhs;
        }
        if (!*rhs) {
            return true;
        }
    }

    return false;
}

struct AuxiliaryProcessWindowSearchContext;
BOOL CALLBACK FindAuxiliaryProcessWindowProc(HWND hwnd, LPARAM lParam);

}  // namespace detail

struct AuxiliaryProcessWindowInfo {
    HWND hwnd = nullptr;
    DWORD threadId = 0;
    bool visible = false;
    char className[64] = {};
    char title[256] = {};
};

namespace detail {

struct AuxiliaryProcessWindowSearchContext {
    DWORD processId = 0;
    HWND primaryWindow = nullptr;
    AuxiliaryProcessWindowInfo* info = nullptr;
};

inline BOOL CALLBACK FindAuxiliaryProcessWindowProc(HWND hwnd, LPARAM lParam) {
    auto* context = reinterpret_cast<AuxiliaryProcessWindowSearchContext*>(lParam);
    if (!context) {
        return TRUE;
    }

    DWORD windowProcessId = 0;
    const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId != context->processId) {
        return TRUE;
    }
    if (context->primaryWindow && hwnd == context->primaryWindow) {
        return TRUE;
    }
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (context->info) {
        context->info->hwnd = hwnd;
        context->info->threadId = windowThreadId;
        context->info->visible = true;
        GetClassNameA(hwnd, context->info->className, static_cast<int>(sizeof(context->info->className)));
        GetWindowTextA(hwnd, context->info->title, static_cast<int>(sizeof(context->info->title)));
    }
    return FALSE;
}

}  // namespace detail

inline bool IsThirdPartyOverlayModulePath(const char* path) {
    static constexpr const char* kOverlayTokens[] = {
        "gameoverlayrenderer",   "discord_hook", "socialclub", "eosovh",    "eossdk_win64_shipping",
        "eossdk-win64-shipping", "nvspcap",      "nvoverlay",  "rtsshooks", "specialk",
    };

    for (const char* token : kOverlayTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsThirdPartyOverlayModulePath(const wchar_t* path) {
    static constexpr const wchar_t* kOverlayTokens[] = {
        L"gameoverlayrenderer",   L"discord_hook", L"socialclub", L"eosovh",    L"eossdk_win64_shipping",
        L"eossdk-win64-shipping", L"nvspcap",      L"nvoverlay",  L"rtsshooks", L"specialk",
    };

    for (const wchar_t* token : kOverlayTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsStartupBlockingOverlayModulePath(const char* path) {
    static constexpr const char* kStartupBlockingOverlayTokens[] = {
        "socialclub",
        "eosovh",
        "eossdk_win64_shipping",
        "eossdk-win64-shipping",
    };

    for (const char* token : kStartupBlockingOverlayTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsStartupBlockingOverlayModulePath(const wchar_t* path) {
    static constexpr const wchar_t* kStartupBlockingOverlayTokens[] = {
        L"socialclub",
        L"eosovh",
        L"eossdk_win64_shipping",
        L"eossdk-win64-shipping",
    };

    for (const wchar_t* token : kStartupBlockingOverlayTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsFFXFrameGenerationModulePath(const char* path) {
    static constexpr const char* kFFXFrameGenerationTokens[] = {
        "amd_fidelityfx_framegeneration_dx12",
        "amd_fidelityfx_framegeneration_vk",
        // GTA V Enhanced currently uses the generic runtime DLL name while
        // still routing native FSR FG through the FFX API exports.
        "amd_fidelityfx_dx12",
        "amd_fidelityfx_vk",
        "amd_fidelityfx_fg",
        "ffx_frameinterpolation",
        "ffx_framegeneration",
        "fsr3fg",
        "fsr3mod",
    };

    for (const char* token : kFFXFrameGenerationTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsFFXFrameGenerationModulePath(const wchar_t* path) {
    static constexpr const wchar_t* kFFXFrameGenerationTokens[] = {
        L"amd_fidelityfx_framegeneration_dx12",
        L"amd_fidelityfx_framegeneration_vk",
        L"amd_fidelityfx_dx12",
        L"amd_fidelityfx_vk",
        L"amd_fidelityfx_fg",
        L"ffx_frameinterpolation",
        L"ffx_framegeneration",
        L"fsr3fg",
        L"fsr3mod",
    };

    for (const wchar_t* token : kFFXFrameGenerationTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline const char* GetEffectiveCreateSwapchainCallerModulePath(const char* forwardedCallerModulePath,
                                                               const char* immediateCallerModulePath) {
    if (forwardedCallerModulePath && *forwardedCallerModulePath) {
        return forwardedCallerModulePath;
    }

    return immediateCallerModulePath;
}

inline bool IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(const char* forwardedCallerModulePath,
                                                                  const char* immediateCallerModulePath) {
    return IsThirdPartyOverlayModulePath(
        GetEffectiveCreateSwapchainCallerModulePath(forwardedCallerModulePath, immediateCallerModulePath));
}

inline bool IsThirdPartyOverlayLoaded() {
    static constexpr const char* kOverlayModules[] = {
        "gameoverlayrenderer64.dll",
        "gameoverlayrenderer.dll",
        "discord_hook64.dll",
        "discord_hook.dll",
        "socialclub.dll",
        "socialclub",
        "EOSOVH_Win64_Shipping.dll",
        "EOSOVH_Win64_Shipping",
        "EOSOVH_Win32_Shipping.dll",
        "EOSOVH_Win32_Shipping",
        "nvspcap64.dll",
        "nvspcap.dll",
        "nvoverlay.dll",
        "RTSSHooks64.dll",
        "RTSSHooks.dll",
        "RTSSHooks",
    };

    for (const char* moduleName : kOverlayModules) {
        if (GetModuleHandleA(moduleName) != nullptr) {
            return true;
        }
    }

    return false;
}

inline const char* GetLoadedThirdPartyOverlayModuleName() {
    static constexpr const char* kOverlayModules[] = {
        "gameoverlayrenderer64.dll",
        "gameoverlayrenderer.dll",
        "discord_hook64.dll",
        "discord_hook.dll",
        "socialclub.dll",
        "socialclub",
        "EOSOVH_Win64_Shipping.dll",
        "EOSOVH_Win64_Shipping",
        "EOSOVH_Win32_Shipping.dll",
        "EOSOVH_Win32_Shipping",
        "nvspcap64.dll",
        "nvspcap.dll",
        "nvoverlay.dll",
        "RTSSHooks64.dll",
        "RTSSHooks.dll",
        "RTSSHooks",
    };

    for (const char* moduleName : kOverlayModules) {
        if (GetModuleHandleA(moduleName) != nullptr) {
            return moduleName;
        }
    }

    return nullptr;
}

inline const char* GetStartupBlockingOverlayModuleName() {
    static constexpr const char* kBlockingOverlayModules[] = {
        "socialclub.dll",
        "socialclub",
        "EOSOVH_Win64_Shipping.dll",
        "EOSOVH_Win64_Shipping",
        "EOSOVH_Win32_Shipping.dll",
        "EOSOVH_Win32_Shipping",
    };

    for (const char* moduleName : kBlockingOverlayModules) {
        if (GetModuleHandleA(moduleName) != nullptr) {
            return moduleName;
        }
    }

    return nullptr;
}

inline const char* GetStartupBlockingOverlayRenderModuleName() {
    static constexpr const char* kBlockingOverlayRenderModules[] = {
        "SocialClubD3D12Renderer.dll", "SocialClubD3D12Renderer",   "EOSOVH_Win64_Shipping.dll",
        "EOSOVH_Win64_Shipping",       "EOSOVH_Win32_Shipping.dll", "EOSOVH_Win32_Shipping",
    };

    for (const char* moduleName : kBlockingOverlayRenderModules) {
        if (GetModuleHandleA(moduleName) != nullptr) {
            return moduleName;
        }
    }

    return nullptr;
}

inline bool ShouldPreemptivelyDelayDX12OverlayInitForProcess(const char* processName) {
    (void)processName;
    return false;
}

inline bool HasUsableDX12OverlayStartupWindowSize(LONG width, LONG height) {
    return width >= 640 && height >= 360;
}

inline bool ShouldDelayDX12OverlayAfterStartupResume(bool processNeedsDelay, bool hadStartupSuppression,
                                                     bool actualFGActive, bool runtimeOwnedSwapchainNeedsExtraSettle,
                                                     bool windowForeground, LONG width, LONG height,
                                                     ULONGLONG msSinceResumeReady, ULONGLONG settleDelayMs) {
    // GTA 5 Enhanced can briefly return from the Social Club startup window while
    // the live swapchain is still in the middle of a non-game runtime queue
    // handoff. Even with the correct queue captured, drawing during that short
    // window can still trigger ERR_GFX_STATE, so keep overlay work deferred
    // until the handoff settles and the main window has been stable again for
    // long enough.
    return processNeedsDelay && hadStartupSuppression && !actualFGActive &&
           (runtimeOwnedSwapchainNeedsExtraSettle || !windowForeground ||
            !HasUsableDX12OverlayStartupWindowSize(width, height) || msSinceResumeReady < settleDelayMs);
}

inline bool ShouldDelayDX12OverlayInitAfterStartupResume(bool processNeedsDelay, bool hadStartupSuppression,
                                                         bool actualFGActive, bool windowForeground, LONG width,
                                                         LONG height, ULONGLONG msSinceResumeReady,
                                                         ULONGLONG settleDelayMs) {
    return ShouldDelayDX12OverlayAfterStartupResume(processNeedsDelay, hadStartupSuppression, actualFGActive, false,
                                                    windowForeground, width, height, msSinceResumeReady,
                                                    settleDelayMs);
}

inline size_t GetStartupCompatibleDX12AllocatorPoolSize(bool processNeedsDelay, bool startupOverlayPresent,
                                                        bool actualFGActive, bool startupCompatSettled,
                                                        size_t defaultPoolSize) {
    if (processNeedsDelay && startupOverlayPresent && !actualFGActive && !startupCompatSettled) {
        // Use a reduced pool only during the actual startup compatibility
        // window. Keeping the reduced pool after startup has already settled can
        // starve later FG recovery paths under heavy GPU load.
        return 3;
    }
    return defaultPoolSize;
}

inline bool ShouldDelayDX12OverlayRenderAfterSyncInit(bool processNeedsDelay, bool actualFGActive,
                                                      ULONGLONG msSinceSyncInit, ULONGLONG settleDelayMs) {
    return processNeedsDelay && !actualFGActive && msSinceSyncInit < settleDelayMs;
}

inline bool ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(bool processNeedsDelay, bool actualFGActive,
                                                                   const char* startupBlockingOverlayModule,
                                                                   ULONGLONG msSinceSyncInit,
                                                                   ULONGLONG maxSuppressionMs) {
    return processNeedsDelay && !actualFGActive && startupBlockingOverlayModule != nullptr &&
           msSinceSyncInit < maxSuppressionMs;
}

inline bool HasRecentDX12StartupBlockingRenderActivity(ULONGLONG lastActivityMs, ULONGLONG now,
                                                       ULONGLONG quietPeriodMs) {
    return lastActivityMs != 0 && now >= lastActivityMs && (now - lastActivityMs) < quietPeriodMs;
}

inline bool ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(bool processNeedsDelay,
                                                                             bool actualFGActive,
                                                                             const char* startupBlockingOverlayModule,
                                                                             bool hasRecentBlockingRendererActivity) {
    return processNeedsDelay && !actualFGActive && startupBlockingOverlayModule != nullptr &&
           hasRecentBlockingRendererActivity;
}

inline bool FindAuxiliaryProcessWindow(DWORD processId, HWND primaryWindow,
                                       AuxiliaryProcessWindowInfo* info = nullptr) {
    AuxiliaryProcessWindowInfo scratch = {};
    AuxiliaryProcessWindowInfo* targetInfo = info ? info : &scratch;
    *targetInfo = {};

    detail::AuxiliaryProcessWindowSearchContext context = {};
    context.processId = processId;
    context.primaryWindow = primaryWindow;
    context.info = targetInfo;

    EnumWindows(detail::FindAuxiliaryProcessWindowProc, reinterpret_cast<LPARAM>(&context));
    return targetInfo->hwnd != nullptr;
}

inline bool ShouldUseDedicatedDX12OverlayQueue(bool actualFGActive, bool processNeedsDelay = false,
                                               const char* startupBlockingOverlayModule = nullptr) {
    (void)processNeedsDelay;
    (void)startupBlockingOverlayModule;
    // Only use a dedicated overlay queue when frame generation is active.
    // Previously this also created a dedicated queue for startup compat with
    // third-party overlays (Social Club, EOS), but cross-queue resource state
    // transitions on swapchain backbuffers cause ERR_GFX_STATE in GTA5 Enhanced.
    // Single-queue mode avoids the cross-queue barrier conflict entirely.
    return actualFGActive;
}

inline bool ShouldSuppressDX12OverlayForStartup(bool startupOverlayLoaded, bool actualFGActive,
                                                bool auxiliaryWindowPresent, ULONGLONG msSinceOverlayDetected,
                                                ULONGLONG overlayWarmupMs, ULONGLONG msSinceAuxiliaryWindowSeen,
                                                ULONGLONG quietPeriodMs) {
    return startupOverlayLoaded && !actualFGActive &&
           (msSinceOverlayDetected < overlayWarmupMs || auxiliaryWindowPresent ||
            msSinceAuxiliaryWindowSeen < quietPeriodMs);
}

}  // namespace ce::overlay_compat
