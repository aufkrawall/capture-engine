#pragma once

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>

#include "module_table.h"

// Module matching plus the hook-install-order and draw-routing decisions.

namespace ce::overlay_compat {

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

inline void SetIdentifiedOverlayIdentityLoaded(const char* canonicalIdentity, bool loaded) {
    const int idx = MatchKnownThirdPartyOverlayModuleIndex(canonicalIdentity);
    if (idx < 0)
        return;
    if (loaded)
        TrackedOverlayLoadedBits().fetch_or(1u << idx, std::memory_order_acq_rel);
    else
        TrackedOverlayLoadedBits().fetch_and(~(1u << idx), std::memory_order_acq_rel);
}

// Test-only: clear all detection state and mark seeded so queries do not trigger a real loader
// walk (tests fully control the loaded-set via NoteModuleLoaded/Unloaded).
inline void ResetThirdPartyOverlayModuleCacheForTesting() {
    TrackedOverlayLoadedBits().store(0, std::memory_order_release);
    TrackedOverlaySeeded().store(true, std::memory_order_release);
    ResetIdentifiedThirdPartyOverlayModulePaths();
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

// Loader-free (cached): is a specific tracked module currently loaded? Lazy-seeds once.
inline bool IsTrackedModuleLoaded(const char* name) {
    EnsureThirdPartyOverlayModuleCacheSeeded();
    const int idx = MatchKnownThirdPartyOverlayModuleIndex(name);
    if (idx < 0) {
        return false;
    }
    return (TrackedOverlayLoadedBits().load(std::memory_order_acquire) & (1u << idx)) != 0;
}

// Loader-free (cached) Streamline-interposer presence check. Replaces per-Present
// GetModuleHandleA("sl.interposer.dll"), which stalls in the loader during the Alt+Tab
// mode-switch DLL churn.
inline bool IsStreamlineInterposerModuleLoaded() {
    return IsTrackedModuleLoaded("sl.interposer.dll");
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
    //
    // DO NOT naively enable this for plain non-FG to offload the overlay's per-frame
    // ExecuteCommandLists off the app's shared queue: the dedicated queue's overlay command list
    // draws DIRECTLY to the swapchain backbuffer, and DXGI forbids a non-owning queue from
    // touching the swapchain backbuffers — it returns DXGI_ERROR_ACCESS_DENIED (0x887A002B) and
    // removes the device on the FIRST submit (proven at startup in logs/20260606_153428: "DEVICE
    // REMOVED after reinit submit #1", queue=dedicated, offscreen=0). Only the queue passed to
    // CreateSwapChainForHwnd may render the backbuffer. A dedicated queue can therefore only do
    // OFFSCREEN overlay work; the composite onto the backbuffer must still happen on the app's
    // queue. (The FG path that uses the dedicated queue does not direct-draw the backbuffer.)
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
