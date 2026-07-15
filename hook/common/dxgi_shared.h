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
// interpolation — matching the standard inject-overlay approach for FG compatibility.
using PostSLOverlayRenderFn = void (*)(IDXGISwapChain* pSwapChain);
using PostSLStartupActivationServiceFn = bool (*)(const char* source, bool clearStartupWindow);

namespace DXGIShared {

enum class APIType {
    Unknown,
    D3D10,
    D3D11,
    D3D12,
    Vulkan  // For WSI-DXGI interop
};

enum class SteamNullCallbackRecoveryPatchTarget {
    DummyNoPresent,
    DXGIBypassPresent,
};

inline SteamNullCallbackRecoveryPatchTarget SelectSteamNullCallbackRecoveryPatchTarget(bool bypassPresentAvailable) {
    return bypassPresentAvailable ? SteamNullCallbackRecoveryPatchTarget::DXGIBypassPresent
                                  : SteamNullCallbackRecoveryPatchTarget::DummyNoPresent;
}

// Some runtimes expose lower-version compatibility interfaces on higher-version
// swapchains (for example DX11-on-DXVK can answer ID3D10 queries). Always prefer
// the highest actual device API so DX11 swapchains do not fall back to DX10 code
// paths just because compatibility interfaces are present.
inline APIType SelectPrimarySwapChainAPIType(bool hasD3D12Device, bool hasD3D11Device, bool hasD3D10Device) {
    if (hasD3D12Device) {
        return APIType::D3D12;
    }
    // On Windows 10+ the D3D10 runtime is implemented on top of D3D11
    // (D3D10-on-D3D11).  A D3D10 device's swapchain will QI for both
    // ID3D11Device (translation layer) and ID3D10Device (native), while a
    // native D3D11 device's swapchain will QI for ID3D11Device but NOT for
    // ID3D10Device.  When both succeed the device is D3D10-on-D3D11 —
    // functionally D3D10 — so prefer D3D10 over D3D11.
    if (hasD3D10Device) {
        return APIType::D3D10;
    }
    if (hasD3D11Device) {
        return APIType::D3D11;
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

// Service for forcing a pending PostSL startup activation with a valid retained
// swapchain when the normal ProcessFrame path has stalled behind Streamline.
extern std::atomic<PostSLStartupActivationServiceFn> g_PostSLStartupActivationService;

// Direct Streamline FG active signal — set by streamline_hook.cpp when
// slDLSSGSetOptions transitions FG on/off.  More immediate than heuristic
// FG type detection.  Used by DX12 hook for pre-SL vs post-SL routing.
extern std::atomic<bool> g_StreamlineFGRunning;

// Present/Present1 call counter for bypass detection by SL hook.
extern std::atomic<uint64_t> g_PresentCallCounter;

// Records that ProcessFrame already performed the exact-proxy PostSL
// explicit-OFF keep-alive draw for the current top-level Present. The wrapper
// owns the outer scope across ProcessFrame and its real Present; nested detour
// scopes preserve the marker so one displayed frame never receives the overlay
// twice even if the DLSS suspend edge occurs inside the real Present call.
void BeginPostSLOffKeepAlivePresentScope();
void EndPostSLOffKeepAlivePresentScope();
void MarkPostSLOffKeepAlivePrePresentDrawn();
bool WasPostSLOffKeepAlivePrePresentDrawn();

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

static constexpr ULONGLONG kStreamlineStartupTransitionGraceMs = 3000;

inline void ArmStreamlineStartupTransitionWindow(ULONGLONG durationMs = kStreamlineStartupTransitionGraceMs) {
    g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    g_SharedState.streamlineStartupTransitionUntilMs.store(GetTickCount64() + durationMs, std::memory_order_release);
}

inline void ExtendStreamlineStartupTransitionWindow(ULONGLONG durationMs = kStreamlineStartupTransitionGraceMs) {
    const ULONGLONG extendedUntilMs = GetTickCount64() + durationMs;
    ULONGLONG currentUntilMs = g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);

    while (currentUntilMs < extendedUntilMs &&
           !g_SharedState.streamlineStartupTransitionUntilMs.compare_exchange_weak(
               currentUntilMs, extendedUntilMs, std::memory_order_acq_rel, std::memory_order_acquire)) {}
}

inline void ClearStreamlineStartupTransitionWindow() {
    // Window expiry alone must not erase the one-shot bootstrap latch. Startup can
    // remain half-armed after the timer ends, and the next Streamline-originated
    // Present still needs to know that the top-level handoff bootstrap already ran.
    g_SharedState.streamlineStartupTransitionUntilMs.store(0, std::memory_order_release);
}

inline void ResetStreamlineStartupTransitionState() {
    g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    ClearStreamlineStartupTransitionWindow();
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
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain);
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain);

// Experimental: skip CE overlay rendering when Steam handler is invoked.
// When enabled, DetourPresent skips HandleDX12ProcessFrame and CallOriginalPresent
// calls Steam's handler directly.  This isolates whether Steam's handler alone
// (without CE overlay) causes the black screen.
inline std::atomic<bool>& GetSteamOnlyOverlayExperimentalFlag() {
    static std::atomic<bool> s_flag{false};
    return s_flag;
}

// Exported handlers for specific APIs (implemented in their respective hook
// files)
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame);
void HandleDX12ResizeBegin();
void HandleDX12ResizeEnd();
void HandleDX11ResizeBegin();

