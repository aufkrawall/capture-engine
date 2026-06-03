#pragma once

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
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

inline bool IsStreamlineFrameGenerationModulePath(const char* path) {
    static constexpr const char* kStreamlineFrameGenerationTokens[] = {
        "sl.interposer", "sl.common", "sl.dlss", "sl.dlss_g", "nvngx_dlssg", "nvngx_dlss",
    };

    for (const char* token : kStreamlineFrameGenerationTokens) {
        if (detail::ContainsInsensitive(path, token)) {
            return true;
        }
    }
    return false;
}

inline bool IsStreamlineFrameGenerationModulePath(const wchar_t* path) {
    static constexpr const wchar_t* kStreamlineFrameGenerationTokens[] = {
        L"sl.interposer", L"sl.common", L"sl.dlss", L"sl.dlss_g", L"nvngx_dlssg", L"nvngx_dlss",
    };

    for (const wchar_t* token : kStreamlineFrameGenerationTokens) {
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

inline bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut = nullptr) {
    if (moduleOut) {
        *moduleOut = nullptr;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule) ||
        !callerModule) {
        return false;
    }

    if (moduleOut) {
        *moduleOut = callerModule;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        if (GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount)) == 0) {
            modulePathOut[0] = '\0';
            return false;
        }
    }

    return true;
}

inline bool IsFFXFrameGenerationModuleHandle(HMODULE moduleHandle, char* modulePathOut = nullptr,
                                             size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsFFXFrameGenerationModulePath(modulePath);
}

inline bool IsCodeAddressFromFFXFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                      size_t modulePathOutCount = 0) {
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount, &callerModule) ||
        !callerModule) {
        return false;
    }

    return IsFFXFrameGenerationModuleHandle(callerModule, modulePathOut, modulePathOutCount);
}

inline bool HasFFXFrameGenerationModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromFFXFrameGenerationModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
            return true;
        }
    }

    return false;
}

inline bool IsCodeAddressFromStreamlineFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                             size_t modulePathOutCount = 0) {
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount, &callerModule) ||
        !callerModule) {
        return false;
    }

    return IsStreamlineFrameGenerationModulePath(modulePathOut);
}

inline bool HasStreamlineFrameGenerationModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromStreamlineFrameGenerationModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
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

// Generation counter for the third-party-overlay module cache. Bumped whenever a
// DLL is loaded into the process (via NotifyHookModuleLoaded ->
// InvalidateThirdPartyOverlayModuleCache). The set of loaded modules only changes
// on a DLL load, so caching the lookup and re-scanning only on a load event lets
// GetLoadedThirdPartyOverlayModuleName() run on every Present essentially for free.
inline std::atomic<uint32_t>& ThirdPartyOverlayModuleCacheGeneration() {
    static std::atomic<uint32_t> generation{0};
    return generation;
}

inline void InvalidateThirdPartyOverlayModuleCache() {
    ThirdPartyOverlayModuleCacheGeneration().fetch_add(1, std::memory_order_release);
}

// Returns the name of the first loaded known third-party overlay module, or null.
//
// PERF/CORRECTNESS: this is called from DetourPresent on EVERY Present. The naive
// implementation did 16 GetModuleHandleA() loader-lock walks per Present, which the
// x86 (WoW64) freeze dumps showed the present thread stuck in — and which contends
// with DWM's d3d11.dll load during the iflip<->composited Alt+Tab mode switch. The
// loaded-module set only changes on a DLL load, so we cache the result and re-scan
// only when the module-load generation changes. Steady-state Presents now cost one
// atomic load + compare instead of 16 loader-lock walks.
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

    static std::atomic<uint32_t> s_cachedGeneration{0xFFFFFFFFu};
    static std::atomic<const char*> s_cachedResult{nullptr};

    const uint32_t generation = ThirdPartyOverlayModuleCacheGeneration().load(std::memory_order_acquire);
    if (s_cachedGeneration.load(std::memory_order_acquire) == generation) {
        return s_cachedResult.load(std::memory_order_acquire);
    }

    const char* found = nullptr;
    for (const char* moduleName : kOverlayModules) {
        if (GetModuleHandleA(moduleName) != nullptr) {
            found = moduleName;
            break;
        }
    }
    // Benign if two threads race here: the module state is identical so they compute
    // the same result; the cache is only an optimization.
    s_cachedResult.store(found, std::memory_order_release);
    s_cachedGeneration.store(generation, std::memory_order_release);
    return found;
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

