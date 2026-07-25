#pragma once

#include <dxgi1_4.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "../dxgi_presentation_color.h"
#include "../fg_runtime_state.h"

// Forward declaration
class PerformanceMetrics;

// Post-SL overlay rendering callback.  Invoked during re-entrant Present
// (after Streamline's FG pipeline finishes) so the overlay renders AFTER FG
// interpolation - matching the standard inject-overlay approach for FG compatibility.
using PostSLOverlayRenderFn = void (*)(IDXGISwapChain* pSwapChain);
using PostSLStartupActivationServiceFn = bool (*)(const char* source, bool clearStartupWindow);

// API/present types and the shared DXGI swapchain state declarations.

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
bool RecordSwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace,
                               bool* changed = nullptr);
bool QuerySwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE& colorSpace);
HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain,
                                          DXGI_COLOR_SPACE_TYPE colorSpace);
ce::presentation_color::Encoding ResolveSwapChainPresentationEncoding(IDXGISwapChain* swapChain,
                                                                      DXGI_FORMAT format,
                                                                      DXGI_COLOR_SPACE_TYPE* trackedColorSpace = nullptr,
                                                                      bool* hasTrackedColorSpace = nullptr);
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

}  // namespace DXGIShared