// Opt-in kill-switch (env var CE_DLSS_TOGGLE_OVERLAY_EAGER, default OFF): when set, CE draws the
// overlay present-time (RTSS-style) right before the Streamline-startup Present bypass so the frame
// that DLSS-G freezes on during a runtime DLSS-FG toggle-ON still carries the overlay. See Round 4
// (llm-wiki/frame-generation/guardrails.md).
bool IsDlssToggleEagerOverlayEnabled();

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
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason);

// Returns true when DXGI swapchain hooks should be installed despite a
// third-party overlay being loaded.  Third-party overlays (e.g. Steam) may
// install inline hooks on the Present function code in dxgi.dll.  Our vtable
// hooks on the swapchain object bypass those inline hooks entirely, so there
// is no recursion risk.  Always install — the vtable path is safe regardless
// of overlay presence.
inline bool ShouldInstallSwapchainHooksWithThirdPartyOverlay(bool /*thirdPartyOverlayLoaded*/,
                                                             bool /*hasPresentDetourHooks*/) {
    return true;
}

inline bool ShouldRefreshLivePresentHooksForSwapchainPath(bool hasReadableVtable, bool trackedVtableMatchesCurrent,
                                                          bool presentHookInstalled, bool present1HookInstalled) {
    if (!hasReadableVtable) {
        return false;
    }

    return !trackedVtableMatchesCurrent || !presentHookInstalled || !present1HookInstalled;
}

inline bool ShouldRunSharedD3D10Or11ProcessFrame(APIType api) {
    return api == APIType::D3D10 || api == APIType::D3D11;
}

inline bool ShouldApplyUnfocusedFlipModelDoNotWait(bool isD3D12Swapchain, bool isFullscreen, bool isForeground,
                                                   UINT presentFlags) {
    if (isForeground || isFullscreen) {
        return false;
    }

    // D3D12 engines often keep building GPU work while unfocused. Forcing
    // DO_NOT_WAIT there can create an unbounded ECL/Present loop and has caused
    // x86 DX12 device hangs during Alt+Tab. Let DXGI's normal pacing stall
    // instead; CE keeps overlay resources alive so the overlay can resume on the
    // first drawable frame.
    if (isD3D12Swapchain) {
        return false;
    }

    constexpr UINT kAllowTearing = 0x00000200U;
    constexpr UINT kRestart = 0x00000004U;
    return (presentFlags & (kAllowTearing | kRestart)) == 0;
}

inline bool ShouldWaitOnD3D12FocusLossFrameLatency(bool isD3D12Swapchain, bool isFullscreen, bool processHasForeground,
                                                   bool isIconic, bool hasZeroSize, bool presentSucceeded,
                                                   bool frameGenerationActive, bool runtimeOwnedPresentation,
                                                   bool hasFrameLatencyWaitable) {
    (void)isD3D12Swapchain;
    (void)isFullscreen;
    (void)processHasForeground;
    (void)isIconic;
    (void)hasZeroSize;
    (void)presentSucceeded;
    (void)frameGenerationActive;
    (void)runtimeOwnedPresentation;
    (void)hasFrameLatencyWaitable;
    // `20260602_213952` proved the waitable stays immediately signaled during
    // the failing focus churn, so it is not a correctness gate. Keep the wrapper
    // on a pure Present path for D3D12 focus loss; focus/reacquire safety is
    // handled by the DX12 overlay policy.
    return false;
}

