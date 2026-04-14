#pragma once
#include <dxgi1_4.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "fg_runtime_state.h"

// Forward declaration
class PerformanceMetrics;

// Post-SL overlay rendering callback.  Invoked during re-entrant Present
// (after Streamline's FG pipeline finishes) so the overlay renders AFTER FG
// interpolation — matching RTSS's approach for FG compatibility.
using PostSLOverlayRenderFn = void (*)(IDXGISwapChain* pSwapChain);

namespace DXGIShared {

enum class APIType {
    Unknown,
    D3D10,
    D3D11,
    D3D12,
    Vulkan  // For WSI-DXGI interop
};

// Some runtimes expose lower-version compatibility interfaces on higher-version
// swapchains (for example DX11-on-DXVK can answer ID3D10 queries). Always prefer
// the highest actual device API so DX11 swapchains do not fall back to DX10 code
// paths just because compatibility interfaces are present.
inline APIType SelectPrimarySwapChainAPIType(bool hasD3D12Device, bool hasD3D11Device, bool hasD3D10Device) {
    if (hasD3D12Device) {
        return APIType::D3D12;
    }
    if (hasD3D11Device) {
        return APIType::D3D11;
    }
    if (hasD3D10Device) {
        return APIType::D3D10;
    }
    return APIType::Unknown;
}

struct SharedState {
    std::atomic<bool> swapchainInvalid{false};
    std::atomic<bool> fsr4RecreationPending{false};
    std::atomic<int> wrapperResizeDepth{0};
    std::atomic<uint32_t> presentInFlightDepth{0};
    std::atomic<uint64_t> frameCount{0};
    std::atomic<bool> deviceRemovedFatal{false};
    std::atomic<uint64_t> presentCallCount{0};
    std::chrono::steady_clock::time_point lastSwapchainCreation;
    std::atomic<bool> inPresentHook{false};
    std::atomic<bool> fgRuntimeOwnsSwapchain{false};
    std::atomic<bool> streamlineStartupHandoffPending{false};
    std::atomic<bool> streamlineStartupTopLevelPresentConsumed{false};
    std::atomic<ULONGLONG> streamlineStartupTransitionUntilMs{0};
    std::atomic<bool> postSLSyntheticStartupActivationPending{false};
};

extern SharedState g_SharedState;
extern std::mutex g_SharedMutex;

// Callback for post-SL FG overlay rendering (set by dx12_hook.cpp).
extern std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback;

// Direct Streamline FG active signal — set by streamline_hook.cpp when
// slDLSSGSetOptions transitions FG on/off.  More immediate than heuristic
// FG type detection.  Used by DX12 hook for pre-SL vs post-SL routing.
extern std::atomic<bool> g_StreamlineFGRunning;

// Present/Present1 call counter for bypass detection by SL hook.
extern std::atomic<uint64_t> g_PresentCallCounter;

// Initialization
void Init();

// The unified hook installer
bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly = false);

inline bool DoesFGRuntimeOwnSwapchain() {
    return g_SharedState.fgRuntimeOwnsSwapchain.load(std::memory_order_acquire);
}

inline bool IsStreamlineStartupHandoffPending() {
    return g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
}

static constexpr ULONGLONG kStreamlineStartupTransitionGraceMs = 1500;

inline void ArmStreamlineStartupTransitionWindow(ULONGLONG durationMs = kStreamlineStartupTransitionGraceMs) {
    g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    g_SharedState.streamlineStartupTransitionUntilMs.store(GetTickCount64() + durationMs, std::memory_order_release);
}

inline void ExtendStreamlineStartupTransitionWindow(ULONGLONG durationMs = kStreamlineStartupTransitionGraceMs) {
    const ULONGLONG extendedUntilMs = GetTickCount64() + durationMs;
    ULONGLONG currentUntilMs =
        g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);

    while (currentUntilMs < extendedUntilMs &&
           !g_SharedState.streamlineStartupTransitionUntilMs.compare_exchange_weak(
               currentUntilMs, extendedUntilMs, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

inline void ClearStreamlineStartupTransitionWindow() {
    g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    g_SharedState.streamlineStartupTransitionUntilMs.store(0, std::memory_order_release);
}

inline bool IsStreamlineStartupTransitionWindowActive() {
    const ULONGLONG untilMs = g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
    return untilMs != 0 && GetTickCount64() < untilMs;
}

// Set pending swapchain for lazy hook installation (called from DX12 hook)
void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain);

// Verify and re-install vtable hooks if they were overwritten by
// third-party software (e.g. Streamline during FG re-activation).
void RepairVTableHooksIfNeeded();

// Common helpers
bool IsVulkanPrimary();
PerformanceMetrics* GetPerformanceMetrics();
uint32_t GetLatestSourceFrameIndex();
void SetLatestSourceFrameIndex(uint32_t frameIndex);

// Exported handlers for specific APIs (implemented in their respective hook
// files)
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ResizeBegin();
void HandleDX12ResizeEnd();
void HandleDX11ResizeBegin();

// Remove Present/Present1 vtable hooks (called when COM wrapper takes over)
void RemovePresentHooks();

// Disable SL Present routing so Present calls go through the trampoline
// directly instead of through SL's hook chain. Called when FSR FG takes over
// to avoid SL/FSR Present chain conflicts.
void DisableSLPresentRouting();

// Remove all swapchain vtable hooks (Present, Present1, ResizeBuffers,
// ResizeBuffers1)
void RemoveSwapchainVTableHooks();

// Install inline hooks on Present/Present1 (instead of vtable hooks)
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook - preventing re-entry issues with wrapped swapchains
bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain);
bool HasPresentInlineHooks();
bool HasPresentDetourHooks();

