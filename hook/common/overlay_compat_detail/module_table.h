#pragma once

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>

// Tracked third-party overlay module table and loader-free detection state.

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
// NOT-loaded name (socialclub.dll, discord_hook*, ...) GetModuleHandleA takes the
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
    // Streamline interposer — NOT an overlay (no subset). Tracked only so SL-loaded checks on
    // the Present hot path can be answered from the cache instead of a per-Present
    // GetModuleHandleA("sl.interposer.dll") that stalls in the loader during the mode switch.
    {"sl.interposer.dll", false, false, false},
};
inline constexpr size_t kTrackedOverlayModuleCount = sizeof(kTrackedOverlayModules) / sizeof(kTrackedOverlayModules[0]);
static_assert(kTrackedOverlayModuleCount <= 32, "tracked-module loaded-set is a 32-bit mask");

}  // namespace ce::overlay_compat