inline bool ShouldDeferVTableRepairDuringStreamlineStartup(bool streamlineFGRunning,
                                                           bool streamlineStartupHandoffPending,
                                                           bool streamlineStartupTransitionWindowActive,
                                                           bool postSLConfirmedRendering) {
    if (!streamlineFGRunning) {
        return false;
    }

    // Confirmation belongs to a specific swapchain/queue epoch. A fresh
    // Streamline startup handoff can still have an older confirmed PostSL proof
    // while DLSSG is rebuilding its internal swapchain path, so keep vtable
    // repair out of both the unconfirmed and explicitly fresh-startup windows.
    return !postSLConfirmedRendering || streamlineStartupHandoffPending || streamlineStartupTransitionWindowActive;
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

inline bool ShouldKeepSLPresentRoutingDisabledForNativeFG(bool effectiveFSRRuntime,
                                                          bool runtimeOwnedNativeFGPresentPath) {
    // Streamline's Present-hook chain must stay detached not only while the
    // effective runtime mode already says FSR_FG, but also through the native
    // FSR teardown window where the runtime still owns presentation even though
    // ffxConfigure(frameGenerationEnabled=0) has temporarily published Off.
    return effectiveFSRRuntime || runtimeOwnedNativeFGPresentPath;
}

inline bool ShouldKeepSLPresentRoutingDisabledForRuntimeState(ce::fg_runtime::RuntimeMode runtimeMode,
                                                              bool runtimeOwnedNativeFGPresentPath) {
    // Streamline's Present hook is required for DLSS-G frames, but a
    // Streamline-owned "no FG" phase should behave like an ordinary DXGI
    // Present path. Routing the no-FG startup/menu phase through SL's global
    // Present hook can hand NVIDIA's driver a partially transitioned swapchain
    // while CE is also in the hook chain.
    if (runtimeMode == ce::fg_runtime::RuntimeMode::kStreamlineNoFG) {
        return true;
    }

    return ShouldKeepSLPresentRoutingDisabledForNativeFG(ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode),
                                                         runtimeOwnedNativeFGPresentPath);
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
        // When Steam overlay is loaded without Streamline or NvPresent (e.g.
        // Strange Brigade DX12), calling oPresent (dxgi!Present with Steam's
        // E9 JMP) re-enters Steam's overlay handler which crashes because
        // vtable[8] = DetourPresent and Steam can't resolve a "next" handler.
        // The bypass trampoline skips all in-memory hooks and calls the real
        // DXGI Present directly, which is safe.
        return true;
    }

    const bool streamlineNeedsBypass = streamlineLoaded && !streamlineFGRunning;
    const bool smoothMotionNeedsBypass = nvPresentLoaded;
    return streamlineNeedsBypass || smoothMotionNeedsBypass;
}

inline bool ShouldInvokeGuardedExternalSteamOverlayPresentForState(
    bool externalPresentHookAvailable, bool bypassAvailable, bool isSteamOverlay, bool isD3D12SwapChain,
    bool inWrapperPresent, bool isWrappedSwapChain, bool externalOverlayPresentInvokeInProgress,
    bool streamlineStackActive, bool streamlinePluginLookupGuardAvailable,
    bool steamNullCallbackRecoveryAvailable = true) {
    // Directly calling Steam's saved Present hook is only safe when CE has a
    // bypass trampoline available for any recursive Present that Steam may issue
    // internally.  Wrapped swapchains already have their own cooperation path.
    if (!externalPresentHookAvailable || !bypassAvailable || !isSteamOverlay || !isD3D12SwapChain || inWrapperPresent ||
        isWrappedSwapChain || externalOverlayPresentInvokeInProgress) {
        return false;
    }

    if (streamlineStackActive && (!streamlinePluginLookupGuardAvailable || !steamNullCallbackRecoveryAvailable)) {
        // Steam may query Streamline while rendering its overlay, and it may
        // also call through its own lazily initialized NULL callback. The
        // Streamline plugin guard and Steam NULL-callback VEH guard protect
        // different failure modes; both must be present before CE directly
        // invokes Steam from a Streamline-originated stack.
        return false;
    }

    return true;
}

