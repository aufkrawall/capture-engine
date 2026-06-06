#pragma once

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>

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

// Case-insensitive (ASCII) full-string equality. Locale-independent and deterministic so it
// is safe to call under the loader lock (DLL load/unload notifications) and from unit tests.
template <typename CharT>
inline bool EqualsInsensitive(const CharT* a, const CharT* b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (ToLowerAscii(*a) != ToLowerAscii(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;  // both reached the terminator
}

// Returns the base-name portion of a module path/name (after the last '\\' or '/'). Never
// touches the loader; safe under the loader lock.
inline const char* ExtractBaseName(const char* pathOrName) {
    if (!pathOrName) {
        return pathOrName;
    }
    const char* base = pathOrName;
    for (const char* cursor = pathOrName; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            base = cursor + 1;
        }
    }
    return base;
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

// Loader-free: true if any known third-party overlay module is loaded. Defined in terms of
// the cached GetLoadedThirdPartyOverlayModuleName() below (forward-declared here).
inline const char* GetLoadedThirdPartyOverlayModuleName();
inline bool IsThirdPartyOverlayLoaded() {
    return GetLoadedThirdPartyOverlayModuleName() != nullptr;
}

// ---------------------------------------------------------------------------------------
// Third-party overlay detection — LOADER-FREE on the Present hot path.
//
// WHY: GetLoadedThirdPartyOverlayModuleName() is called from DetourPresent on EVERY Present.
// The previous implementation walked the loader (GetModuleHandleA over the list below) and
// only cached the result behind a generation counter that was bumped on *every* DLL load. On
// the Alt+Tab iflip<->composited mode switch the system reloads d3d11.dll (+deps) repeatedly,
// invalidating that cache each time and forcing the next Present to re-walk the loader. For a
// NOT-loaded name (socialclub.dll, discord_hook*, RTSSHooks*, ...) GetModuleHandleA takes the
// expensive LdrGetDllHandleEx + ApiSet-resolution + module-DB-walk path; on x86/WoW64 that
// stalled Present for >2 s and tripped the 2 s GPU TDR (DXGI_ERROR_DEVICE_HUNG), killing the
// overlay. Native x64 ran the same walk in <=7 ms, so the freeze was x86-only. See the freeze
// dump in installed/captureengine/logs/20260605_231633.
//
// FIX: the Present hot path now does ONLY a single atomic load. The set of loaded overlay
// modules is tracked out-of-band — a one-time seed scan at init plus DLL load/unload
// notifications (NotifyHookModuleLoaded + LdrRegisterDllNotification) — none of which run on
// the Present thread. Detection logic is split into pure, unit-testable matchers.
//
// Unified tracked-module table. Each known module carries the subsets it belongs to, so a
// single cached loaded-set serves every detector. The Present hot path then reads ONLY an
// atomic bitmask + a tiny loop — it NEVER calls the Windows loader. (GetModuleHandleA for a
// NOT-loaded module takes the slow LdrGetDllHandleEx + ApiSet-resolution / loader-snap path;
// on x86 that stalled Present for >2 s and tripped the GPU TDR — see freeze dumps
// logs/20260605_231633 (ApiSet) and logs/20260606_021018 (loader-snap) — both inside a
// per-Present GetModuleHandleA from the present path.)
//
// List order is detection priority: the lowest-index loaded entry in a subset is reported,
// matching the previous in-order walks — Steam wins for `overlay`; socialclub before EOSOVH
// for `startupBlocking`; SocialClubD3D12Renderer before EOSOVH for `startupBlockingRender`.
// Downstream consumers only token-match the Steam entry ("gameoverlayrenderer"); everyone else
// checks for non-null or logs the string, so these exact strings preserve behavior.
struct TrackedOverlayModule {
    const char* name;
    bool overlay;                // GetLoadedThirdPartyOverlayModuleName / IsThirdPartyOverlayLoaded
    bool startupBlocking;        // GetStartupBlockingOverlayModuleName
    bool startupBlockingRender;  // GetStartupBlockingOverlayRenderModuleName
};
inline constexpr TrackedOverlayModule kTrackedOverlayModules[] = {
    {"gameoverlayrenderer64.dll", true, false, false},
    {"gameoverlayrenderer.dll", true, false, false},
    {"discord_hook64.dll", true, false, false},
    {"discord_hook.dll", true, false, false},
    {"socialclub.dll", true, true, false},
    {"SocialClubD3D12Renderer.dll", false, false, true},
    {"EOSOVH_Win64_Shipping.dll", true, true, true},
    {"EOSOVH_Win32_Shipping.dll", true, true, true},
    {"nvspcap64.dll", true, false, false},
    {"nvspcap.dll", true, false, false},
    {"nvoverlay.dll", true, false, false},
    {"RTSSHooks64.dll", true, false, false},
    {"RTSSHooks.dll", true, false, false},
};
inline constexpr size_t kTrackedOverlayModuleCount =
    sizeof(kTrackedOverlayModules) / sizeof(kTrackedOverlayModules[0]);
static_assert(kTrackedOverlayModuleCount <= 32, "tracked-module loaded-set is a 32-bit mask");

// Pure / loader-free: index of the tracked module whose base name case-insensitively equals
// `baseName`, or -1. Directly unit-testable (no globals, no loader).
inline int MatchKnownThirdPartyOverlayModuleIndex(const char* baseName) {
    if (!baseName || !*baseName) {
        return -1;
    }
    for (size_t i = 0; i < kTrackedOverlayModuleCount; ++i) {
        if (detail::EqualsInsensitive(baseName, kTrackedOverlayModules[i].name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Pure / loader-free: canonical list entry for `baseName`, or nullptr. This is the exact
// string handed to downstream code, so classifiers like IsSteamOverlayModule keep working.
inline const char* MatchKnownThirdPartyOverlayModuleBaseName(const char* baseName) {
    const int idx = MatchKnownThirdPartyOverlayModuleIndex(baseName);
    return idx >= 0 ? kTrackedOverlayModules[idx].name : nullptr;
}

// Lock-free loaded-set bitmask over kTrackedOverlayModules, mutated only off the Present hot
// path (one-time seed + DLL load/unload notifications). `seeded` guards a one-time lazy loader
// walk so the very first query is correct even if it beats the HookThread seed; afterwards
// every query is loader-free.
inline std::atomic<uint32_t>& TrackedOverlayLoadedBits() {
    static std::atomic<uint32_t> bits{0};
    return bits;
}
inline std::atomic<bool>& TrackedOverlaySeeded() {
    static std::atomic<bool> seeded{false};
    return seeded;
}

// Walks the loader ONCE to seed modules already loaded before our hooks/notification
// installed. Returns the seeded bitmask (for logging/diagnostics). Call once from HookThread;
// also lazy-triggered on first query.
inline uint32_t SeedThirdPartyOverlayModuleCacheFromLoader() {
    uint32_t seeded = 0;
    for (size_t i = 0; i < kTrackedOverlayModuleCount; ++i) {
        if (GetModuleHandleA(kTrackedOverlayModules[i].name) != nullptr) {
            seeded |= (1u << i);
        }
    }
    TrackedOverlayLoadedBits().fetch_or(seeded, std::memory_order_acq_rel);
    TrackedOverlaySeeded().store(true, std::memory_order_release);
    return seeded;
}

inline void EnsureThirdPartyOverlayModuleCacheSeeded() {
    if (!TrackedOverlaySeeded().load(std::memory_order_acquire)) {
        SeedThirdPartyOverlayModuleCacheFromLoader();
    }
}

enum class TrackedOverlaySubset { kOverlay, kStartupBlocking, kStartupBlockingRender };

// Loader-free (after the one-time seed): first loaded tracked module in `subset` by list-order
// priority, or nullptr.
inline const char* FirstLoadedTrackedOverlayModule(TrackedOverlaySubset subset) {
    EnsureThirdPartyOverlayModuleCacheSeeded();
    const uint32_t bits = TrackedOverlayLoadedBits().load(std::memory_order_acquire);
    if (bits == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < kTrackedOverlayModuleCount; ++i) {
        if (!(bits & (1u << i))) {
            continue;
        }
        const TrackedOverlayModule& e = kTrackedOverlayModules[i];
        const bool inSubset = (subset == TrackedOverlaySubset::kOverlay && e.overlay) ||
                              (subset == TrackedOverlaySubset::kStartupBlocking && e.startupBlocking) ||
                              (subset == TrackedOverlaySubset::kStartupBlockingRender && e.startupBlockingRender);
        if (inSubset) {
            return e.name;
        }
    }
    return nullptr;
}

// Present HOT PATH: loader-free. (Forward-declared earlier for IsThirdPartyOverlayLoaded.)
inline const char* GetLoadedThirdPartyOverlayModuleName() {
    return FirstLoadedTrackedOverlayModule(TrackedOverlaySubset::kOverlay);
}

// Off-hot-path. Record that `moduleNameOrPath` just loaded; sets its bit if it is a tracked
// module. No loader calls. Returns the canonical match (for logging) or null.
inline const char* NoteModuleLoadedForOverlayCache(const char* moduleNameOrPath) {
    const int idx = MatchKnownThirdPartyOverlayModuleIndex(detail::ExtractBaseName(moduleNameOrPath));
    if (idx < 0) {
        return nullptr;
    }
    TrackedOverlayLoadedBits().fetch_or(1u << idx, std::memory_order_acq_rel);
    return kTrackedOverlayModules[idx].name;
}

// Off-hot-path. Record that `moduleNameOrPath` just unloaded; clears its bit if tracked. No
// loader calls. Returns the canonical match (for logging) or null.
inline const char* NoteModuleUnloadedForOverlayCache(const char* moduleNameOrPath) {
    const int idx = MatchKnownThirdPartyOverlayModuleIndex(detail::ExtractBaseName(moduleNameOrPath));
    if (idx < 0) {
        return nullptr;
    }
    TrackedOverlayLoadedBits().fetch_and(~(1u << idx), std::memory_order_acq_rel);
    return kTrackedOverlayModules[idx].name;
}

// Test-only: clear all detection state and mark seeded so queries do not trigger a real loader
// walk (tests fully control the loaded-set via NoteModuleLoaded/Unloaded).
inline void ResetThirdPartyOverlayModuleCacheForTesting() {
    TrackedOverlayLoadedBits().store(0, std::memory_order_release);
    TrackedOverlaySeeded().store(true, std::memory_order_release);
}

// Loader-free (cached). Startup-blocking overlay modules (Social Club / EOS) gate early
// temporary-swapchain Present-hook deferral and startup suppression.
inline const char* GetStartupBlockingOverlayModuleName() {
    return FirstLoadedTrackedOverlayModule(TrackedOverlaySubset::kStartupBlocking);
}

// Loader-free (cached). Startup-blocking RENDER modules (Social Club D3D12 renderer / EOS).
inline const char* GetStartupBlockingOverlayRenderModuleName() {
    return FirstLoadedTrackedOverlayModule(TrackedOverlaySubset::kStartupBlockingRender);
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
