#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "graphics_hook.h"

namespace ce::ffx_api {
struct CallbackDescFrameGenerationPresent;
struct Resource;
using PresentCallback = uint32_t (*)(CallbackDescFrameGenerationPresent*, void*);
}  // namespace ce::ffx_api

// Installs the global DXGI factory vtable hooks (CreateSwapChain / CreateSwapChainForHwnd) plus
// the inline/deep hooks on dxgi!CreateSwapChainForHwnd. Called as the HookThread's first action
// (fast-app coverage) and retried from DX12Hook::Init when dxgi.dll was not loaded yet.
void InstallGlobalVTableHooks();

// Internal D3D10/11 hook-discovery swapchains must pass through the shared DXGI factory
// without entering DX12 tracking/wrapping. This scope is thread-local so a concurrent game
// swapchain creation remains fully intercepted.
void DX12_BeginInternalDXGISwapchainProbe();
void DX12_EndInternalDXGISwapchainProbe();
bool DX12_IsInternalDXGISwapchainProbe();

// =============================================================================
// DX12 Debug Logging Infrastructure
// =============================================================================
// These macros provide step-by-step logging for diagnosing DX12 overlay issues.
// Enable via config.ini: debug_logging = true

extern std::atomic<bool> g_DebugLoggingEnabled;

#define DX12_DEBUG(fmt, ...)                       \
    do {                                           \
        if (g_DebugLoggingEnabled) {               \
            HookLog("[DX12] " fmt, ##__VA_ARGS__); \
        }                                          \
    } while (0)

#define DX12_DEBUG_STEP(step, fmt, ...) DX12_DEBUG("[%s] " fmt, step, ##__VA_ARGS__)

#define DX12_DEBUG_HR(step, op, hr) DX12_DEBUG("[%s] %s hr=0x%08X (%s)", step, op, hr, SUCCEEDED(hr) ? "OK" : "FAILED")

#define DX12_DEBUG_PTR(step, name, ptr) DX12_DEBUG("[%s] %s=%p", step, name, ptr)

// Frame logging (throttled - every N frames)
#define DX12_DEBUG_FRAME(frameNum, fmt, ...)                                        \
    do {                                                                            \
        if (g_DebugLoggingEnabled && ((frameNum) % 300 == 0)) {                     \
            HookLog("[DX12:FRAME:%llu] " fmt, (uint64_t)(frameNum), ##__VA_ARGS__); \
        }                                                                           \
    } while (0)

// Error logging (always on)
#define DX12_ERROR(fmt, ...) HookLog("[DX12:ERROR] " fmt, ##__VA_ARGS__)

// Warning logging
#define DX12_WARN(fmt, ...)                             \
    do {                                                \
        if (g_DebugLoggingEnabled) {                    \
            HookLog("[DX12:WARN] " fmt, ##__VA_ARGS__); \
        }                                               \
    } while (0)

class DX12Hook : public GraphicsHook {
    std::vector<IUnknown*> trackedResources;
    std::recursive_mutex resourceMutex;

public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
    void EnsurePresentHooks();  // Called after D3D12 device creation is confirmed

    void TrackResource(IUnknown* res);
    void CleanupResources();

    // Frame classification for FG support
    bool IsRealFrame() const;
    void ClassifyFrame(int commandListCount);
};

extern DX12Hook* g_dx12HookInstance;

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);
void DX12_AccountOverlayTransportPresent(bool inheritCoverageIfNoDraw, const char* gate, const char* source);
bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(IDXGISwapChain* pSwapChain, const char* source);
bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(IDXGISwapChain* pSwapChain, const char* source);
void DX12_HookQueueVTable(ID3D12CommandQueue* queue);
void DX12_HookDeviceVTable(ID3D12Device* device);
void DX12_OnSwapchainResizeBegin();
void DX12_OnSwapchainResizeEnd();
void DX12_InvalidateSwapchain();
void DX12_SignalFSR4SwapchainRecreated();
void DX12_AdjustWrapperResizeDepth(int delta);
void DX12_StartTransitionCooldown();
void DX12_OnStreamlineFGStateChanged(bool active);
void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();
bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
void DX12_PrepareForStreamlineEnableTransition();
void DX12_BeginStreamlineEnableCall();
void DX12_EndStreamlineEnableCall();
void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source);
bool DX12_TryInvokePostSLStartupActivationCallback(const char* source, bool clearStartupWindow,
                                                   bool allowConfirmedWarmupService = false);
DWORD DX12_GetGamePresentThreadId();
// Returns one retained reference to the real game queue. Streamline's official UI-tag overlay
// recorder uses it only to initialize device-scoped PSO/font resources; the overlay draw itself
// remains inside the app-provided command list and is never separately submitted on this queue.
ID3D12CommandQueue* DX12_AcquireOriginalGameQueueForOverlay();
void DX12_SetFFXPresentCallbackBridge(void* bridgeKey, ce::ffx_api::PresentCallback originalCallback,
                                      void* originalUserContext);
bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey);
bool DX12_HasFFXPresentCallbackBridgeWithOriginal(void* bridgeKey);
bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback);
void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey);
void DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain(void* swapChain);
// Runtime-owned FG/UI resources do not carry a DXGI color space. Resolve their
// HDR interpretation from the last validated real-swapchain presentation
// contract instead of inferring content from an R10/FP16 storage format.
bool DX12_ResolveRuntimeOwnedOverlayTargetHDRState(DXGI_FORMAT format);
// Capture the queue supplied in an FFX FrameGenerationSwapChain DX12 creation descriptor. Normally this exact
// queue owns direct work. If it is proven to be a Streamline wrapper, CE may use the already-validated real
// original game queue on the target resource's device (the wrapper's underlying submission path).
void DX12_RegisterNativeFSRSwapchainPresentationQueue(void* context, void* swapChain,
                                                      ID3D12CommandQueue* presentationQueue);