inline bool ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(
    bool basePolicyAllowsInvoke, bool steamCallbackSlotReadable, bool steamCallbackIsNull, bool steamCallbackIsCEDummy,
    bool steamCallbackIsInvalidLowAddress, bool steamNullCallbackRecoveryAvailable) {
    if (!basePolicyAllowsInvoke) {
        return false;
    }

    if (!steamCallbackSlotReadable) {
        // Unknown Steam builds may move the callback slot. Preserve the older
        // guarded behavior when CE cannot inspect the slot.
        return true;
    }

    if (steamCallbackIsCEDummy || steamCallbackIsInvalidLowAddress) {
        // CE installs the dummy only after proving Steam's callback slot was
        // absent. Re-entering Steam while the slot still points at that no-op,
        // or at an invalid sentinel, can drive Steam/Streamline into a partial
        // overlay path with no real renderer behind it.
        return false;
    }

    if (steamCallbackIsNull) {
        return steamNullCallbackRecoveryAvailable;
    }

    return true;
}

inline bool ShouldInvokeGuardedSteamPresentDuringForcedBypass(bool streamlineLoaded, bool streamlineFGRunning,
                                                              bool nativeFSRPresentationActive = false) {
    // When Streamline is merely loaded but FG has not actually started, Talos'
    // Steam hook chain can accept direct calls without advancing Present. Repeating
    // that partial third-party hook path is unsafe; the DXGI bypass trampoline is
    // the stable transport until an FG runtime owns the Present chain. Native
    // FSR is not Streamline FG, but it is an FG-owned presentation path; keep
    // Steam visible there by invoking it through the guarded path first.
    return !streamlineLoaded || streamlineFGRunning || nativeFSRPresentationActive;
}

inline bool ShouldFallbackGuardedExternalSteamOverlayPresentForResult(bool bypassAvailable, HRESULT steamPresentHr,
                                                                      bool backbufferIndexMeasured,
                                                                      bool backbufferAdvanced) {
    if (!bypassAvailable) {
        return false;
    }
    if (FAILED(steamPresentHr)) {
        return true;
    }
    return backbufferIndexMeasured && !backbufferAdvanced;
}

inline bool ShouldBypassRecursiveExternalOverlayPresent(bool externalOverlayPresentInvokeInProgress,
                                                        bool bypassAvailable) {
    return externalOverlayPresentInvokeInProgress && bypassAvailable;
}

inline bool ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(
    bool bypassAvailable, bool isSteamOverlay, bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain,
    bool hadFSRFGPhase, bool startupTopLevelCandidate) {
    // The first recovered top-level Streamline Present after an FSR-owned epoch
    // can still hit Steam's stale Present hook chain even after Streamline has
    // already flipped back to DLSS FG. The older Steam startup bypass helper is
    // intentionally narrower and goes inactive once DLSS FG is live, so keep a
    // separate transport-risk signal for this one protected post-FSR handoff.
    return bypassAvailable && isSteamOverlay && isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain &&
           hadFSRFGPhase && startupTopLevelCandidate;
}

inline bool ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupNormalRoute(
    bool bypassAvailable, bool isSteamOverlay, bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain,
    bool hadFSRFGPhase, bool keepStartupPresentOnNormalRoute) {
    // The same stale Steam Present-hook chain can survive past the one-shot
    // startup-handoff Present and still be live on the later decisive
    // synthetic-startup normal-route callbacks that keep PostSL progressing on a
    // recovered post-FSR swapchain.
    return bypassAvailable && isSteamOverlay && isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain &&
           hadFSRFGPhase && keepStartupPresentOnNormalRoute;
}

inline bool ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRConfirmedStandaloneNormalRoute(
    bool bypassAvailable, bool isSteamOverlay, bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain,
    bool hadFSRFGPhase, bool invokePostSLOnConfirmedStandaloneNormalRoute) {
    // Once startup has settled, some runtimes surface the live generated-frame
    // callback as a confirmed standalone Streamline Present on the recovered
    // swapchain. That later branch still needs the bypass trampoline when the
    // stale Steam hook chain is present.
    return bypassAvailable && isSteamOverlay && isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain &&
           hadFSRFGPhase && invokePostSLOnConfirmedStandaloneNormalRoute;
}