inline bool ShouldInstallSwapchainHooksWithThirdPartyOverlay(bool thirdPartyOverlayLoaded, bool hasPresentDetourHooks) {
    return !thirdPartyOverlayLoaded || hasPresentDetourHooks;
}

inline bool ShouldRefreshLivePresentHooksForSwapchainPath(bool hasReadableVtable,
                                                          bool trackedVtableMatchesCurrent,
                                                          bool presentHookInstalled,
                                                          bool present1HookInstalled) {
    if (!hasReadableVtable) {
        return false;
    }

    return !trackedVtableMatchesCurrent || !presentHookInstalled || !present1HookInstalled;
}

inline bool ShouldTreatEarlyPresentRecursionAsForwardable(bool hasPresentTrampoline, bool hasPresentBypass,
                                                          bool inWrapperPresent, bool isWrappedSwapChain,
                                                          bool streamlineFGRunning) {
    // The thread-local fast recursion guard exists to break Steam/overlay
    // stack-overflow loops before the heavier Present ownership tracking runs.
    // But during startup on the non-wrapper DX12 path we can legally re-enter
    // the detour before any inline trampoline exists. In that specific case,
    // treating the call as already-forwardable starves the first real Present of
    // normal ProcessFrame/overlay bootstrap. Only take the fast-path when we
    // already have a safe forwarding target or are in a known nested Present
    // topology.
    return hasPresentTrampoline || hasPresentBypass || inWrapperPresent || isWrappedSwapChain || streamlineFGRunning;
}

inline bool ShouldCaptureQueueWhenSkippingWrapForStreamline(bool streamlineLoaded) {
    // When Streamline is present we skip swapchain wrapping, but the non-wrapper
    // DX12 overlay path still needs the swapchain queue/device captured on the
    // create path that actually fired. Steam can block our inline
    // CreateSwapChainForHwnd hook, and some games use CreateSwapChain instead of
    // CreateSwapChainForHwnd entirely.
    return streamlineLoaded;
}

// External Present entry hooks can recurse back through our detour. Some paths
// need a bypass trampoline available at install time so re-entrant Present can
// still reach the real DXGI implementation.
bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable);

inline bool ShouldForceSteamDX12BypassForState(bool bypassAvailable, bool isSteamOverlay, bool isD3D12SwapChain,
                                               bool inWrapperPresent, bool isWrappedSwapChain, bool streamlineLoaded,
                                               ce::fg_runtime::RuntimeMode runtimeMode, bool streamlineFGRunning,
                                               bool nvPresentLoaded) {
    if (!bypassAvailable || !isSteamOverlay || !isD3D12SwapChain) {
        return false;
    }
    if (inWrapperPresent || isWrappedSwapChain) {
        return false;
    }
    const bool unsafeSteamStartupWindow = streamlineLoaded || nvPresentLoaded;
    if (!unsafeSteamStartupWindow) {
        return false;
    }

    const bool streamlineNeedsBypass =
        streamlineLoaded && !streamlineFGRunning && runtimeMode != ce::fg_runtime::RuntimeMode::kDLSSFG;
    const bool smoothMotionNeedsBypass = nvPresentLoaded;
    return streamlineNeedsBypass || smoothMotionNeedsBypass;
}

inline bool ShouldAllowDX12StartupPresentPassForState(bool hasThirdPartyOverlay, bool presentTrampolineInstalled,
                                                       bool present1TrampolineInstalled, bool steamBypassShouldOwnPath,
                                                       ce::fg_runtime::RuntimeMode runtimeMode,
                                                       bool streamlineFGRunning) {
    if (!hasThirdPartyOverlay || presentTrampolineInstalled || present1TrampolineInstalled) {
        return false;
    }

    const bool actualFrameGenerationActive = streamlineFGRunning ||
                                             runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG ||
                                             runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;

    // The startup compatibility pass exists to let third-party overlays settle
    // before we start driving our own DX12 startup routing. Once Steam's
    // dedicated bypass path already owns the call chain, consuming the first
    // top-level Presents here just starves HandleDX12ProcessFrame and the
    // overlay never bootstraps.
    return !actualFrameGenerationActive && !steamBypassShouldOwnPath;
}

