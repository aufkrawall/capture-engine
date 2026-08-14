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

// Index (into kTrackedOverlayModules) of the most recently loaded tracked overlay
// module, recorded ONLY from real load notifications (LdrRegisterDllNotification /
// LoadLibrary hooks). The seed walk and the identity-refresh enumeration see
// modules in loader/enumeration order, not load order, so they must not touch
// this. The last-load order decides which overlay owns a foreign Present entry
// jump when two overlays both hook dxgi!Present (the later hooker displaces the
// earlier one's entry jump, e.g. RTSS loading after Steam).
inline std::atomic<int>& LastLoadedTrackedOverlayModuleIndex() {
    static std::atomic<int> index{-1};
    return index;
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

// Loader-free (after the one-time seed): how many tracked modules in `subset` are loaded.
inline size_t CountLoadedTrackedOverlayModules(TrackedOverlaySubset subset) {
    EnsureThirdPartyOverlayModuleCacheSeeded();
    const uint32_t bits = TrackedOverlayLoadedBits().load(std::memory_order_acquire);
    size_t count = 0;
    for (size_t i = 0; i < kTrackedOverlayModuleCount; ++i) {
        if (!(bits & (1u << i))) {
            continue;
        }
        const TrackedOverlayModule& e = kTrackedOverlayModules[i];
        const bool inSubset = (subset == TrackedOverlaySubset::kOverlay && e.overlay) ||
                              (subset == TrackedOverlaySubset::kStartupBlocking && e.startupBlocking) ||
                              (subset == TrackedOverlaySubset::kStartupBlockingRender && e.startupBlockingRender);
        if (inSubset) {
            ++count;
        }
    }
    return count;
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

// Same as NoteModuleLoadedForOverlayCache, but additionally records this module as
// the most recently loaded tracked overlay. Use ONLY from real load notifications
// (LdrRegisterDllNotification callback and the LoadLibrary* hooks); the identity
// refresh walk enumerates modules in a non-chronological order and must keep using
// the plain variant.
inline const char* NoteModuleLoadedForOverlayCacheFromNotification(const char* moduleNameOrPath) {
    const char* matched = NoteModuleLoadedForOverlayCache(moduleNameOrPath);
    if (matched) {
        const int idx = MatchKnownThirdPartyOverlayModuleIndex(detail::ExtractBaseName(moduleNameOrPath));
        LastLoadedTrackedOverlayModuleIndex().store(idx, std::memory_order_release);
    }
    return matched;
}

// Loader-free: canonical name of the most recently loaded tracked overlay module,
// or null when no tracked overlay load was observed through the notification path.
inline const char* GetLastLoadedTrackedOverlayModuleName() {
    const int idx = LastLoadedTrackedOverlayModuleIndex().load(std::memory_order_acquire);
    return idx >= 0 ? kTrackedOverlayModules[idx].name : nullptr;
}

// Pure decision for classifying an unresolvable foreign Present chain: the most
// recently loaded tracked overlay owns the preserved entry jump (it displaced the
// earlier hooker, e.g. RTSS loading after Steam). RTSS as the last loader means the
// chain is NOT Steam's even when Steam is also loaded and wins the list-priority
// name cache.
inline bool IsSteamExternalChainOwnerByLoadOrderEvidence(const char* lastLoadedOverlayName,
                                                         const char* loadedOverlayName) {
    if (lastLoadedOverlayName && detail::ContainsInsensitive(lastLoadedOverlayName, "rtsshooks")) {
        return false;
    }
    return loadedOverlayName != nullptr &&
           detail::ContainsInsensitive(loadedOverlayName, "gameoverlayrenderer");
}

// Pure decision: may CE add its own entry patch to a dxgi!Present entry that a foreign
// overlay already owns? No — CE stays out of that entry entirely and intercepts BELOW it
// with a deep hook in the function body, past the bytes those tools rewrite. Two independent
// reasons, either of which is sufficient:
//
//  * Chain integrity. Steam and RTSS both implement "save the current entry bytes, patch, and
//    on every call restore the saved bytes / re-install" on that SAME shared entry. Two of them
//    compose naturally. A third participant does not: whichever tool (re-)installs its hook
//    while CE's five-byte prepend is live records CE as its own "next", which silently drops
//    the other overlay out of the chain — Strange Brigade DX12 + Steam + RTSS, sessions
//    20260812_002958 / _005530 / _010529: all three overlays drew on frame 1 and RTSS was gone
//    from frame 2 onwards. No forwarding heuristic can repair that from CE's side, because the
//    damage is inside the other tools' saved-chain state.
//
//  * Draw order. Every participant in that chain composites BEFORE it forwards, so whoever
//    runs last ends up on top. CE's prepend made it the FIRST participant, i.e. the bottom
//    layer: Steam's fullscreen overlay and RTSS's OSD drew over CE's overlay (user report,
//    build 0.1.5959). Below the chain CE composites after all of them and is topmost, which is
//    the project rule for CE's own overlay.
//
// A single foreign overlay does compose with a CE prepend, so there the second reason is the
// one that decides; `MayPrependPresentEntryWhenBelowChainViewUnavailable` allows the prepend
// back as a fallback when the body patch cannot be placed. With two or more it is forbidden
// outright.
//
// The frame-generation interposer (Streamline / NvPresent) used to keep the prepend, because
// the alternative then was wrapper-only interception, which cannot see a runtime present on a
// swapchain CE never created. That is no longer the alternative: the leave-entry branch takes
// a deep body hook, and every present reaches it — including presents from swapchains that
// pre-date injection, which the entry hook itself covers no better. The exception therefore
// has no remaining purpose and would only keep FG games on the bottom layer.
inline bool ShouldLeavePresentEntryToForeignOverlayChain(bool foreignEntryPatchOwnedByOverlay,
                                                         size_t loadedOverlayModuleCount) {
    return foreignEntryPatchOwnedByOverlay && loadedOverlayModuleCount >= 1;
}

// Once CE has a deep Present hook below a foreign overlay chain, wrapping the DX12 swapchain
// adds no coverage: every Present already reaches CE after the foreign chain.
// Returning a proxy object would nevertheless change COM identity and Release traffic above that
// chain. Steam and the other overlays maintain per-swapchain state there, so preserve the real
// object just as CE preserves the entry bytes. This applies with one overlay too: the draw-order
// reason already requires the same below-chain view, and Steam alone can retain the proxy's old
// real object long enough to reject a same-HWND FSR replacement. Non-DX12 swapchains and the
// wrapper-only fallback remain unchanged.
inline bool ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(bool d3d12CommandQueueSwapchain,
                                                                        bool interceptedBelowForeignChain,
                                                                        size_t loadedOverlayModuleCount) {
    return d3d12CommandQueueSwapchain && interceptedBelowForeignChain && loadedOverlayModuleCount >= 1;
}

// A hidden-window DX12 create deliberately skips Present-hook refresh and all authoritative
// swapchain side effects because it may be an auxiliary chain. A retaining CE proxy is itself a
// side effect, though, and can leave a foreign overlay holding the hidden chain after the app
// releases it. Preserve COM identity when a known overlay is already loaded; the later global
// Present-body installation covers a main window once it becomes visible, while a genuine helper
// remains completely untouched.
inline bool ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(
    bool d3d12CommandQueueSwapchain, bool hasOutputWindow, bool outputWindowVisible,
    size_t loadedOverlayModuleCount) {
    return d3d12CommandQueueSwapchain && hasOutputWindow && !outputWindowVisible &&
           loadedOverlayModuleCount >= 1;
}

// Pure decision: the below-the-chain body patch was refused (thread quiescence, an
// unrecognized prolog, a 32-bit target the deep-hook policy rejects). May CE fall back to the
// entry prepend it used before the below-the-chain mode existed?
//
// Only against a SINGLE foreign overlay. There the prepend is the historical, validated
// topology and costs nothing but draw order, whereas having no Present view at all costs the
// overlay entirely. With two or more overlays on the entry the prepend is what corrupts their
// saved chains, so that case keeps the leave-entry state and retries the body patch on the
// next real swapchain event instead.
inline bool MayPrependPresentEntryWhenBelowChainViewUnavailable(size_t loadedOverlayModuleCount) {
    return loadedOverlayModuleCount < 2;
}

// Post-wrap transition decision: after CE wrapped the FG runtime's swapchain (non-entry view of
// runtime presents established), may CE now remove its own Present-entry prepend and switch to
// wrapper-only interception? Only when CE still owns the entry bytes — a foreign re-hook that
// already took the entry recorded CE's relay inside its own saved chain, and un-prepending then
// cannot repair that state (CE must not touch bytes it no longer owns).
inline bool ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(bool entryPatchStillIntact,
                                                             bool foreignEntryPatchOwnedByOverlay,
                                                             size_t loadedOverlayModuleCount,
                                                             bool alreadyLeftToForeignChain) {
    if (alreadyLeftToForeignChain || !entryPatchStillIntact) {
        return false;
    }
    return ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryPatchOwnedByOverlay,
                                                        loadedOverlayModuleCount);
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
    LastLoadedTrackedOverlayModuleIndex().store(-1, std::memory_order_release);
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