inline bool ShouldAllowDX12StartupPresentPassForState(bool hasThirdPartyOverlay, bool presentTrampolineInstalled,
                                                      bool present1TrampolineInstalled, bool steamBypassShouldOwnPath,
                                                      bool bypassAvailable, ce::fg_runtime::RuntimeMode runtimeMode,
                                                      bool streamlineFGRunning) {
    if (!hasThirdPartyOverlay || presentTrampolineInstalled || present1TrampolineInstalled) {
        return false;
    }

    // The startup compatibility pass forwards Present through a safe trampoline
    // (oPresentTrampoline) or the bypass trampoline (oPresentBypass). When the
    // vtable-hook path was chosen (inline hooks skipped due to external E9 JMP),
    // the inline trampoline is null. Without a bypass trampoline available,
    // CallOriginalPresent has no safe forwarding path — it would route through
    // the external overlay's E9 JMP, causing re-entrant crashes (RIP=0).
    if (!bypassAvailable) {
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

inline bool ShouldTreatStreamlinePresentAsSyntheticReentrant(
    bool isD3D12SwapChain, bool streamlineFGRunning, bool callerFromStreamlineModule, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling, bool streamlineStartupHandoffInProgress, bool presentOwnershipActive,
    bool recentLargePresentGap, bool matchesExpectedPresentThread, bool startupTopLevelPresentAlreadyConsumed) {
    if (!(isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule)) {
        return false;
    }

    // Once PostSL has already confirmed a successful render and the explicit
    // confirmed-startup-settling window has ended, a later Streamline-originated
    // Present that arrives with no active Present owner is no longer just a
    // synthetic worker-thread recursion candidate. That is the live FG Present
    // path resurfacing as a standalone top-level call, and forcing it through the
    // synthetic/bypass path corrupts the active DX12 FG chain.
    //
    // During the short confirmed-startup-settling window, however, GTA still
    // needs these standalone Streamline Presents to classify as synthetic first
    // so the later callback-on-normal-route split can keep PostSL rendering
    // advancing without sending the Present itself down the old bypass path.
    if (postSLConfirmedRendering && !postSLConfirmedButStartupSettling && !presentOwnershipActive) {
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

inline bool ShouldUseStreamlineStartupTopLevelCandidate(
    bool observerOnlyMode, bool streamlineSyntheticReentrant, bool callerFromStreamlineModule,
    bool isD3D12SwapChain, bool streamlineFGRunning, bool streamlineStartupHandoffInProgress,
    bool recentLargePresentGap, bool matchesExpectedPresentThread, bool postSLConfirmedRendering) {
    // The large-gap promotion is a one-shot bootstrap route. Once PostSL has
    // already submitted successfully, reclassifying later standalone output
    // Presents as the original handoff bypass suppresses the proven PostSL
    // callback until the large-gap window expires and visibly drains the
    // overlay from every swapchain buffer.
    return !observerOnlyMode && !streamlineSyntheticReentrant && callerFromStreamlineModule && isD3D12SwapChain &&
           streamlineFGRunning && streamlineStartupHandoffInProgress && recentLargePresentGap &&
           matchesExpectedPresentThread && !postSLConfirmedRendering;
}

inline bool ShouldRenderExactPostSLBeforeStartupHandoffTransport(
    bool isD3D12SwapChain, bool hadFSRFGPhase, bool safePostFSRBootstrapPath, bool streamlineFGRunning,
    bool startupTopLevelCandidate, bool postSLConfirmedRendering) {
    // The first runtime-owned Present after FSR can precede the ordinary PostSL
    // callback by one output. Once the post-FSR queue topology is proven safe,
    // draw onto that exact proxy backbuffer before forwarding it. Cold DLSS and
    // already-confirmed routes keep their established paths unchanged.
    return isD3D12SwapChain && hadFSRFGPhase && safePostFSRBootstrapPath && streamlineFGRunning &&
           startupTopLevelCandidate && !postSLConfirmedRendering;
}

inline bool ShouldAllowSpecialStreamlinePresentRouting(bool observerOnlyMode) {
    // The narrowed observer-startup-present-only seam now preserves only the
    // non-Streamline startup-Present probe pieces such as the FFX startup
    // bypass. Special Streamline-originated Present routing stays passive in
    // all observer modes so the staged seam can be compared cleanly against the
    // stable observer-policy-only baseline.
    return !observerOnlyMode;
}

inline bool ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool hadFSRFGPhase, bool explicitSetOptionsActivation, bool safePostFSRBootstrapPath,
    bool startupTopLevelPresentConsumed, bool callerFromStreamlineModule, bool postSLStartupActivationPending,
    bool postSLActiveButUnconfirmed, bool postSLConfirmedButStartupSettling, bool streamlineSyntheticReentrant) {
    // Once the pure-DLSS startup family has already consumed its one-shot
    // top-level bootstrap, the next decisive Streamline-originated Present can be
    // the only callback opportunity before the runtime either settles or stalls.
    // Sending that call down the old synthetic/bypass path can strand PostSL both
    // before activation and during the first post-activation warm-up callbacks
    // before PostSL has ever confirmed a successful render. GTA's latest active
    // validation also showed one more seam: the very next Streamline-originated
    // Present immediately after the first successful PostSL submits can still be
    // part of the same fragile startup family. Keep the call on the normal SL
    // route instead until startup is no longer half-armed and those first
    // confirmed PostSL frames have settled.
    // The post-FSR comeback family is stricter than cold pure-DLSS startup, but
    // it also already has stronger ownership evidence: the fresh runtime-owned
    // Streamline handoff queue preserved across the FSR->DLSS transition. Once
    // that stricter family is still half-armed, forcing its first comeback
    // Presents down the old synthetic/bypass return path reopens the old crash
    // seam before the post-FSR bootstrap can progress. Talos showed two more
    // nuances: a GetState-only comeback is still weaker evidence than an explicit
    // SetOptions activation edge, but some runtimes can also reach a safe
    // post-FSR bootstrap topology before any OFF->ON SetOptions edge ever
    // appears. Once that safe topology is already proven, continuing to force the
    // comeback through synthetic/bypass just starves PostSL until the same old NX
    // crash family returns later.
    const bool startupHalfArmed =
        postSLStartupActivationPending || postSLActiveButUnconfirmed || postSLConfirmedButStartupSettling;
    return !observerOnlyMode && callerFromStreamlineModule && startupHalfArmed && streamlineSyntheticReentrant &&
           (startupTopLevelPresentConsumed ||
            (hadFSRFGPhase && (explicitSetOptionsActivation || safePostFSRBootstrapPath)));
}

inline bool ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(bool isD3D12SwapChain, bool hadFSRFGPhase,
                                                                            bool startupTopLevelCandidate,
                                                                            bool safePostFSRBootstrapPath,
                                                                            bool staleThirdPartyPresentHookRisk) {
    // The first large-gap Streamline startup-handoff Present can still be the
    // only top-level call that re-establishes the live route after an FSR-owned
    // swapchain handoff. That Present should remain logically on the normal SL
    // route so startup-policy state can advance, but the actual transport is
    // still fragile while the recovered queue topology has only just been
    // proven. A stale third-party Present hook is one risk; the FSR->DLSS
    // runtime handoff itself is another even when no external overlay is
    // present. Keep the route, but transport through the bypass trampoline.
    return isD3D12SwapChain && hadFSRFGPhase && startupTopLevelCandidate &&
           (safePostFSRBootstrapPath || staleThirdPartyPresentHookRisk);
}

inline bool ShouldTreatStreamlineStartupNormalRouteTransportAsUnsafe(
    bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain, bool bypassAvailable,
    bool callerFromStreamlineModule, bool streamlineStartupHandoffInProgress, bool runtimeOwnedSwapchainActive,
    bool startupNormalRouteCandidate, bool postFSRRuntimeHandoffRisk, bool thirdPartyPresentHookRisk) {
    // Runtime ownership alone is not a transport hazard. GTA's all-off -> DLSS FG
    // Apply path needs real Streamline Present traffic after the EOS-backed
    // runtime-owned handoff, otherwise DLSSG startup never activates. Use bypass
    // transport only when the same startup family also carries a stale/fragile
    // third-party Present hook risk or a proven post-FSR runtime handoff risk.
    return isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain && bypassAvailable &&
           callerFromStreamlineModule && streamlineStartupHandoffInProgress && runtimeOwnedSwapchainActive &&
           startupNormalRouteCandidate && (postFSRRuntimeHandoffRisk || thirdPartyPresentHookRisk);
}

inline bool ShouldBypassPresentForStreamlineStartupHandoffPresentOnNormalRoute(bool isD3D12SwapChain,
                                                                               bool startupTopLevelCandidate,
                                                                               bool startupNormalRouteTransportRisk,
                                                                               bool staleThirdPartyPresentHookRisk) {
    return isD3D12SwapChain && startupTopLevelCandidate &&
           (startupNormalRouteTransportRisk || staleThirdPartyPresentHookRisk);
}

inline bool ShouldActivateStreamlinePresentRoutingForHookTarget(bool entryHookDetected, bool hookTargetResolved,
                                                                bool hookTargetFromStreamlineModule,
                                                                bool hookTargetFromCaptureHookModule) {
    // dxgi!Present can begin with a jump for several reasons: CE's own inline
    // hook, DXGI's internal thunking, Steam/EOS hook chains, or Streamline. Only
    // the Streamline target is safe to route through as the SL FG chain.
    return entryHookDetected && hookTargetResolved && hookTargetFromStreamlineModule &&
           !hookTargetFromCaptureHookModule;
}

inline bool ShouldUseOverlaylessAppThreadPresentForPostFSRStreamlineStartupHandoff(
    bool observerOnlyMode, bool isD3D12SwapChain, bool inWrapperPresent, bool isWrappedSwapChain,
    bool originalPresentAvailable, bool streamlineFGRunning, bool streamlinePresentRoutingActive,
    bool callerFromStreamlineModule, bool streamlineStartupHandoffInProgress, bool runtimeOwnedSwapchainActive,
    bool hadFSRFGPhase, bool safePostFSRBootstrapPath, bool postSLConfirmedRendering,
    bool startupTopLevelPresentAlreadyConsumed) {
    // Some real runtimes surface the first post-FSR Streamline startup handoff as
    // an app-thread Present on the freshly runtime-owned swapchain before any
    // Streamline-originated top-level Present reaches us. Treat that first call
    // as the startup bootstrap transport hazard too; otherwise the normal SL
    // route can enter the driver after CE has injected overlay work, while a
    // direct DXGI bypass skips Streamline's mandatory startup handling. Keep the
    // first handoff overlayless but route it through Streamline.
    return !observerOnlyMode && isD3D12SwapChain && !inWrapperPresent && !isWrappedSwapChain &&
           originalPresentAvailable && streamlineFGRunning && streamlinePresentRoutingActive &&
           !callerFromStreamlineModule && streamlineStartupHandoffInProgress && runtimeOwnedSwapchainActive &&
           hadFSRFGPhase && safePostFSRBootstrapPath && !postSLConfirmedRendering &&
           !startupTopLevelPresentAlreadyConsumed;
}

inline bool ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool hadFSRFGPhase, bool explicitSetOptionsActivation,
    bool activeDLSSFGRuntimeSignalObserved, bool safePostFSRBootstrapPath, bool postSLStartupActivationPending,
    bool postSLActiveButUnconfirmed, bool postSLStartupActivationEntered, bool postSLConfirmedButStartupSettling,
    bool streamlineSyntheticReentrant) {
    // Once PostSL has already confirmed at least one successful render, GTA's
    // startup family can still need a few more Streamline-originated Presents to
    // advance the stable-frame counter and keep the visible overlay alive. Those
    // calls should still execute the PostSL callback, but they must stay on the
    // normal SL route instead of taking the old synthetic/bypass return path.
    if (observerOnlyMode || !streamlineSyntheticReentrant) {
        return false;
    }

    if (postSLConfirmedButStartupSettling) {
        return true;
    }

    // Once the retained-startup service has entered PostSL, progress must move
    // through the normal Streamline Present family. Blocking the callback here
    // leaves pure-DLSS resume paths active-but-unconfirmed forever.
    if (postSLStartupActivationEntered && postSLActiveButUnconfirmed) {
        return true;
    }

    // Pure-DLSS startup is allowed to enter PostSL once the app has either
    // explicitly requested DLSS-G or the official DLSS-G state API reports an
    // active runtime signal. Both are stronger than "Streamline is present" and
    // keep the overlay from waiting for the whole startup timer before PostSL can
    // even begin its own protected warmup.
    if (!hadFSRFGPhase && (explicitSetOptionsActivation || activeDLSSFGRuntimeSignalObserved) &&
        postSLStartupActivationPending) {
        return true;
    }

    // Post-FSR comeback still intentionally uses the older repeated-callback
    // stabilization path before PostSL fully confirms rendering, but only once
    // the bootstrap topology itself is already safe.  We no longer require an
    // explicit SetOptions(ON) edge here: the safe bootstrap path already proves
    // the queue topology can handle the callback, and requiring both conditions
    // can strand PostSL indefinitely when SetOptions has not yet been observed
    // but the handoff queue is already live (FSR->OFF->DLSS transitions).
    return hadFSRFGPhase && safePostFSRBootstrapPath && (postSLStartupActivationPending || postSLActiveButUnconfirmed);
}

inline bool ShouldBypassPresentWhileKeepingStreamlineStartupPresentOnNormalRoute(
    bool isD3D12SwapChain, bool keepStartupPresentOnNormalRoute, bool postSLConfirmedRendering,
    bool postSLConfirmedButStartupSettling, bool startupNormalRouteTransportRisk, bool staleThirdPartyPresentHookRisk) {
    // After a fresh runtime-owned Streamline handoff, the shared routing layer can
    // correctly decide that decisive startup Presents must stay in the normal
    // Streamline family so PostSL keeps making progress. If that same family has
    // a stale third-party hook chain, keep the callback/routing decision but
    // transport the actual Present through the bypass trampoline until PostSL has
    // both confirmed a successful render and left startup settling.
    return isD3D12SwapChain && keepStartupPresentOnNormalRoute &&
           (!postSLConfirmedRendering || postSLConfirmedButStartupSettling) &&
           (startupNormalRouteTransportRisk || staleThirdPartyPresentHookRisk);
}

inline bool ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute(
    bool observerOnlyMode, bool isD3D12SwapChain, bool streamlineFGRunning, bool callerFromStreamlineModule,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling, bool presentOwnershipActive,
    bool streamlineSyntheticReentrant) {
    // Some DLSS FG runtimes surface the steady-state generated-frame Present as a
    // standalone top-level Streamline call with no active Present owner instead
    // of a later recursive callback. Keep that Present on the normal SL route,
    // but still invoke PostSL there once the explicit startup-settling window has
    // ended so visible PostSL rendering does not starve after the first few
    // confirmed startup frames.
    return !observerOnlyMode && isD3D12SwapChain && streamlineFGRunning && callerFromStreamlineModule &&
           postSLConfirmedRendering && !postSLConfirmedButStartupSettling && !presentOwnershipActive &&
           !streamlineSyntheticReentrant;
}

inline bool ShouldBypassPresentForConfirmedStandaloneStreamlinePresentOnNormalRoute(
    bool isD3D12SwapChain, bool hadFSRFGPhase, bool invokePostSLOnConfirmedStandaloneNormalRoute,
    bool staleThirdPartyPresentHookRisk) {
    // Talos still reaches the stale-Steam-hook crash family after the post-FSR
    // comeback has already left the earlier startup-bypass window. At that later
    // boundary the Present is no longer classified as synthetic startup traffic;
    // it is the confirmed standalone Streamline Present that keeps PostSL alive
    // on the recovered swapchain. Routing should stay on the normal Streamline
    // family, but DX12 transport still needs the bypass trampoline on post-FSR
    // comebacks so we do not fall back through a fresh-swapchain third-party hook
    // chain whose saved original Present pointer is still stale.
    return isD3D12SwapChain && hadFSRFGPhase && invokePostSLOnConfirmedStandaloneNormalRoute &&
           staleThirdPartyPresentHookRisk;
}

inline bool ShouldBypassFFXPresentDuringStreamlineStartup(bool isD3D12SwapChain, bool callerFromFFXFGModule,
                                                          bool streamlineStartupHandoffPending,
                                                          bool streamlineStartupTransitionWindowActive,
                                                          bool observerOnlyMode, bool observerStartupPresentOnlyMode) {
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