// Recover the descriptor-equivalent owner from the retained pre-FSR original game queue when ffxCreateContext
// was already in flight before CE routed a cached export pointer. The protected inner DXGI create is required
// evidence only: its queue is FFX's internal present queue and must never be used as the owner binding.
// Must run before the proxy Present hook becomes reachable.
bool DX12_TryRecoverNativeFSRSwapchainPresentationQueue(void* context, void* swapChain);
void DX12_UnregisterNativeFSRSwapchainPresentationQueue(void* context, const char* reason);
void DX12_ServiceDeferredECLProbe();
void DX12_OnNativeFSRFrameGenerationConfigured(bool enabled, bool retainedPresentCallbackBridge = false);
void DX12_OnNativeFSRPresentCallbackRoutingConfigured(bool enabled, bool bridgeActive, bool appCallbackProvided);
void DX12_OnNativeFSRFrameGenerationContextsDestroyed();
void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason);
bool DX12_IsNativeFSRStartupConfigureArmingPending();
void DX12_ClearNativeFSRStartupConfigureArming(const char* reason);
void DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason);
uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* callbackDesc,
                                                 void* userCtx);
// Last-resort compatibility draw onto a game-registered FFX UI resource (no-app-callback FSR FG). The normal
// game-thread proxy path uses target-compatible owner-queue ordering with no copy or CPU wait; this function
// retains the isolated completion-waited path for a CE-owned substitute when no owner queue can be resolved.
bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResource, uint32_t ffxState, uint32_t flags);
// Drive the composite using the cached (CE-substituted or game) UI texture. The per-present overlay refresh
// under active no-callback FSR FG. Primary driver: the FFX proxy-present prework (game thread, before AMD's
// Present). Fallback driver: DetourPresent's no-callback branch (AMD's presenter thread — composite only,
// NEVER the substitute re-assert; see the deadlock boundary in dx12_overlay_policy.h). Returns true if composited.
bool DX12_CompositeOverlayOntoCachedFFXUiResource();
// --- FFX proxy-swapchain Present hook (game-thread composite driver) ---
// Install CE's vtable hook on the game-facing FFX FrameInterpolation proxy swapchain (from the successful
// swapchain-context create output or ffxConfigure(FrameGeneration).swapChain). ffxRuntimeAnchor is any address
// inside the FFX runtime module (e.g. the forwarded create/configure target) used to verify the Present entry
// actually resolves into that module before patching. Idempotent; returns true when already installed.
bool DX12_TryInstallFFXProxyPresentHook(void* swapChain, void* ffxRuntimeAnchor, const char* source);
void DX12_RemoveFFXProxyPresentHook(const char* reason);
bool DX12_IsFFXProxyPresentHookInstalled();
// True while the proxy-present prework is the live composite driver (hook installed + game presenting
// through it). DetourPresent's kSkipBundleCovers arm skips its fallback composite while this is true.
bool DX12_IsFFXProxyPresentHookDriving();
// True only on a thread currently inside the proxy-present prework — the ONLY context allowed to call
// FFXHook_ReRegisterSubstituteUiResource (deadlock boundary; see dx12_overlay_policy.h).
bool DX12_IsCurrentThreadInsideFFXProxyPresentPrework();
void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason);
bool DX12_IsFFXUiResourceCompositionActive();
// Cache the UI texture for the per-present composite. Gated to no-callback FSR FG only.
bool DX12_ShouldCacheFFXUiResourceForBundle();
// True if the UI texture has been cached (for the VEH disarm condition).
bool DX12_IsFFXUiResourceCachedForBundle();
// Direct read of the no-callback composition flag (for the VEH one-shot disarm logic).
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive();
// True when the live swapchain queue is the game's own original queue (AMD's FG swapchain is gone). Used by
// DetourPresent to detect a STALE no-callback latch on FSR->off and safely fall back to the backbuffer route.
bool DX12_IsLiveSwapchainQueueOriginalGameQueue();
// True when native FSR FG is DISABLED/SUSPENDED while AMD still owns the swapchain (no-callback suspension —
// AMD is not interpolating). The proxy-present prework uses this to select the exact-owner-queue backbuffer route.
bool DX12_IsNativeFSRFGSuspendedDisablePending();
// Minimal-overhead ProcessFrame for no-callback FSR FG (skips policy/lock/heuristic work).
void DX12_ProcessFrameMinimal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                              bool frameGenerationPresentationActive);
struct DX12FFXUiOverlayTargetPreparation {
    ID3D12Resource* target = nullptr;  // one staged reference; commit or discard consumes it
    uint32_t state = 0;
    uint32_t flags = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    uint64_t sequence = 0;
    bool substitute = false;
    bool clearTransparent = false;
};

// Stage the target for a no-callback FSR FG RegisterUiResource intercept without changing the live cache.
// Returns true iff CE should forward a backbuffer-sized substitute for a degenerate game texture. The caller
// must commit only after AMD accepts the configure, or discard on failure, so rejected configurations cannot
// replace the last known-good overlay target.
bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags,
                                    ce::ffx_api::Resource* ceSubstitute,
                                    DX12FFXUiOverlayTargetPreparation* preparation);
void DX12_CommitFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation);
void DX12_DiscardFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation);
// Called from Hooked_ffxConfigure right before forwarding the configure to AMD. Stamps a QPC + frame
// counter so the freeze-watchdog timeline can correlate composite calls with ffxConfigure forwards.
void DX12_NoteFfxConfigureForward(uint64_t configureType);
void RemoveGlobalVTableHooks();

extern "C" {
void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue);
void DX12_AdjustWrapperResizeDepth_C(int delta);
}