inline bool ShouldTreatStreamlinePresentAsSyntheticReentrant(bool isD3D12SwapChain, bool streamlineFGRunning,
                                                             bool callerFromStreamlineModule,
                                                             bool streamlineStartupHandoffInProgress,
                                                             bool presentOwnershipActive,
                                                             bool recentLargePresentGap,
                                                             bool matchesExpectedPresentThread,
                                                             bool startupTopLevelPresentAlreadyConsumed) {
    if (!(isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule)) {
        return false;
    }

    // Late DLSS runtime-owned handoffs can surface the first live Present only
    // after the game already stopped issuing top-level Presents for a few
    // hundred milliseconds. Treating that first startup-window Present as a
    // synthetic bypass starves the normal Present path and PostSL never
    // bootstraps. Keep the synthetic route for true recursive/worker-thread
    // cases, but let large-gap startup-handoff Presents run as top-level
    // Presents when no other Present currently owns the path, the call is still
    // arriving on the expected game-present thread, and we have not already
    // consumed that one-time startup bootstrap Present for the current handoff.
    if (streamlineStartupHandoffInProgress && recentLargePresentGap && !presentOwnershipActive &&
        matchesExpectedPresentThread && !startupTopLevelPresentAlreadyConsumed) {
        return false;
    }

    return true;
}

inline bool ShouldAllowSpecialStreamlinePresentRouting(bool observerOnlyMode) {
    // The narrowed observer-startup-present-only seam now preserves only the
    // non-Streamline startup-Present probe pieces such as the FFX startup
    // bypass. Special Streamline-originated Present routing stays passive in
    // all observer modes so the staged seam can be compared cleanly against the
    // stable observer-policy-only baseline.
    return !observerOnlyMode;
}

inline bool ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(bool observerOnlyMode,
                                                                     bool startupTopLevelPresentConsumed,
                                                                     bool callerFromStreamlineModule,
                                                                     bool streamlineStartupHandoffInProgress,
                                                                     bool postSLStartupActivationPending,
                                                                     bool streamlineSyntheticReentrant) {
    // Once the pure-DLSS startup family has already consumed its one-shot
    // top-level bootstrap and PostSL startup is still pending, the next decisive
    // Streamline-originated Present can be the only callback opportunity before
    // the runtime either settles or stalls. Sending that call down the old
    // synthetic/bypass path can strand PostSL activation again. Keep the call on
    // the normal SL route instead while startup is still half-armed, including
    // the first post-expiry callback family right after the startup window clears.
    return !observerOnlyMode && startupTopLevelPresentConsumed && streamlineStartupHandoffInProgress &&
           callerFromStreamlineModule && postSLStartupActivationPending && streamlineSyntheticReentrant;
}

inline bool ShouldBypassFFXPresentDuringStreamlineStartup(bool isD3D12SwapChain, bool callerFromFFXFGModule,
                                                            bool streamlineStartupHandoffPending,
                                                            bool streamlineStartupTransitionWindowActive,
                                                            bool observerOnlyMode,
                                                            bool observerStartupPresentOnlyMode) {
    // During repeated FSR->DLSS handoffs, FFX teardown Presents can arrive
    // before Streamline publishes its running signal or after an older PostSL
    // path clears the pending latch for the new epoch. A short explicit
    // transition window keeps those Presents on the safe bypass route instead
    // of falling through the generic oPresent path into third-party hook
    // chains.
    return !(observerOnlyMode && !observerStartupPresentOnlyMode) && isD3D12SwapChain && callerFromFFXFGModule &&
           (streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive);
}

inline bool ShouldKeepPostSLCallbackInstalledDuringTransition(bool streamlineFGRunningAfterTransition) {
    return streamlineFGRunningAfterTransition;
}

// Vulkan-layer label selection for translated APIs should prefer the active DXVK
// D3D11 path over a merely-present DXVK D3D9 helper DLL in the same folder.
inline const char* SelectTranslatedGraphicsAPIName(bool hasDxvkD3D11, bool hasDxvkD3D9, bool hasVkd3dD3D12,
                                                   bool hasDX10) {
    if (hasDxvkD3D11) {
        return hasDX10 ? "DX10 (DXVK)" : "DX11 (DXVK)";
    }
    if (hasDxvkD3D9) {
        return "DX9 (DXVK)";
    }
    if (hasVkd3dD3D12) {
        return "DX12 (VKD3D-Proton)";
    }
    return "Vulkan";
}

// Direct-call helpers: bypass vtable hooks by calling saved original function
// pointers directly. Used by CWrapDXGISwapChain to avoid re-entry through
// hooked vtable.
HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams);

// DX12 marks known foreign overlay swapchains so Present can bypass full
// ProcessFrame/queue-tracking on those auxiliary chains.
void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath = nullptr);
void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain);

}  // namespace DXGIShared