inline bool IsSameProcessWindow(HWND window, DWORD expectedProcessId) {
    if (!window || expectedProcessId == 0) {
        return false;
    }

    DWORD windowProcessId = 0;
    if (GetWindowThreadProcessId(window, &windowProcessId) == 0) {
        return false;
    }

    return windowProcessId == expectedProcessId;
}

inline bool IsUsableSameProcessForegroundWindow(HWND foregroundWindow, DWORD expectedProcessId, LONG* width = nullptr,
                                                LONG* height = nullptr) {
    if (!IsSameProcessWindow(foregroundWindow, expectedProcessId)) {
        return false;
    }

    RECT clientRect = {};
    LONG localWidth = 0;
    LONG localHeight = 0;
    if (GetClientRect(foregroundWindow, &clientRect)) {
        localWidth = clientRect.right - clientRect.left;
        localHeight = clientRect.bottom - clientRect.top;
    }

    if (width) {
        *width = localWidth;
    }
    if (height) {
        *height = localHeight;
    }

    return HasUsableDX12OverlayStartupWindowSize(localWidth, localHeight);
}

inline bool ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
    bool exactWindowForeground, bool usableSameProcessForegroundWindow, LONG gameWidth, LONG gameHeight,
    LONG foregroundWidth, LONG foregroundHeight, LONG* resolvedWidth = nullptr, LONG* resolvedHeight = nullptr,
    bool* usingSameProcessForegroundWindow = nullptr) {
    if (resolvedWidth) {
        *resolvedWidth = gameWidth;
    }
    if (resolvedHeight) {
        *resolvedHeight = gameHeight;
    }
    if (usingSameProcessForegroundWindow) {
        *usingSameProcessForegroundWindow = false;
    }

    if (exactWindowForeground) {
        return HasUsableDX12OverlayStartupWindowSize(gameWidth, gameHeight);
    }

    if (!usableSameProcessForegroundWindow) {
        // The swapchain window is not the foreground window and there is no
        // usable same-process foreground window (e.g. the controller's
        // pseudo-overlay window is foreground but is zero-sized or belongs to
        // a different process).  As long as the game window itself is still a
        // usable size we can track it; the game is actively rendering and the
        // only reason it is not foreground is an external window steal.
        return HasUsableDX12OverlayStartupWindowSize(gameWidth, gameHeight);
    }

    if (!HasUsableDX12OverlayStartupWindowSize(foregroundWidth, foregroundHeight)) {
        return false;
    }

    if (resolvedWidth) {
        *resolvedWidth = foregroundWidth;
    }
    if (resolvedHeight) {
        *resolvedHeight = foregroundHeight;
    }
    if (usingSameProcessForegroundWindow) {
        *usingSameProcessForegroundWindow = true;
    }

    return true;
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
                                                    windowForeground, width, height, msSinceResumeReady, settleDelayMs);
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
                                                      ULONGLONG msSinceSyncInit, ULONGLONG settleDelayMs,
                                                      bool overlayBackendReady = false) {
    return processNeedsDelay && !actualFGActive && !overlayBackendReady && msSinceSyncInit < settleDelayMs;
}

inline bool ShouldSuppressDX12OverlayRenderForLoadedStartupOverlay(bool processNeedsDelay, bool actualFGActive,
                                                                   const char* startupBlockingOverlayModule,
                                                                   ULONGLONG msSinceSyncInit,
                                                                   ULONGLONG maxSuppressionMs,
                                                                   bool overlayBackendReady = false) {
    return processNeedsDelay && !actualFGActive && !overlayBackendReady && startupBlockingOverlayModule != nullptr &&
           msSinceSyncInit < maxSuppressionMs;
}

inline bool HasRecentDX12StartupBlockingRenderActivity(ULONGLONG lastActivityMs, ULONGLONG now,
                                                       ULONGLONG quietPeriodMs) {
    return lastActivityMs != 0 && now >= lastActivityMs && (now - lastActivityMs) < quietPeriodMs;
}

inline bool ShouldSuppressDX12OverlayRenderForRecentBlockingRendererActivity(bool processNeedsDelay,
                                                                             bool actualFGActive,
                                                                             const char* startupBlockingOverlayModule,
                                                                             bool hasRecentBlockingRendererActivity,
                                                                             bool overlayBackendReady = false) {
    return processNeedsDelay && !actualFGActive && !overlayBackendReady && startupBlockingOverlayModule != nullptr &&
           hasRecentBlockingRendererActivity;
}

inline bool ShouldKeepDX12OverlayVisibleDuringStartupSuppression(bool overlayBackendReady) {
    // Once CE has a usable DX12 backend, a third-party startup window is not a
    // reason to blank the overlay. Keep rendering on the already-valid path;
    // truly invalid swapchain states are filtered separately.
    return overlayBackendReady;
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
