#pragma once

struct DX12OverlayCoverageSnapshot;

class DX12DescFreeBackend;

struct DX12OverlayState;

struct SteamDeferredOverlaySubmitState;

struct DX12Context;

struct ForwardedCreateSwapchainForHwndCallerContext;

class ScopedForwardedCreateSwapchainForHwndCallerContext;

class ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard;

struct CreateSwapchainForHwndCallerContext;

struct CreateSwapchainQueueCaptureEvidence;

struct FFXPresentCallbackBridgeState;

struct DX12WrappedPresentFocusLossContext;

class ScopedCEOverlayECLSubmission;

struct ProgressResolvedOfficialFFXOverlayFallbackProof;

struct FFXUiCompositeTimelineEntry;

struct NativeFSRSwapchainQueueBinding;

struct AcquiredNativeFSROwnerQueue;

namespace {
struct Dx12FocusAnalysisSample;
}

#include <combaseapi.h>

#include <d3d11.h>

#include <d3d11on12.h>

#include <d3d12.h>

#include <dxgi1_6.h>

#include <unknwn.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>

#include <atomic>

#include <chrono>

#include <cmath>

#include <condition_variable>

#include <cstdint>

#include <filesystem>

#include <limits>

#include <map>

#include <mutex>

#include <string>

#include <unordered_map>

#include <vector>

#include "../../common/frame_timing.h"

#include "../../common/raii_helpers.h"

#include "../capture/shared_capture.h"

#include "../common/capture_base.h"

#include "../common/capture_pacing.h"

#include "../common/custom_overlay_dx12.h"

#include "../common/dx12_process_frame_diagnostics.h"

#include "../common/dx12_fg_transition_model.h"

#include "../common/fg_detection.h"

#include "../common/fg_session_state.h"

#include "../common/hook_common.h"

#include "../common/input_manager.h"

#include "../common/overlay_adapter.h"

#include "../common/overlay_compat.h"

#include "../common/overlay_metrics_publisher.h"

#include "../common/performance_metrics.h"

#include "../common/screenshot_hook.h"

#include "../common/streamline_compat.h"

#include "../common/streamline_runtime_policy.h"

#include "../../common/secure_dll_loading.h"

#include "../common/fps_limiter.h"

#include "../common/freeze_watchdog.h"

#include "../common/perf_logger.h"

#include "../common/swapchain_wrapper.h"

#include "../common/system_metrics.h"

#include "../wrappers/dxgi_swapchain_wrap.h"

#include "../wrappers/wrapper_hooks.h"

#include "dx11_hook.h"

#include "dx12_ffx_suspend_overlay.h"

#include "dx12_hook.h"

#include "dx12_sampler_hooks.h"

#include "dx12_streamline_ui_overlay.h"

#include "ffx_hook.h"

#include "graphics_hook.h"

#include "lod_helper.h"

#include "streamline_hook.h"

#include "../common/custom_overlay.h"

#include "../common/dx12_dred.h"

#include "../common/dx12_overlay_policy.h"

#include "../common/ffx_api_parsing.h"

#include "../common/overlay_shader_bytecode.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

#include "../wrappers/wrapper_base.h"

#include "dxgi_shared.h"

// ============================================================================
// Typedefs for D3D12 functions
typedef void(STDMETHODCALLTYPE* ExecuteCommandListsPtr)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

typedef HRESULT(STDMETHODCALLTYPE* SignalPtr)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);

// Global Function Pointers for detours (Visible to other modules)
extern ExecuteCommandListsPtr oExecuteCommandLists;

extern CreateCommittedResourcePtr oCreateCommittedResource;

// --- DX12 API call trace diagnostic (gated by Dx12TraceEnabled; off by default) ---
typedef HRESULT(STDMETHODCALLTYPE* CreateCommandQueuePtr)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID,
                                                          void**);

typedef HRESULT(STDMETHODCALLTYPE* CreateDescriptorHeapPtr)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                            void**);

typedef HRESULT(STDMETHODCALLTYPE* CommandQueueSignalPtr)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

extern CreateCommandQueuePtr oTraceCreateCommandQueue;

extern CreateDescriptorHeapPtr oTraceCreateDescriptorHeap;

extern CommandQueueSignalPtr oTraceCommandQueueSignal;

#if defined(__clang__) || defined(__GNUC__)
#define CE_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#elif defined(_MSC_VER)
#include <intrin.h>
#define CE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_RETURN_ADDRESS() nullptr
#endif

// SwapChain Detour Pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

enum class DX12OverlayRenderRoute : uint32_t {
    kNone = 0,
    kNormal = 1,
    kPostSL = 2,
    kFFXPresentCallback = 3,
    kStreamlineUI = 4,
};

// PostSL ECL diagnostic counter — reset on each PostSL reactivation epoch.
extern std::atomic<int> g_PostSLECLDiagCount;

// CRITICAL FIX: Use atomic pointers for thread-safe access
// These are read/written from multiple threads (hook thread, present thread, etc.)
extern std::atomic<ID3D12Device*> g_Device;

extern std::atomic<ID3D12CommandQueue*> g_CommandQueue;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
extern std::recursive_mutex g_CommandQueueMutex;

// --- Overlay GPU breadcrumbs (native-FSR ffxQuery-wedge diagnosis) -------------------------------------
// A GPU-writable / CPU-readable marker buffer. CE records WriteBufferImmediate(MARKER_OUT) markers INTO the
// overlay command list around each op (start / after RT barrier / after draw / before close). When the
// FSR-FG overlay submit on AMD's runtime queue wedges AMD's ffxQuery, the freeze watchdog reads these
// markers: the highest op that reached the latest sequence value is the last GPU op CE's command list
// completed before the GPU stalled. This distinguishes "CE's GPU op stalled the queue" (markers stop
// mid-list) from "CE's list finished — the deadlock is a fence/CPU issue or AMD's own subsequent work"
// (all markers reached). Works without any device removal (the freeze is a pure hang).
enum OverlayGpuBreadcrumbOp : uint32_t {
    kOverlayBcStart = 1,       // command list reset, recording started
    kOverlayBcAfterRTBarrier,  // backbuffer transitioned to RENDER_TARGET
    kOverlayBcAfterDraw,       // overlay draw recorded
    kOverlayBcBeforeClose,     // all overlay commands recorded (about to Close)
    kOverlayBcSlotCount,
};

extern ID3D12Resource* g_DummyBackBuffer;

// Use pointer to prevent static destructor execution in non-game processes
// (Explorer fix)
extern DX12Hook* g_dx12HookInstance;

// Swapchain visibility, tracked from the wrapped Present HRESULT.
// DXGI_STATUS_OCCLUDED means the window is fully covered/minimized and the
// present is a no-op; the overlay is not visible to the user in that state. A
// merely-unfocused window that is STILL VISIBLE (e.g. a borderless background
// window or a window on another monitor) keeps presenting S_OK, so the overlay
// must keep rendering. CE holds backbuffer GPU work only when the swapchain is
// not presentable, never merely because focus moved elsewhere.
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#endif

// ---- FFX proxy-swapchain Present hook (game-thread composite driver) -----------------------------------
// Under native no-callback FSR FG the game presents to AMD's game-facing FrameInterpolation PROXY swapchain
// (an FFX-runtime object the game got from ffxCreateContext). AMD's proxy Present enters its swapchain
// criticalSection and spin-waits (no timeout) on compositionFenceCPU while HOLDING it; the real DXGI
// swapchain is presented later by AMD's dedicated presenter thread — which is where CE's DetourPresent
// used to run the composite + substitute re-assert. Any blocking CE work there stalls AMD's pacing, and the
// re-assert (registerUiResource takes the same criticalSection) closes a permanent deadlock cycle with the
// game thread (session 20260701_213656). This driver moves the per-frame overlay work to the GAME thread by
// vtable-hooking the PROXY's Present/Present1 (class vtable inside the FFX runtime module): composite CE's
// overlay onto the cached/substituted UI texture, re-assert the substitute registration (same thread + lock
// order as the game's own per-frame RegisterUiResource), then forward to AMD's Present — which snapshots
// the registered UI resource under its criticalSection (copyUiResource per present with internal double
// buffering), so the prework lands exactly between the game's 1x1 re-registration and AMD's consumption.
// The proxy pointer arrives generically in ffxConfigure(FrameGeneration).swapChain (GTA passes it in the
// startup-arming AND enabled configures the one-shot VEH intercepts; the test app passes it too).
typedef HRESULT(STDMETHODCALLTYPE* PFN_FFXProxyPresent)(IDXGISwapChain*, UINT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_FFXProxyPresent1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

enum class StartupOverlayActivationStage {
    kNone = 0,
    kDelayRTVInitAfterBackendInit,
    kDelaySyncInitAfterRTVInit,
};

enum class StartupOverlayFirstDrawProbeStage {
    kNone = 0,
    kBackbufferTouchOnly,
    kPipelineStateOnly,
    kActualRender,
    kComplete,
};

// Function pointers for global factory vtable hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);

HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);

HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

uint64_t HookAllocateOverlayFGPublicationSequence();

void HookUpdatePreferredOverlayFGPublicationState(bool active, ce::fg_runtime::RuntimeMode runtimeMode, const char* source);

bool HookTryGetPreferredOverlayFGPublicationState(PreferredOverlayFGPublicationState* state);

void DX12_AccountOverlayTransportPresent(bool inheritCoverageIfNoDraw, const char* gate, const char* source);

bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(IDXGISwapChain* pSwapChain, const char* source);

bool HookIsPostSLOverlayActiveButUnconfirmed();

bool HookHasPostSLSyntheticStartupActivationEntered();

bool HookIsPostSLOverlayConfirmedRendering();

bool HookIsPostSLOverlayConfirmedButStartupSettling();

bool HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();

int HookGetPostSLRuntimeStateStabilizationLastFrame();

bool HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();

int HookGetPostSLStaleOffWarmupProtectionLastFrame();

bool HookIsPostSLOverlayConfirmedButGetStateOffWarmupProtected();

int HookGetPostSLGetStateOffWarmupProtectionLastFrame();

bool HookHasFSRFGHistory();

bool HookHasExplicitStreamlineSetOptionsActivation();

extern "C" __declspec(dllexport) void DX12_SetDeferOverlaySubmitToSteamECL(bool defer);

extern "C" __declspec(dllexport) bool DX12_IsDeferOverlaySubmitPending();

ID3D12CommandQueue* DX12_AcquireOriginalGameQueueForOverlay();

void DX12_DumpDredIfDeviceRemoved(const char* reason);

void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason); // forward decl — defined near UI-composite globals;

void DX12_LogOverlayGpuBreadcrumbs(const char* reason);

void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason);

void DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason);

bool DX12_IsNativeFSRStartupConfigureArmingPending();

void DX12_ClearNativeFSRStartupConfigureArming(const char* reason);

void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source);

bool DX12_TryInvokePostSLStartupActivationCallback(const char* source, bool clearStartupWindow, bool allowConfirmedWarmupService);

bool HookHasSafePostFSRBootstrapPath();

bool HookHasRuntimeOwnedNativeFGPresentPath();

extern "C" __declspec(dllexport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount, UINT syncInterval, UINT presentFlags);

extern "C" __declspec(dllexport) void DX12_ClearWrappedPresentFocusLossContext();

bool DX12_ResolveRuntimeOwnedOverlayTargetHDRState(DXGI_FORMAT format);

void DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain(void* swapChain);

void DX12_SetFFXPresentCallbackBridge(void* bridgeKey, ce::ffx_api::PresentCallback originalCallback, void* originalUserContext);

bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey);

bool DX12_HasFFXPresentCallbackBridgeWithOriginal(void* bridgeKey);

bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback);

void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey);

void DX12_OnNativeFSRPresentCallbackRoutingConfigured(bool enabled, bool bridgeActive, bool appCallbackProvided);

void DX12_OnNativeFSRFrameGenerationContextsDestroyed();

void DX12_OnNativeFSRFrameGenerationConfigured(bool enabled, bool retainedPresentCallbackBridge);

uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* desc, void* userCtx);

void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason); // defined with the proxy hook below;

void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason);

bool DX12_IsFFXUiResourceCompositionActive();

bool DX12_ShouldCacheFFXUiResourceForBundle();

bool DX12_IsFFXUiResourceCachedForBundle();

bool DX12_IsNativeFSRInternalNoCallbackCompositionActive();

bool DX12_IsLiveSwapchainQueueOriginalGameQueue();

bool DX12_IsNativeFSRFGSuspendedDisablePending();

bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags, ce::ffx_api::Resource* ceSubstitute, DX12FFXUiOverlayTargetPreparation* preparation);

void DX12_DiscardFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation);

void DX12_CommitFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation);

void DX12_NoteFfxConfigureForward(uint64_t configureType);

bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr, uint32_t ffxState, uint32_t flags);

bool DX12_CompositeOverlayOntoCachedFFXUiResource();

void DX12_RegisterNativeFSRSwapchainPresentationQueue(void* context, void* swapChain, ID3D12CommandQueue* presentationQueue);

bool DX12_TryRecoverNativeFSRSwapchainPresentationQueue(void* context, void* swapChain);

void DX12_UnregisterNativeFSRSwapchainPresentationQueue(void* context, const char* reason);

bool DX12_CompositeOverlayOntoSuspendBackbuffer(IDXGISwapChain* proxy, const char* routeName);

bool DX12_IsFFXProxyPresentHookInstalled();

bool DX12_IsCurrentThreadInsideFFXProxyPresentPrework();

bool DX12_IsFFXProxyPresentHookDriving();

bool DX12_TryInstallFFXProxyPresentHook(void* swapChain, void* ffxRuntimeAnchor, const char* source);

void DX12_RemoveFFXProxyPresentHook(const char* reason);

void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason);

void ShutdownImGui();

void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend);

inline void CleanupOverlay();

void CleanupRTVs();

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);

void DX12_HookQueueVTable(ID3D12CommandQueue* queue);

void DX12_HookDeviceVTable(ID3D12Device* device);

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC);

void DX12_AdjustWrapperResizeDepth(int delta);

void CleanupRTVs();

void DX12_InvalidateSwapchain();

void DX12_SignalFSR4SwapchainRecreated();

void DX12_AdjustWrapperResizeDepth_C(int delta);

__attribute__((noinline)) void DX12_NotifyCommandListsForQueue(ID3D12CommandQueue* pQueue, UINT numCommandLists);

void DX12_NotifyCommandLists(UINT numCommandLists);

void DX12_OnSwapchainResizeEnd();

void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend);

void CleanupRTVs();

void DX12_InvalidateSwapchain();

void EnsureDX12Hook();

void DX12_StartTransitionCooldown();

void DX12_BeginStreamlineEnableCall();

void DX12_EndStreamlineEnableCall();

void DX12_PrepareForStreamlineEnableTransition();

bool DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();

DWORD DX12_GetGamePresentThreadId();

void DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();

void DX12_OnStreamlineFGStateChanged(bool active);

void RemoveGlobalVTableHooks();

void DX12_InstallPresentHooksForSwapchain(IDXGISwapChain* pSwapChain);

void ShutdownImGui();

bool InitImGui(ID3D12Device* device, int buffers, DXGI_FORMAT format, HWND hwnd);

void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx, D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride);

void CreateRTVs(ID3D12Device* device, IDXGISwapChain3* swapChain, int bufferCount);

void InitOverlaySync(ID3D12Device* device, int bufferCount, ID3D12CommandQueue* gameQueue);

void CleanupOverlay(bool preserveNativeFSRPresentCallbackBackend);

void CleanupRTVs();

void DX12_OnSwapchainResizeBegin();

void DX12_OnSwapchainResizeEnd();

bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(IDXGISwapChain* pSwapChain, const char* source);

extern "C" __declspec(dllexport) void DX12_SubmitSteamDeferredOverlay();

extern "C" __declspec(dllexport) void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount, UINT syncInterval, UINT presentFlags, HRESULT presentHr, BOOL isFullscreen, BOOL isIconic, BOOL hasZeroSize, HWND gameWindow);

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture, bool applicationSourcePresent, bool frameGenerationPresentationActive, ce::dx12_process_frame_diagnostics::StageTimings* diagnostics = nullptr);

void DX12_ResetImGuiFrameCounter();

void DX12_ResetOverlayFrameDelay();

void DX12_ProcessFrameMinimal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent, bool frameGenerationPresentationActive);

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain);

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool applicationSourcePresent, bool frameGenerationPresentationActive);
}

namespace DXGIShared {
void HandleDX12ResizeBegin();
}

namespace DXGIShared {
void HandleDX12ResizeEnd();
}

extern "C" __declspec(dllexport) bool DX12_FlushDeferredSignalWithInfo( ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo);

extern "C" __declspec(dllexport) void DX12_FlushDeferredSignal();

extern "C" __declspec(dllexport) bool DX12_WaitForFocusLossOverlayFenceAfterPresent( const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext* context, const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* flushInfo);

extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue);

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);

__attribute__((noinline)) void DX12_HookQueueVTable(ID3D12CommandQueue* queue);

void DX12_HookDeviceVTable(ID3D12Device* device);

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC);

HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppQueue);

HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid, void** ppHeap);

HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);

void DX12_ServiceDeferredECLProbe();

DWORD WINAPI UnloadThread(LPVOID lpParam);

inline bool IsActualFrameGenerationActive();

inline bool IsStreamlineLoaded();

inline bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue,
                                   bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain = false,
                                   IDXGISwapChain* associatedSwapchain = nullptr,
                                   bool authoritativeNormalSwapchainReturn = false);

inline bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
    bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source);

// --- end DX12 API call trace diagnostic ---
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex dx12_hook_g_ExecuteCommandListsHookStateMutex;

inline std::map<void**, ExecuteCommandListsPtr> dx12_hook_g_ExecuteCommandListsOriginalByVTable;

// ExecuteCommandLists runs many times per frame in CPU-bound workloads, so keep a
// lock-free cache for the most recently used vtable/original pair.
inline std::atomic<void**> dx12_hook_g_LastExecuteCommandListsVTable{nullptr};

inline std::atomic<ExecuteCommandListsPtr> dx12_hook_g_LastExecuteCommandListsOriginal{nullptr};

// Real D3D12 ExecuteCommandLists function pointer obtained by probing a
// COMPUTE queue (which SL doesn't hook for FG).  Used to bypass SL's
// vtable ECL hook when submitting overlay command lists.
inline std::atomic<ExecuteCommandListsPtr> dx12_hook_g_RealD3D12ECL{nullptr};

// Real D3D12 Signal function pointer for the command queue.  Probed alongside
// g_RealD3D12ECL.  Used to signal an overlay completion fence that ensures
// all overlay GPU work is finished before the FG runtime processes the
// swapchain backbuffer.  Bypasses any FG-runtime hooks on the Signal
// vtable entry.
inline std::atomic<SignalPtr> dx12_hook_g_RealD3D12Signal{nullptr};

// Separate fence for tracking overlay GPU completion during FG.  When FSR or
// DLSS frame generation is active, the main g_State.fence signal is skipped
// to avoid desyncing the FG pipeline.  This completion fence is signaled via
// the raw D3D12 Signal pointer and CPU-waited to ensure overlay GPU work
// completes before the FG runtime reads the swapchain backbuffer.
inline std::atomic<ID3D12Fence*> dx12_hook_g_OverlayCompletionFence{nullptr};

// Deferred ECL probe flag: set when ProbeRealD3D12ECL is skipped due to
// the Streamline startup window being active.  The probe runs after the
// startup window expires to avoid creating a temporary COMPUTE queue
// during Streamline's critical initialization (which can crash Streamline
// with a null pointer call on some games/configs).
inline std::atomic<bool> dx12_hook_g_ProbeRealD3D12ECLDeferred{false};

inline PFN_CreateSwapChain dx12_hook_oCreateSwapChain = nullptr;

inline PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwnd = nullptr;

inline ID3D12GraphicsCommandList* dx12_hook_s_descFreeCmdList = nullptr;

inline D3D12_CPU_DESCRIPTOR_HANDLE dx12_hook_s_descFreeRtv = {};

// Per-slot GPU-completion guard for the DescFree UPLOAD ring (vb_/ib_).
//
// Overlay render sites can set these two statics right before calling
// RenderOverlay() when a slot-reuse guard is intentionally enabled:
//   * s_descFreeSlotFence      = g_State.fence (the fence signaled for this
//                                frame's overlay GPU work)
//   * s_descFreeSlotGuardValue = the value that fence will reach once this
//                                frame's overlay ECL has executed on the GPU,
//                                or 0 to disable the guard for this frame.
//
// The DescFree backend round-robins through kPoolSize persistently-mapped
// UPLOAD vertex/index buffers.  Without a per-slot fence it would memcpy new
// geometry into a slot the GPU might still be reading from a previous frame.
// That is harmless while the GPU keeps up, but during long GPU pauses the CPU
// can wrap and overwrite in-flight vertex/index data.  Guard 0 leaves the
// current behavior unchanged and is used unless a caller publishes a concrete
// fence/value pair for this frame.
inline ID3D12Fence* dx12_hook_s_descFreeSlotFence = nullptr;

inline UINT64 dx12_hook_s_descFreeSlotGuardValue = 0;

inline void PostSLOverlayRender(IDXGISwapChain* pSwapChain);

// Post-SL overlay rendering state.  Controls whether the re-entrant Present
// callback should actually render or skip (e.g. during FG cooldown / resize).
inline std::atomic<bool> dx12_hook_g_PostSLOverlayActive{false};

inline std::atomic<int> dx12_hook_g_PostSLCooldownRemaining{0};

// Make-before-break across explicit Streamline FG OFF (suspend/menu): while
// the DLSS-G proxy keeps presenting after slDLSSGSetOptions(off), confirmed
// PostSL stays armed-and-rendering on the same proven queue/swapchain until
// an authoritative normal swapchain/queue return is observed (or the proxy/SL
// stack dies). Cleared on: authoritative normal-route recovery, Streamline FG
// ON (warm resume), protected-FFX-startup quiesce, FFX takeover, swapchain
// invalidation/resize, Streamline modules gone, shutdown. Session
// 20260613_032326: the suspend/resume handoff seams were the last visible
// 3-4-present DLSS blanks.
inline std::atomic<bool> dx12_hook_g_PostSLExplicitOffKeepAlive{false};

// A warm resume consumes the explicit-OFF latch, but the first wrapper
// ProcessFrame can report its outer/original swapchain before PostSL performs
// the next real submit on the still-live proxy. Preserve the confirmed backend
// across those bookkeeping pointer changes until an active PostSL submit
// proves the resumed route again. This is event-driven, not time-based.
inline std::atomic<bool> dx12_hook_g_PostSLWarmResumePreservationPending{false};

// Raw identity only; never AddRef'd or dereferenced. A successful PostSL submit
// proves that this exact swapchain is compatible with the retained PostSL route
// for the current lifecycle epoch. The explicit-OFF keep-alive must not apply
// that route proof to any other swapchain pointer.
inline std::atomic<IDXGISwapChain*> dx12_hook_g_LastSuccessfulPostSLSwapchain{nullptr};

// Per-calling-thread proof that PostSL completed a real, device-healthy submit.
// The explicit-OFF top-level route snapshots this around its direct callback so
// it only suppresses a nested callback when that same thread/Present was
// actually covered; a distinct Streamline worker frame cannot create a false
// success or be de-duplicated.
inline thread_local uint64_t dx12_hook_s_PostSLSuccessfulSubmitSequence = 0;

inline std::atomic<ULONGLONG> dx12_hook_g_LastProcessFrameTickMs{0};

inline std::atomic<ULONGLONG> dx12_hook_g_LastFFXPresentCallbackTickMs{0};

inline std::atomic<bool> dx12_hook_g_FFXPresentCallbackBridgeExpected{false};

inline std::atomic<bool> dx12_hook_g_NativeFSRInternalNoCallbackComposition{false};

inline std::atomic<ULONGLONG> dx12_hook_g_LastDX12OverlayRenderTickMs{0};

inline std::atomic<uint32_t> dx12_hook_g_LastDX12OverlayRenderRoute{static_cast<uint32_t>(DX12OverlayRenderRoute::kNone)};

// NOTE: DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending is now in DXGIShared::g_SharedState
// so streamline_hook.cpp can check it from FlushSuppressedSetOptionsOffIfNeeded().
inline std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupTakeoverLogged{false};

inline std::atomic<int> dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount{0};

inline std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested{false};

inline std::atomic<uint32_t> dx12_hook_g_PostSLLifecycleEpoch{0};

inline std::mutex dx12_hook_g_PostSLRenderMutex;

inline std::atomic<uint32_t> dx12_hook_g_StreamlineEnableCallsInFlight{0};

// Set to true when PostSLOverlayRender has confirmed it can render (i.e., re-entrant
// Present calls are actually happening).  In games like GTA V, SL FG bypasses our
// vtable hook for interpolated frames, so PostSL never fires.  When this is false,
// pre-SL rendering is NOT suppressed, allowing the overlay to render before SL.
inline std::atomic<bool> dx12_hook_g_PostSLConfirmedRendering{false};

// True once PostSL has performed at least one CONFIRMED render (devRemoved=0) in the
// CURRENT reactivation epoch. The reactivation warmup exists only to protect the FIRST
// ECL submit on DLSS-FG's fragile init state; once a confirmed render lands, that first
// ECL already succeeded, so the remaining warmup must not re-blank a live overlay (the
// no-blank principle). Strictly epoch-scoped: reset where s_callsSinceReactivation=0 in
// the genuine-reactivation block, set at the confirmed-render edge. The preserve/keep-alive
// warm-resume paths do not bump the epoch, so the flag correctly persists through them.
inline std::atomic<bool> dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch{false};

inline std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed{false};

inline std::atomic<bool> dx12_hook_g_PostSLRuntimeStateStabilizationLogged{false};

// A reactivated PostSL startup that had already confirmed a few frames but had
// not yet reached the repo's broader warmup proof threshold can still inherit
// stale Streamline OFF churn from the earlier epoch. Keep only the narrow
// runtime-state stale-OFF guard extended for that new epoch.
inline std::atomic<bool> dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch{false};

inline const char* DX12OverlayRenderRouteName(uint32_t route) {
    switch (static_cast<DX12OverlayRenderRoute>(route)) {
        case DX12OverlayRenderRoute::kNormal:
            return "normal";
        case DX12OverlayRenderRoute::kPostSL:
            return "post-sl";
        case DX12OverlayRenderRoute::kFFXPresentCallback:
            return "ffx-present-callback";
        case DX12OverlayRenderRoute::kStreamlineUI:
            return "streamline-ui";
        default:
            return "none";
    }
}

// ---------------------------------------------------------------------------
// [OVERLAY COVERAGE] per-present overlay-coverage telemetry (regression gate).
// ---------------------------------------------------------------------------
// Every overlay draw of any route (normal, PostSL, FFX present callback) bumps
// g_OverlayCoverageDrawCount via NoteDX12OverlayRendered. Two accounting event
// streams consume that counter and classify each present as covered/uncovered:
//   1. DX12_ProcessFrameExternal — top-level processed presents (normal, FSR
//      no-callback, FFX-callback and post-FG recovery transports).
//   2. PostSLOverlayRenderGated — SL-routed presents (synthetic re-entrant /
//      startup normal-route callbacks), which bypass ProcessFrameExternal.
// Presents whose visible overlay is composed by the FG runtime from a previous
// covered present (zero-ECL interpolated frames; SL-owned top-level transport
// presents) inherit coverage while no uncovered streak is active, so healthy FG
// sessions stay noise-free and real blanks form one continuous streak.
// Gate attribution: skip sites record a static reason string; the gate captured
// at streak start is reported when the streak ends. Atomics + a tiny spin lock
// only; logging happens at streak boundaries and FG transition edges, never per
// frame.
inline std::atomic<uint64_t> dx12_hook_g_OverlayCoverageDrawCount{0};

inline std::atomic<uint64_t> dx12_hook_g_OverlayCoverageLastSeenDrawCount{0};

inline std::atomic<const char*> dx12_hook_g_OverlayCoverageLastGate{nullptr};

inline std::atomic<const char*> dx12_hook_g_OverlayCoverageStreakGate{nullptr};

// Streak-onset markers so a blank window is bracketed start+end in the log: wall-clock
// tick (ms) and whether PostSL was already CONFIRMED rendering when the streak began
// (confirmed=1 at start means a live overlay got blanked — a no-blank-principle violation).
inline std::atomic<uint64_t> dx12_hook_g_OverlayCoverageStreakStartTickMs{0};

inline std::atomic<bool> dx12_hook_g_OverlayCoverageStreakStartConfirmed{false};

inline ce::dx12_overlay_policy::OverlayPresentCoverageTracker dx12_hook_g_OverlayCoverageTracker;

inline std::atomic_flag dx12_hook_g_OverlayCoverageLock = ATOMIC_FLAG_INIT;

inline thread_local bool dx12_hook_g_RequireExactPostSLStartupTransportDraw = false;

inline thread_local bool dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent = false;

// Verbose overlay-handoff diagnostic window. The [OVERLAY COVERAGE] streak gate only reports a blank
// when a present is UNCOVERED, but an off->DLSS engage flash can sit BELOW that: every present is
// "covered" (real draw or FG-composed inheritance) yet a brand-new DLSS-G proxy can still show a
// generated frame whose overlay history is empty. When a PostSL reactivation arms this window, the
// next N presents log per-present coverage detail (real-draw vs inherited-if-no-draw vs uncovered,
// route, source) so the exact handoff frame is attributable. prevRoute captured at arming separates
// off->DLSS (prevRoute=normal, native->fresh-proxy) from FSR->DLSS (prevRoute=post-sl/ffx, warm proxy).
inline std::atomic<int> dx12_hook_g_OverlayHandoffVerboseLogPresents{0};

inline std::atomic<uint32_t> dx12_hook_g_OverlayHandoffVerbosePrevRoute{0};

inline void NoteDX12OverlayCoverageGate(const char* gate) {
    dx12_hook_g_OverlayCoverageLastGate.store(gate, std::memory_order_relaxed);
}

struct DX12OverlayCoverageSnapshot {
    uint64_t totalPresents = 0;
    uint64_t uncoveredPresents = 0;
    uint64_t currentStreak = 0;
    uint64_t longestStreak = 0;
};

inline DX12OverlayCoverageSnapshot GetOverlayCoverageSnapshot() {
    DX12OverlayCoverageSnapshot snapshot;
    while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
    snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
    snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
    snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
    dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);
    return snapshot;
}

inline const char* DX12OverlayRenderRouteName(uint32_t route);

// Accounts one presented frame. covered = draw-counter delta since the previous
// accounted present (any route), with FG-composed inheritance (see block comment).
inline void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source) {
    const uint64_t draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    // Visibility cannot be interrupted before CE has established its first
    // visible overlay draw. Excluding pre-initialization Presents keeps later
    // transition summaries and interruption markers semantically precise.
    if (!ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(draws)) {
        return;
    }
    const uint64_t lastSeen = dx12_hook_g_OverlayCoverageLastSeenDrawCount.exchange(draws, std::memory_order_acq_rel);
    const bool drawObserved = draws != lastSeen;

    ce::dx12_overlay_policy::OverlayPresentCoverageResult result;
    DX12OverlayCoverageSnapshot snapshot;
    while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
    result = dx12_hook_g_OverlayCoverageTracker.NotePresent(drawObserved, inheritCoverageIfNoDraw);
    snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
    snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
    snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
    snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
    dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);

    // Verbose overlay-handoff diagnostic: per-present detail for the first N presents after a PostSL
    // reactivation. `drawObserved=0 inheritIfNoDraw=1` (covered ONLY by FG-composed inheritance) is the
    // smoking gun for an off->DLSS fresh-proxy flash — DLSS-G presented a generated frame relying on a
    // proxy whose overlay history is still empty. A real draw shows `drawObserved=1`.
    {
        int verboseRemaining = dx12_hook_g_OverlayHandoffVerboseLogPresents.load(std::memory_order_relaxed);
        if (verboseRemaining > 0) {
            dx12_hook_g_OverlayHandoffVerboseLogPresents.store(verboseRemaining - 1, std::memory_order_relaxed);
            const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            const uint32_t prevRoute = dx12_hook_g_OverlayHandoffVerbosePrevRoute.load(std::memory_order_relaxed);
            HookLogImportant(
                "[OVERLAY HANDOFF] present=%llu drawObserved=%d inheritIfNoDraw=%d covered=%d route=%s prevRoute=%s "
                "source=%s currentStreak=%llu remaining=%d",
                static_cast<unsigned long long>(snapshot.totalPresents), drawObserved ? 1 : 0,
                inheritCoverageIfNoDraw ? 1 : 0, (drawObserved || inheritCoverageIfNoDraw) ? 1 : 0,
                DX12OverlayRenderRouteName(route), DX12OverlayRenderRouteName(prevRoute), source ? source : "unknown",
                static_cast<unsigned long long>(snapshot.currentStreak), verboseRemaining - 1);
        }
    }

    if (result.uncoveredStreakStarted) {
        const char* streakGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
        dx12_hook_g_OverlayCoverageStreakGate.store(streakGate, std::memory_order_relaxed);
        const uint64_t startTick = GetTickCount64();
        dx12_hook_g_OverlayCoverageStreakStartTickMs.store(startTick, std::memory_order_relaxed);
        const bool startConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        dx12_hook_g_OverlayCoverageStreakStartConfirmed.store(startConfirmed, std::memory_order_relaxed);
        // Bracket the onset of every blank window with a timestamped marker so even a
        // single-present gap is fully attributable from the log alone.
        static std::atomic<int> s_streakStartLogCount{0};
        const int startLogCount = s_streakStartLogCount.fetch_add(1, std::memory_order_relaxed);
        if (startLogCount < 100 || (startLogCount % 20) == 0) {
            const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            HookLogImportant(
                "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN: no overlay draw belongs to the "
                "current presentation route (gate=%s route=%s source=%s confirmed=%d present=%llu uncovered=%llu)",
                streakGate ? streakGate : "unknown", DX12OverlayRenderRouteName(route), source ? source : "unknown",
                startConfirmed ? 1 : 0, static_cast<unsigned long long>(snapshot.totalPresents),
                static_cast<unsigned long long>(snapshot.uncoveredPresents));
        }
    }
    if (result.uncoveredStreakEnded) {
        static std::atomic<int> s_streakEndLogCount{0};
        const int logCount = s_streakEndLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 100 || (logCount % 20) == 0) {
            const char* streakGate = dx12_hook_g_OverlayCoverageStreakGate.load(std::memory_order_relaxed);
            const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
            const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            const uint64_t startTick = dx12_hook_g_OverlayCoverageStreakStartTickMs.load(std::memory_order_relaxed);
            const uint64_t durationMs = startTick ? (GetTickCount64() - startTick) : 0;
            const bool confirmedDuringStreak = dx12_hook_g_OverlayCoverageStreakStartConfirmed.load(std::memory_order_relaxed);
            HookLogImportant(
                "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] RESTORED after uncovered route: missed=%llu durationMs=%llu "
                "confirmedDuringStreak=%d longestStreak=%llu gate=%s lastGate=%s route=%s source=%s totals: "
                "presents=%llu uncovered=%llu",
                static_cast<unsigned long long>(result.endedStreakLength), static_cast<unsigned long long>(durationMs),
                confirmedDuringStreak ? 1 : 0, static_cast<unsigned long long>(snapshot.longestStreak),
                streakGate ? streakGate : "unknown", lastGate ? lastGate : "unknown", DX12OverlayRenderRouteName(route),
                source ? source : "unknown", static_cast<unsigned long long>(snapshot.totalPresents),
                static_cast<unsigned long long>(snapshot.uncoveredPresents));
        }
    }
}

// Logs a coverage summary line. Called at FG transition edges and shutdown so
// the scripted transition matrix can gate on "no uncovered streak > 1 present".
inline void LogOverlayCoverageSummary(const char* edge) {
    const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
    const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
    const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
    HookLogImportant(
        "[OVERLAY COVERAGE] %s: presents=%llu uncovered=%llu currentStreak=%llu longestStreak=%llu lastGate=%s "
        "lastRoute=%s",
        edge ? edge : "summary", static_cast<unsigned long long>(snapshot.totalPresents),
        static_cast<unsigned long long>(snapshot.uncoveredPresents),
        static_cast<unsigned long long>(snapshot.currentStreak),
        static_cast<unsigned long long>(snapshot.longestStreak), lastGate ? lastGate : "none",
        DX12OverlayRenderRouteName(route));
}

inline void NoteDX12OverlayRendered(DX12OverlayRenderRoute route) {
    const uint64_t drawsBefore = dx12_hook_g_OverlayCoverageDrawCount.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t previousRoute =
        dx12_hook_g_LastDX12OverlayRenderRoute.exchange(static_cast<uint32_t>(route), std::memory_order_acq_rel);
    dx12_hook_g_LastDX12OverlayRenderTickMs.store(GetTickCount64(), std::memory_order_release);
    // [OVERLAY DOUBLE-DRAW] detector: a draw already happened since the last ACCOUNTED present
    // (drawsBefore > lastSeen) and it came from a DIFFERENT route — i.e. two overlay routes rendered
    // within the same present window. One route re-drawing is benign; two different routes can show the
    // overlay TWICE on screen (e.g. the FFX UI-composite prework and PostSL backbuffer rendering were both
    // live for ~3.5s during the GTA FSR->DLSS pre-apply window, session 20260702_092933). Diagnostic only —
    // makes route-arbitration overlaps attributable from one run; visible flicker/dimming correlates here.
    const uint64_t lastAccountedDraws = dx12_hook_g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);
    if (drawsBefore > lastAccountedDraws && previousRoute != static_cast<uint32_t>(route)) {
        static std::atomic<int> s_doubleDrawLogCount{0};
        const int n = s_doubleDrawLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "[OVERLAY DOUBLE-DRAW] two overlay routes rendered in the same present window: %s then %s "
                "(pendingDraws=%llu log=%d)",
                DX12OverlayRenderRouteName(previousRoute), DX12OverlayRenderRouteName(static_cast<uint32_t>(route)),
                static_cast<unsigned long long>(drawsBefore + 1 - lastAccountedDraws), n + 1);
        }
    }
}

// Counts Present calls where PostSL was expected but didn't fire.
// ProcessFrame increments this; PostSLOverlayRender resets it to 0.
// When it exceeds kPostSLStallThreshold (5), pre-SL rendering is allowed
// as a fallback for "FG suspension" (SL FG nominally on, but not generating
// re-entrant Present calls — e.g., game menu/pause).
//
// CONTEXT: During DLSS FG, SL generates re-entrant Present calls from worker
// threads for each interpolated frame.  Our PostSL callback renders the overlay
// in these re-entrant calls.  But when the game enters a menu or pause state,
// SL may stop generating FG frames while g_StreamlineFGRunning stays true
// (slDLSSGSetOptions isn't called with mode=0).  In this state:
//   - Pre-SL rendering is suppressed (g_StreamlineFGRunning = true)
//   - PostSL never fires (no re-entrant Present from SL)
//   - Result: overlay gap with BOTH paths blocked
//
// The stall counter detects this gap and temporarily allows pre-SL rendering.
// When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY: Tested in GTA V Enhanced (menu pauses FG) and Talos Principle
// Reawakened (continuous FG).  The threshold of 5 frames ensures PostSL has
// enough time to fire during normal FG before pre-SL takes over.
inline std::atomic<int> dx12_hook_g_PostSLStallCounter{0};

// Counts consecutive successful PostSL renders since the last FG transition.
// Incremented by PostSLOverlayRender, reset to 0 by FG transition handler.
// The stall fallback is only enabled once this exceeds kPostSLWarmupThreshold,
// preventing pre-SL rendering during FG warmup (SL pipeline still initializing).
//
// PROBLEM: After FG OFF→ON (menu close), SL generates ONE re-entrant Present
// immediately (setting PostSLConfirmed=true), then stalls briefly while the FG
// pipeline warms up.  Without this counter, the stall fallback fires during
// warmup and renders on origGame → DEVICE_HUNG (cross-queue backbuffer access).
inline std::atomic<int> dx12_hook_g_PostSLStableFrameCount{0};

// Flag to reset the queue-change heuristic's internal state.  Set during FG
// transitions so that the heuristic starts fresh afterward. Most transitions
// recapture the "initial queue" from the next 5 frames; a proven normal
// swapchain return supplies its authoritative game queue directly. Without
// this, SL's leftover queue persists after FG OFF → immediate false FSR FG
// detection → wrong queue selection, blank overlay, or DEVICE_HUNG.
inline std::atomic<bool> dx12_hook_g_ResetQueueChangeHeuristic{false};

// Companion reset for the ECL-count-pattern FG heuristic in
// DX12_ProcessFrameExternal. Interpolated/real frame counts accumulated during
// a finished FG phase are stale evidence after any FG transition or swapchain
// recovery: session 20260612_215439 showed `FG detected via ECL count pattern
// (real=5, interp=70)` re-latching phantom FSR_FG on the game's fresh native
// swapchain right after FSR->OFF, and the resulting double mode flip armed two
// 60-frame draw cooldowns that blanked a healthy overlay for 61 presents.
// Set at every site that resets the queue-change heuristic.
inline std::atomic<bool> dx12_hook_g_ResetECLPatternHeuristic{false};

// Optional authoritative queue for the next queue-change heuristic epoch. A
// normal swapchain return supplies the proven original queue here so leftover
// Streamline ECL traffic cannot become the new baseline or a phantom FSR edge.
// g_OriginalGameQueue retains this COM object for the hook lifetime.
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline{nullptr};

inline void RequestFGDetectionHeuristicReset(ID3D12CommandQueue* authoritativeBaseline = nullptr) {
    dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.store(authoritativeBaseline, std::memory_order_release);
    dx12_hook_g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);
    dx12_hook_g_ResetECLPatternHeuristic.store(true, std::memory_order_release);
}

// Grace counter after SL FG turns OFF.  Set by the outer block, decremented each
// frame in CanUseFSRFGHeuristics().  While active, the queue-change heuristic is
// suppressed — the queue naturally switches from SL's internal queue back to
// origGame, which would otherwise false-positive as FSR FG.
inline std::atomic<int> dx12_hook_g_SLOffHeuristicGrace{0};

// Grace counter after Streamline FG turns OFF. Set when PostSL/Streamline
// ownership tears down, and consumed by ProcessFrame swapchain-change handling
// to keep the first replacement swapchain on the guarded transition path.
inline std::atomic<int> dx12_hook_g_SLOffSwapchainReinitGrace{0};

// Reset flag for per-reinit submit diagnostic counter.
inline std::atomic<bool> dx12_hook_g_ResetReinitSubmitCounter{false};

// Outer SL transition epoch — incremented each time the outer FG state management
// block processes an SL FG ON/OFF transition.  The inner transition handler checks
// this to avoid redundant transition processing (double cooldowns, duplicate drain).
inline std::atomic<uint32_t> dx12_hook_g_OuterSLTransitionEpoch{0};

// Last Streamline FG signal observed by the outer ProcessFrame transition block.
// Kept at file scope so direct Streamline teardown paths can synchronize it when
// they invalidate overlay state before ProcessFrame reaches the outer tracker.
inline std::atomic<bool> dx12_hook_g_OuterTrackedSLFGRunning{false};

// Locked queue for PostSL overlay — stays on the first successful queue
// (game's render queue) instead of following SL's FG worker queue changes.
// Reset when PostSL rendering is disabled (FG transition off).
// Protected by atomic exchange in writer; readers must load via the
// dedicated helper or raw pointer access when no concurrent write is possible.
inline ID3D12CommandQueue* dx12_hook_g_PostSLLockedQueue = nullptr;

// Tracks whether FSR FG was ever active during this session.
// Once set, origGame is assumed corrupted (NVIDIA driver internal state broken
// after FSR FG phase) and PostSL uses g_CommandQueue (SL's wrapper) instead.
inline std::atomic<bool> dx12_hook_g_HadFSRFGPhase{false};

// Durable process-lifetime proof that PostSL completed at least one real,
// device-healthy submit. Unlike current-route confirmation, this survives an
// authoritative DLSS OFF/native return so a later pure-DLSS proxy can prewarm
// before its FG-off passthrough Present. Exact device/queue/backend guards are
// still revalidated at every handoff.
inline std::atomic<bool> dx12_hook_g_HadSuccessfulPostSLPhase{false};

// After FSR→DLSS→OFF: the swapchain's backbuffers have indeterminate GPU
// resource state from the FG pipeline teardown.  Direct rendering with
// explicit PRESENT→RENDER_TARGET barriers causes DEVICE_REMOVED when the
// barrier's StateBefore doesn't match the actual backbuffer state.  The
// offscreen compositing path (CopyTextureRegion + implicit state promotion)
// avoids ALL explicit barriers on the backbuffer, making it safe regardless
// of actual state.  Cleared on clean swapchain transition (non-FG).
inline std::atomic<bool> dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG{false};

// GPU drain: flush all in-flight GPU work before first overlay render after
// FSR→DLSS transition.  SL's FG pipeline may have concurrent backbuffer
// access that causes DEVICE_HUNG if we draw simultaneously.
inline bool dx12_hook_g_NeedGPUDrainBeforeRender = false;

inline ID3D12Fence* dx12_hook_g_DrainFence = nullptr;

inline HANDLE dx12_hook_g_DrainEvent = nullptr;

inline UINT64 dx12_hook_g_DrainFenceValue = 0;

// Post-FSR graduated probe system: after FSR→DLSS transition, incrementally test
// what rendering operations are safe before committing to full overlay render.
inline std::atomic<int> dx12_hook_g_PostFSRProbeLevel{0};  // 0=scratch, 1=reserved, 2=offscreen-copy-only, 3=full allowed

inline std::atomic<int> dx12_hook_g_PostFSRProbeFrames{0};

inline constexpr int dx12_hook_kPostFSRProbeFramesPerLevel = 3;

inline bool dx12_hook_g_PostFSRDescFreeRecreated = false;

// Dedicated queue created specifically for PostSL overlay rendering.
// After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
// the game's original queue can become corrupted (null pointer dereference
// in nvwgf2umx during ECL).  A fresh queue bypasses this corruption.
// Created once on first PostSL need, re-created after FG transitions.
inline ID3D12CommandQueue* dx12_hook_g_PostSLDedicatedQueue = nullptr;

// Last queue that PostSL successfully submitted to without DEVICE_REMOVED.
// Survives FG transitions — used as preferred queue when PostSL re-activates
// after FSR→DLSS switch (where g_CommandQueue or scQueue might be wrong).
// AddRef'd to prevent use-after-free when g_CommandQueue is updated.
// NOTE: this pointer is read by the ECL detour concurrently with FG-mode
// switches that write it.  The writer (SetPostSLLastWorkingQueue) uses
// exchange + AddRef/Release so the new pointer is alive before the old is
// dropped.  ECL readers should grab a local AddRef'd copy; the PostSL path
// runs under the lifecycle epoch guard which prevents concurrent release.
inline ID3D12CommandQueue* dx12_hook_g_PostSLLastWorkingQueue = nullptr;

inline void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue) {
    if (queue == dx12_hook_g_PostSLLastWorkingQueue)
        return;
    if (queue)
        queue->AddRef();
    if (dx12_hook_g_PostSLLastWorkingQueue)
        dx12_hook_g_PostSLLastWorkingQueue->Release();
    dx12_hook_g_PostSLLastWorkingQueue = queue;
}

// File-scope scene transition cooldown.  Set by ProcessFrame when a large
// frametime gap is detected during FG, checked by PostSLOverlayRender to
// suppress overlay during scene loads.
inline std::atomic<int> dx12_hook_g_SceneTransitionCooldown{0};

// Captures ProcessFrame's resolved gameQueue right before FG transitions ON.
// When PostSL activates after FG, g_CommandQueue may have been polluted by
// SL's internal FG queues and g_SwapchainQueue may be null (e.g., Talos).
// This provides a reliable fallback to the game's real queue.
inline ID3D12CommandQueue* dx12_hook_g_PreFGGameQueue = nullptr;

// The game's very first command queue, captured before any FG ever activates.
// During FG transitions, ALL queue sources can get polluted by SL/FSR internal
// queues (g_CommandQueue from ECL hook, g_SwapchainQueue from CreateSwapChain
// called by SL).  The original queue is the only one guaranteed to be the game's
// own queue and works reliably for PostSL overlay during DLSS FG.
inline ID3D12CommandQueue* dx12_hook_g_OriginalGameQueue = nullptr;

// File-scope FG transition cooldown.  Counts down frames after any FG mode
// change (on/off, FSR→DLSS, SL signal change).  While >0, overlay reinit is
// suppressed so that SL / FSR FG can finish initializing without interference.
// Set by ProcessFrame's FG transition detection; checked by swapchain-change
// invalidation to prevent premature overlay reinit during FG mode switches.
inline std::atomic<int> dx12_hook_g_FGTransitionCooldown{0};

// Counts frames since FG was last active.  When a game switches FG modes
// (e.g., FSR FG → DLSS FG), FG heuristics may go inactive for many frames
// before the swapchain actually changes.  A simple "was active last frame"
// flag misses cases where the gap is >1 frame.  This counter lets the
// swapchain-change logic detect that FG was active recently (within a window)
// even if several "inactive" frames have elapsed.
inline int dx12_hook_g_FramesSinceFGActive = 9999;

class DX12DescFreeBackend : public CustomOverlay::RendererBackend {
public:
    ~DX12DescFreeBackend() override {
        Shutdown();
    }

    // Non-virtual: create device-dependent resources (root sig, PSOs)
    bool InitDevice(ID3D12Device* dev, DXGI_FORMAT rtvFormat) {
        if (deviceReady_)
            return true;
        device_ = dev;
        rtvFormat_ = rtvFormat;
        if (!CreateRootSignature() || !CreatePSOs()) {
            Shutdown();
            return false;
        }
        if (!CreateBuffers()) {
            Shutdown();
            return false;
        }
        deviceReady_ = true;
        return true;
    }

    // RendererBackend: stage font atlas for a descriptor-free structured uint buffer.
    // The pixel shader samples from a DEFAULT-heap buffer; reading a UPLOAD heap
    // directly in the text draw has proven fragile on the x86 NVIDIA path.
    bool Initialize(int fontWidth, int fontHeight, const uint8_t* fontData) override {
        if (!device_ || !fontData)
            return false;
        fontWidth_ = fontWidth;
        fontHeight_ = fontHeight;

        const size_t dataSize = (size_t)fontWidth * fontHeight * 4;  // RGBA8
        fontBufferSize_ = dataSize;

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = dataSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        // D3D12 ignores non-COMMON initial states for buffers.  Start in the
        // real state and record explicit transitions around the one-time copy;
        // relying on implicit COMMON promotion before sampling the same command
        // list was fragile on the x86 NVIDIA path.
        HRESULT hr = device_->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&fontBuffer_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: font default buffer create failed hr=0x%08X", hr);
            return false;
        }
        fontBuffer_->SetName(L"CE_DescFreeFontDefaultBuffer");

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = device_->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&fontUploadBuffer_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: font upload buffer create failed hr=0x%08X", hr);
            fontBuffer_->Release();
            fontBuffer_ = nullptr;
            return false;
        }
        fontUploadBuffer_->SetName(L"CE_DescFreeFontUploadBuffer");

        void* mapped = nullptr;
        D3D12_RANGE readRange = {0, 0};
        hr = fontUploadBuffer_->Map(0, &readRange, &mapped);
        if (FAILED(hr)) {
            HookLogImportant("DescFree: font upload buffer map failed hr=0x%08X", hr);
            fontUploadBuffer_->Release();
            fontUploadBuffer_ = nullptr;
            fontBuffer_->Release();
            fontBuffer_ = nullptr;
            return false;
        }
        memcpy(mapped, fontData, dataSize);
        fontUploadBuffer_->Unmap(0, nullptr);

        fontGpuAddr_ = fontBuffer_->GetGPUVirtualAddress();
        fontUploadPending_ = true;
        HookLogImportant("DescFree: font structured buffer ready (%dx%d, %zu bytes, gpu=0x%llX)", fontWidth, fontHeight,
                         dataSize, (unsigned long long)fontGpuAddr_);
        return true;
    }

    void Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<CustomOverlay::DrawCommand>& commands, int vpW, int vpH) override {
        auto* cmdList = dx12_hook_s_descFreeCmdList;
        if (!cmdList || !deviceReady_ || !fontBuffer_ || vertices.empty())
            return;
        if (fontUploadPending_ && !fontUploadBuffer_)
            return;

        // Rebind the per-slot GPU-completion fence.  If the fence object changed
        // (overlay reinit recreates g_State.fence), the recorded guard values
        // belong to a dead fence — discard them so we never wait on a stale or
        // released fence.
        if (dx12_hook_s_descFreeSlotFence != slotFence_) {
            for (int i = 0; i < kPoolSize; ++i)
                slotFenceValue_[i] = 0;
            slotFence_ = dx12_hook_s_descFreeSlotFence;
        }

        // Upload vertex data
        int slot = frameIdx_ % kPoolSize;
        frameIdx_++;

        // If a caller published a slot guard, block until the GPU has finished
        // the previous frame that used this ring slot before overwriting it.
        if (!WaitForSlotGpuComplete(slot)) {
            return;
        }

        size_t vbBytes = vertices.size() * sizeof(CustomOverlay::DrawVertex);
        if (vbBytes > vbSize_[slot]) {
            if (!ResizeBuffer(vb_[slot], vbPtr_[slot], vbSize_[slot], vbBytes))
                return;
        }
        memcpy(vbPtr_[slot], vertices.data(), vbBytes);

        // Upload index data
        size_t ibBytes = indices.size() * sizeof(uint16_t);
        if (ibBytes > ibSize_[slot]) {
            if (!ResizeBuffer(ib_[slot], ibPtr_[slot], ibSize_[slot], ibBytes))
                return;
        }
        memcpy(ibPtr_[slot], indices.data(), ibBytes);

        // Set pipeline — NO SetDescriptorHeaps!
        cmdList->SetGraphicsRootSignature(rootSig_);

        if (ce::dx12_overlay_policy::ShouldRecordDescFreeFontUpload(fontUploadPending_, fontBuffer_ != nullptr,
                                                                    fontUploadBuffer_ != nullptr)) {
            D3D12_RESOURCE_BARRIER fontToCopy = {};
            fontToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fontToCopy.Transition.pResource = fontBuffer_;
            fontToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            fontToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            fontToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &fontToCopy);

            cmdList->CopyBufferRegion(fontBuffer_, 0, fontUploadBuffer_, 0, fontBufferSize_);

            D3D12_RESOURCE_BARRIER fontBarrier = {};
            fontBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fontBarrier.Transition.pResource = fontBuffer_;
            fontBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            fontBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            fontBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &fontBarrier);

            fontUploadPending_ = false;
            HookLogImportant("DescFree: font upload recorded to default buffer (%zu bytes, gpu=0x%llX)",
                             fontBufferSize_, (unsigned long long)fontGpuAddr_);
        }

        // Root constants: viewportW, viewportH, hdrMode, paperWhiteNits, fontW, fontH
        float constants[6] = {(float)vpW,     (float)vpH,        (float)hdrMode,
                              paperWhiteNits, (float)fontWidth_, (float)fontHeight_};
        cmdList->SetGraphicsRoot32BitConstants(0, 6, constants, 0);

        // Root SRV: font buffer (StructuredBuffer<uint> at t0)
        cmdList->SetGraphicsRootShaderResourceView(1, fontGpuAddr_);

        // Render target + viewport
        cmdList->OMSetRenderTargets(1, &dx12_hook_s_descFreeRtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = {0, 0, (float)vpW, (float)vpH, 0, 1};
        D3D12_RECT scissor = {0, 0, (LONG)vpW, (LONG)vpH};
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        // Vertex/index buffers
        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = vb_[slot]->GetGPUVirtualAddress();
        vbv.SizeInBytes = (UINT)vbBytes;
        vbv.StrideInBytes = sizeof(CustomOverlay::DrawVertex);
        cmdList->IASetVertexBuffers(0, 1, &vbv);

        D3D12_INDEX_BUFFER_VIEW ibv = {};
        ibv.BufferLocation = ib_[slot]->GetGPUVirtualAddress();
        ibv.SizeInBytes = (UINT)ibBytes;
        ibv.Format = DXGI_FORMAT_R16_UINT;
        cmdList->IASetIndexBuffer(&ibv);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Draw
        ID3D12PipelineState* lastPSO = nullptr;
        {
            static std::atomic<int> s_commandDetailLog{0};
            const int logFrame = s_commandDetailLog.fetch_add(1, std::memory_order_relaxed);
            if (logFrame < 6) {
                for (size_t cmdIndex = 0; cmdIndex < commands.size(); ++cmdIndex) {
                    const auto& cmd = commands[cmdIndex];
                    HookLogImportant(
                        "DX12 DIAG: DescFree command frame=%d cmd=%zu textured=%d vtxOff=%u vtxCount=%u idxOff=%u "
                        "idxCount=%u",
                        logFrame, cmdIndex, cmd.useTexture ? 1 : 0, cmd.vertexOffset, cmd.vertexCount, cmd.indexOffset,
                        cmd.indexCount);
                }
            }
        }
        for (const auto& cmd : commands) {
            auto* pso = cmd.useTexture ? psoTextured_ : psoSolid_;
            if (pso != lastPSO) {
                cmdList->SetPipelineState(pso);
                lastPSO = pso;
            }
            cmdList->DrawIndexedInstanced(cmd.indexCount, 1, cmd.indexOffset, 0, 0);
        }
        slotFenceValue_[slot] = dx12_hook_s_descFreeSlotGuardValue;

        // DIAGNOSTIC: per-frame overlay GPU footprint (draw count + vertex/index bytes). This is
        // what CE submits to the app's queue each frame; compare 32-bit vs 64-bit hook_debug.log
        // (expected identical — confirming the freeze is WoW64 allocation speed, not a different
        // CE code path — and quantifying how much a cached-texture composite would save).
        {
            static std::atomic<int> s_overlayFootprintLog{0};
            const int n = s_overlayFootprintLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 5 || (n % 600) == 0) {
                HookLogImportant("DX12 DIAG: overlay footprint draws=%zu vbBytes=%zu ibBytes=%zu slot=%d sample=%d",
                                 commands.size(), vbBytes, ibBytes, slot, n);
            }
        }
    }

    void Shutdown() override {
        // During process termination, D3D12/NVIDIA driver may be partially torn down.
        // Skip GPU resource cleanup to avoid access violations in driver code.
        if (IsProcessTerminating())
            return;
        for (int i = 0; i < kPoolSize; i++) {
            if (vb_[i]) {
                vb_[i]->Unmap(0, nullptr);
                vb_[i]->Release();
                vb_[i] = nullptr;
            }
            if (ib_[i]) {
                ib_[i]->Unmap(0, nullptr);
                ib_[i]->Release();
                ib_[i] = nullptr;
            }
            vbPtr_[i] = nullptr;
            ibPtr_[i] = nullptr;
            vbSize_[i] = 0;
            ibSize_[i] = 0;
        }
        if (fontBuffer_) {
            fontBuffer_->Release();
            fontBuffer_ = nullptr;
        }
        if (fontUploadBuffer_) {
            fontUploadBuffer_->Release();
            fontUploadBuffer_ = nullptr;
        }
        if (psoTextured_) {
            psoTextured_->Release();
            psoTextured_ = nullptr;
        }
        if (psoSolid_) {
            psoSolid_->Release();
            psoSolid_ = nullptr;
        }
        if (rootSig_) {
            rootSig_->Release();
            rootSig_ = nullptr;
        }
        fontGpuAddr_ = 0;
        fontBufferSize_ = 0;
        fontUploadPending_ = false;
        deviceReady_ = false;
        // Drop the (non-owning) slot fence binding and guards; a fresh InitDevice
        // rebinds via the published static, and the GPU work that referenced this
        // backend's ring is gone.
        slotFence_ = nullptr;
        for (int i = 0; i < kPoolSize; ++i)
            slotFenceValue_[i] = 0;
    }

private:
    bool CreateRootSignature() {
        // Parameter 0: 6 root constants at b0
        //   [0-1] viewportSize, [2] hdrMode, [3] paperWhiteNits, [4-5] fontTexSize
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 6;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Parameter 1: root SRV at t0 (font StructuredBuffer<uint>)
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace = 0;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 0;  // No sampler needed — manual bilinear in shader
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
        if (FAILED(hr)) {
            HookLogImportant("DescFree: SerializeRootSignature failed hr=0x%08X", hr);
            if (err)
                err->Release();
            return false;
        }
        hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig_));
        blob->Release();
        if (err)
            err->Release();
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreateRootSignature failed hr=0x%08X", hr);
            return false;
        }
        return true;
    }

    bool CreatePSOs() {
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = {inputLayout, 3};
        psoDesc.pRootSignature = rootSig_;
        psoDesc.VS = {g_VS_5_0, sizeof(g_VS_5_0)};
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        // Alpha blending
        D3D12_RENDER_TARGET_BLEND_DESC& blendRT = psoDesc.BlendState.RenderTarget[0];
        blendRT.BlendEnable = TRUE;
        blendRT.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendRT.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendRT.BlendOp = D3D12_BLEND_OP_ADD;
        blendRT.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendRT.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendRT.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendRT.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = rtvFormat_;
        psoDesc.SampleDesc.Count = 1;

        // Textured PSO — uses StructuredBuffer<uint> (descriptor-free)
        psoDesc.PS = {g_PS_Textured_DescFree_5_0, sizeof(g_PS_Textured_DescFree_5_0)};
        HRESULT hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoTextured_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreatePSO(textured) failed hr=0x%08X", hr);
            return false;
        }

        // Solid PSO — no texture, uses same root sig (t0 unused)
        psoDesc.PS = {g_PS_Solid_5_0, sizeof(g_PS_Solid_5_0)};
        hr = device_->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&psoSolid_));
        if (FAILED(hr)) {
            HookLogImportant("DescFree: CreatePSO(solid) failed hr=0x%08X", hr);
            return false;
        }

        HookLogImportant("DescFree: PSOs created (fmt=%d)", rtvFormat_);
        return true;
    }

    bool CreateBuffers() {
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_RANGE readRange = {0, 0};
        for (int i = 0; i < kPoolSize; i++) {
            rd.Width = kInitVBBytes;
            HRESULT hr = device_->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vb_[i]));
            if (FAILED(hr))
                return false;
            vb_[i]->Map(0, &readRange, &vbPtr_[i]);
            vbSize_[i] = kInitVBBytes;

            rd.Width = kInitIBBytes;
            hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                  nullptr, IID_PPV_ARGS(&ib_[i]));
            if (FAILED(hr))
                return false;
            ib_[i]->Map(0, &readRange, &ibPtr_[i]);
            ibSize_[i] = kInitIBBytes;
        }
        return true;
    }

    bool ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed) {
        size_t newSize = curSize;
        while (newSize < needed)
            newSize *= 2;

        buf->Unmap(0, nullptr);
        buf->Release();

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = newSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                      nullptr, IID_PPV_ARGS(&buf));
        if (FAILED(hr))
            return false;

        D3D12_RANGE readRange = {0, 0};
        buf->Map(0, &readRange, &ptr);
        curSize = newSize;
        return true;
    }

    bool WaitForSlotGpuComplete(int slot) {
        if (!slotFence_ || slot < 0 || slot >= kPoolSize) {
            return true;

        }
        const UINT64 guardValue = slotFenceValue_[slot];
        if (!ce::dx12_overlay_policy::ShouldWaitForOverlayUploadSlot(guardValue, slotFence_->GetCompletedValue())) {
            return true;
        }
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            return false;
        }
        bool completed = false;
        if (SUCCEEDED(slotFence_->SetEventOnCompletion(guardValue, eventHandle))) {
            // The fence is the real synchronization that closes the CPU<->GPU
            // UPLOAD-buffer data race.  The bounded timeout is purely a liveness
            // safety net: a separate code path (FG transition / overlay reinit)
            // may legitimately discard the pending Signal for this guard value,
            // which would otherwise wedge the present thread forever.  On timeout
            // we skip this overlay draw; reusing the slot would corrupt in-flight
            // GPU reads and can turn a transient mode switch into DEVICE_HUNG.
            const DWORD waitResult = WaitForSingleObject(eventHandle, kSlotWaitTimeoutMs);
            completed = waitResult == WAIT_OBJECT_0;
            if (!completed) {
                static std::atomic<int> s_slotWaitTimeoutLog{0};
                const int logN = s_slotWaitTimeoutLog.fetch_add(1, std::memory_order_relaxed);
                if (logN < 40 || (logN % 200) == 0) {
                    HookLogImportant(
                        "DescFree: slot %d GPU-completion wait %s (guard=%llu completed=%llu) — overlay upload ring "
                        "draw skipped to avoid reusing in-flight GPU data",
                        slot, waitResult == WAIT_TIMEOUT ? "timed out" : "failed", (unsigned long long)guardValue,
                        (unsigned long long)slotFence_->GetCompletedValue());
                }
            }
        }
        CloseHandle(eventHandle);
        return completed;
    }

    // Liveness bound for WaitForSlotGpuComplete (see comment there).  Must stay
    // below the ~2s GPU TDR; a normal Alt+Tab mode-switch pause resumes the GPU
    // in well under this, so the wait returns as soon as the slot is free.
    static constexpr DWORD kSlotWaitTimeoutMs = 1000;

    static constexpr int kPoolSize = 4;
    static constexpr size_t kInitVBBytes = 4096 * 20;  // 4096 vertices * 20 bytes
    static constexpr size_t kInitIBBytes = 8192 * 2;   // 8192 indices * 2 bytes

    ID3D12Device* device_ = nullptr;
    DXGI_FORMAT rtvFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool deviceReady_ = false;

    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* psoTextured_ = nullptr;
    ID3D12PipelineState* psoSolid_ = nullptr;

    ID3D12Resource* fontBuffer_ = nullptr;
    ID3D12Resource* fontUploadBuffer_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS fontGpuAddr_ = 0;
    size_t fontBufferSize_ = 0;
    bool fontUploadPending_ = false;
    int fontWidth_ = 0;
    int fontHeight_ = 0;

    ID3D12Resource* vb_[kPoolSize] = {};
    ID3D12Resource* ib_[kPoolSize] = {};
    void* vbPtr_[kPoolSize] = {};
    void* ibPtr_[kPoolSize] = {};
    size_t vbSize_[kPoolSize] = {};
    size_t ibSize_[kPoolSize] = {};
    ID3D12Fence* slotFence_ = nullptr;
    UINT64 slotFenceValue_[kPoolSize] = {};
    int frameIdx_ = 0;
};

inline DX12DescFreeBackend* dx12_hook_g_DescFreeBackend = nullptr;

// --- DX12 Overlay State Management ---
struct DX12OverlayState {
    // Large pool size ensures we never need to wait for GPU.
    // Even at 60fps with 100ms GPU latency, only 6 allocators are in flight.
    // 16 provides 2.5x headroom - allocator is always ready, zero waiting.
    static const int ALLOC_POOL_SIZE = 16;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool overlayInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;

    // Offscreen render target for overlay compositing.
    // Avoids OMSetRenderTargets(swapchain) + SetDescriptorHeaps on the same ECL,
    // which causes GPU pipeline stalls (~40% utilization loss) in some games.
    // Flow: copy backbuffer → offscreen, render overlay → offscreen, copy back.
    ID3D12Resource* offscreenRT = nullptr;
    ID3D12DescriptorHeap* offscreenRtvHeap = nullptr;
    UINT offscreenWidth = 0;
    UINT offscreenHeight = 0;
    DXGI_FORMAT offscreenFormat = DXGI_FORMAT_UNKNOWN;

    // Dedicated overlay command queue for FG-safe rendering.
    // When FG is active, overlay commands execute on this queue with CPU-side
    // fence synchronization to avoid interfering with Streamline's game queue
    // management.  When FG is not active, overlay commands go on the game queue
    // directly (zero CPU waits).
    ID3D12CommandQueue* overlayQueue = nullptr;

    // Cross-queue fence: game queue signals to mark work completion, then
    // CPU-side wait before submitting overlay work on the overlay queue.
    // GPU-side CommandQueue::Wait was removed (NVIDIA WaitImpl Alt+Tab hang).
    ID3D12Fence* crossQueueFence = nullptr;
    UINT64 crossQueueFenceValue = 0;
    HANDLE crossQueueFenceEvent = nullptr;

    IDXGISwapChain3* cachedSC3 = nullptr;  // cached from first successful QI

    // The device used to create sync resources (allocators, command list, fence).
    // Must match the submission queue's device — cross-device submission = DEVICE_REMOVED.
    ID3D12Device* syncDevice = nullptr;

    // D3D11On12 overlay bridge: renders overlay via D3D11 on top of the D3D12
    // backbuffer.  D3D11 doesn't use descriptor heaps, avoiding the NVIDIA
    // driver stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain).
    ID3D11Device* d3d11on12Device = nullptr;
    ID3D11DeviceContext* d3d11on12Context = nullptr;
    ID3D11On12Device* d3d11on12 = nullptr;
    std::vector<ID3D11Resource*> d3d11WrappedBBs;
    std::vector<ID3D11RenderTargetView*> d3d11RTVs;
    bool d3d11on12Init = false;

    void Cleanup() {
        // FG-SAFE: backBuffers no longer holds references
        backBuffers.clear();
        if (offscreenRT) {
            offscreenRT->Release();
            offscreenRT = nullptr;
        }
        if (offscreenRtvHeap) {
            offscreenRtvHeap->Release();
            offscreenRtvHeap = nullptr;
        }
        offscreenWidth = 0;
        offscreenHeight = 0;
        offscreenFormat = DXGI_FORMAT_UNKNOWN;
        if (rtvDescHeap) {
            rtvDescHeap->Release();
            rtvDescHeap = nullptr;
        }
        if (srvDescHeap) {
            srvDescHeap->Release();
            srvDescHeap = nullptr;
        }
        for (auto* alloc : allocators)
            if (alloc)
                alloc->Release();
        allocators.clear();
        if (cmdList) {
            cmdList->Release();
            cmdList = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        // CRITICAL: Release dedicated overlay queue
        if (overlayQueue) {
            overlayQueue->Release();
            overlayQueue = nullptr;
        }
        // Release cross-queue synchronization fence and event
        if (crossQueueFenceEvent) {
            CloseHandle(crossQueueFenceEvent);
            crossQueueFenceEvent = nullptr;
        }
        if (crossQueueFence) {
            crossQueueFence->Release();
            crossQueueFence = nullptr;
        }
        overlayInit = false;
        syncInit = false;
        crossQueueFenceValue = 0;
        cachedSC3 = nullptr;  // weak ref, no Release needed
        // D3D11On12 cleanup
        for (auto* rtv : d3d11RTVs)
            if (rtv)
                rtv->Release();
        d3d11RTVs.clear();
        for (auto* res : d3d11WrappedBBs)
            if (res)
                res->Release();
        d3d11WrappedBBs.clear();
        if (d3d11on12) {
            d3d11on12->Release();
            d3d11on12 = nullptr;
        }
        if (d3d11on12Context) {
            d3d11on12Context->Release();
            d3d11on12Context = nullptr;
        }
        if (d3d11on12Device) {
            d3d11on12Device->Release();
            d3d11on12Device = nullptr;
        }
        d3d11on12Init = false;
    }
};

inline DX12OverlayState dx12_hook_g_State;

// ============================================================
// Steam ECL deferred overlay submission state
// ============================================================
// Set by DetourPresent (dxgi_shared.cpp) before invoking Steam's overlay handler.
// When true, ProcessFrame records overlay commands into g_State.cmdList and closes
// the list, but skips the actual ECL submission.  The submission is deferred to
// DetourExecuteCommandLists, which fires the CE overlay ECL AFTER Steam's overlay
// ECL has been submitted to the queue.  This ensures CE's overlay renders on top
// of Steam's cleared backbuffer instead of being cleared by Steam.
inline bool dx12_hook_g_deferOverlaySubmitToSteamECL = false;

struct SteamDeferredOverlaySubmitState {
    ID3D12CommandList* cmdList = nullptr;
    int allocIdx = -1;
    ID3D12CommandQueue* eclQueue = nullptr;
    bool pending = false;
};

inline SteamDeferredOverlaySubmitState dx12_hook_g_steamDeferredOverlay;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline SharedCaptureD3D12 dx12_hook_g_SharedCaptureD3D12;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline OverlayAdapter dx12_hook_g_D3D11On12Adapter;

// Separate overlay adapter for D3D11On12 rendering during Streamline FG.
// Uses the DX11 backend via D3D11On12 bridge to properly manage cross-queue
// resource transitions, which SL's FG pipeline can track.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline OverlayAdapter dx12_hook_g_SLFGAdapter;

// Native/runtime-owned FSR present-callback rendering must not inherit stale
// normal/DLSS DX12 backend state across later FFX-owned swapchain/device handoffs.
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline OverlayAdapter dx12_hook_g_FFXPresentOverlayAdapter;

inline ID3D12Device* dx12_hook_g_FFXPresentOverlayDevice = nullptr;

inline DXGI_FORMAT dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;

// Live backbuffer geometry cached from the FG-enable ffxConfigure swapchain (see
// DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain). Used to size CE's substitute UI texture and to
// size-classify the game's registered no-callback FSR FG UI texture (usable HUD surface vs degenerate
// placeholder). Declared here (before that helper) so the cache write can reference them.
inline std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferWidth{0};

inline std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferHeight{0};

inline std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferFormat{0};  // DXGI_FORMAT

// Device/format the descriptor-free backend was built for. The backend is
// DEVICE-scoped (PSOs, root signature, font buffer, vb/ib upload pool; the
// backbuffer is fetched per frame) and intentionally survives swapchain
// teardown so the first present of a new swapchain can draw without a backend
// rebuild. These trackers gate the only rebuild triggers: device or RTV
// format change.
inline ID3D12Device* dx12_hook_g_DescFreeBackendDevice = nullptr;

inline DXGI_FORMAT dx12_hook_g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;

inline void ShutdownDescFreeBackend(const char* reason, bool shutdownMode = false) {
    dx12_hook_g_DescFreeBackendDevice = nullptr;
    dx12_hook_g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;
    DX12DescFreeBackend* backend = dx12_hook_g_DescFreeBackend;
    const bool adapterInitialized = dx12_hook_g_D3D11On12Adapter.IsInitialized();
    if (!backend && !adapterInitialized) {
        return;
    }

    CustomOverlay::RendererBackend* adapterBackend = adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackend() : nullptr;
    const OverlayBackendType adapterType =
        adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackendType() : OverlayBackendType::None;
    const bool adapterOwnsBackend = backend && adapterBackend == backend;

    if (ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(backend != nullptr, adapterInitialized,
                                                                                adapterOwnsBackend)) {
        HookLogImportant(
            "DX12: Shutting down adapter-owned DescFree backend (reason=%s backend=%p adapterBackend=%p "
            "adapterType=%d shutdownMode=%d)",
            reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
        if (shutdownMode) {
            dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
        }
        dx12_hook_g_D3D11On12Adapter.Shutdown();
        dx12_hook_g_DescFreeBackend = nullptr;
        return;
    }

    if (adapterInitialized) {
        HookLogImportant(
            "DX12: Shutting down DX12 overlay adapter without tracked DescFree ownership "
            "(reason=%s backend=%p adapterBackend=%p adapterType=%d shutdownMode=%d)",
            reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
        if (shutdownMode) {
            dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
        }
        dx12_hook_g_D3D11On12Adapter.Shutdown();
    }

    if (backend) {
        HookLogImportant("DX12: Shutting down standalone DescFree backend (reason=%s backend=%p)",
                         reason ? reason : "unknown", backend);
        backend->Shutdown();
        delete backend;
        if (dx12_hook_g_DescFreeBackend == backend) {
            dx12_hook_g_DescFreeBackend = nullptr;
        }
    }
}

// Lazily (re)builds the device-scoped descriptor-free overlay backend for the
// requested device/format pair. A live backend is reused as-is when both
// match (the warm path that closes the first-present blank after FG
// transitions); a device or format change is the only rebuild trigger.
// Returns true when a ready backend is bound to (device, format).
inline bool EnsureDescFreeBackendForDeviceAndFormat(ID3D12Device* dev, DXGI_FORMAT format, const char* context) {
    if (!dev) {
        return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
    }
    if (dx12_hook_g_DescFreeBackend && (dx12_hook_g_DescFreeBackendDevice != dev || dx12_hook_g_DescFreeBackendFormat != format)) {
        HookLogImportant("DX12: DescFree backend stale (device %p->%p fmt %d->%d) — rebuilding (%s)",
                         dx12_hook_g_DescFreeBackendDevice, dev, static_cast<int>(dx12_hook_g_DescFreeBackendFormat),
                         static_cast<int>(format), context ? context : "unknown");
        ShutdownDescFreeBackend(context);
    }
    if (!dx12_hook_g_DescFreeBackend) {
        auto* backend = new DX12DescFreeBackend();
        if (backend->InitDevice(dev, format)) {
            dx12_hook_g_DescFreeBackend = backend;
            dx12_hook_g_DescFreeBackendDevice = dev;
            dx12_hook_g_DescFreeBackendFormat = format;
            dx12_hook_g_D3D11On12Adapter.InitCustom(dx12_hook_g_DescFreeBackend, OverlayBackendType::DX12);
            HookLogImportant("DX12: Descriptor-free overlay backend ready (%s, device=%p fmt=%d)",
                             context ? context : "unknown", dev, static_cast<int>(format));
        } else {
            delete backend;
            HookLogImportant("DX12: Descriptor-free backend init FAILED (%s, fmt=%d)", context ? context : "unknown",
                             static_cast<int>(format));
        }
    }
    return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
}

inline ID3D12Resource* dx12_hook_g_OverlayBcBuffer = nullptr;

inline volatile uint32_t* dx12_hook_g_OverlayBcMapped = nullptr;

inline D3D12_GPU_VIRTUAL_ADDRESS dx12_hook_g_OverlayBcGpuVA = 0;

inline std::atomic<uint32_t> dx12_hook_g_OverlayBcSeq{0};

inline void EnsureOverlayBreadcrumbBuffer(ID3D12Device* device) {
    if (dx12_hook_g_OverlayBcBuffer || !device) {
        return;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;  // system memory, CPU-cached, GPU-writable
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(kOverlayBcSlotCount) * sizeof(uint32_t);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buf = nullptr;
    HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&buf));
    if (FAILED(hr) || !buf) {
        static std::atomic<int> s_bcCreateFailLog{0};
        if (s_bcCreateFailLog.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DX12: Overlay GPU breadcrumb buffer create failed hr=0x%08X", (unsigned)hr);
        }
        return;
    }
    void* mapped = nullptr;
    if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) {
        buf->Release();
        return;
    }
    memset(mapped, 0, static_cast<size_t>(rd.Width));
    dx12_hook_g_OverlayBcMapped = static_cast<volatile uint32_t*>(mapped);
    dx12_hook_g_OverlayBcGpuVA = buf->GetGPUVirtualAddress();
    dx12_hook_g_OverlayBcBuffer = buf;
    HookLogImportant("DX12: Overlay GPU breadcrumb buffer armed (slots=%d gpuVA=0x%llX)",
                     static_cast<int>(kOverlayBcSlotCount), static_cast<unsigned long long>(dx12_hook_g_OverlayBcGpuVA));
}

// Call once per overlay submit (before recording) to bump the sequence the GPU will stamp into each slot.
inline void BeginOverlayGpuBreadcrumbFrame(ID3D12Device* device) {
    EnsureOverlayBreadcrumbBuffer(device);
    if (dx12_hook_g_OverlayBcMapped) {
        dx12_hook_g_OverlayBcSeq.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void WriteOverlayGpuBreadcrumb(ID3D12GraphicsCommandList* list, OverlayGpuBreadcrumbOp op) {
    if (!list || !dx12_hook_g_OverlayBcMapped || dx12_hook_g_OverlayBcGpuVA == 0 || op == 0 || op >= kOverlayBcSlotCount) {
        return;
    }
    ID3D12GraphicsCommandList2* list2 = nullptr;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) {
        return;
    }
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = {};
    param.Dest = dx12_hook_g_OverlayBcGpuVA + static_cast<UINT64>(op) * sizeof(uint32_t);
    param.Value = dx12_hook_g_OverlayBcSeq.load(std::memory_order_relaxed);
    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list2->WriteBufferImmediate(1, &param, &mode);
    list2->Release();
}

// CRITICAL FIX: Thread-safe accessors for g_Device and g_CommandQueue
// These functions acquire the mutex and return a reference-counted pointer
// to prevent use-after-free when the queue/device is destroyed on another
// thread
struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

    DX12Context(ID3D12Device* d, ID3D12CommandQueue* q) : device(d), queue(q) {
        if (device)
            device->AddRef();
        if (queue)
            queue->AddRef();
    }

    ~DX12Context() {
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (queue) {
            queue->Release();
            queue = nullptr;
        }
    }

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
    DX12Context(DX12Context&& other) noexcept : device(other.device), queue(other.queue) {
        other.device = nullptr;
        other.queue = nullptr;
    }

    DX12Context& operator=(DX12Context&& other) noexcept {
        if (this != &other) {
            if (device)
                device->Release();
            if (queue)
                queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

    bool IsValid() const {
        return device != nullptr && queue != nullptr;
    }
};

inline DX12Context GetDX12PrerenderContext(bool preferOriginalGameQueue, bool* usesOriginalGameQueue,
                                           ID3D12CommandQueue** currentQueueSnapshot) {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* currentQueue = g_CommandQueue.load(std::memory_order_acquire);
    const bool useOriginalGameQueue = preferOriginalGameQueue && dx12_hook_g_OriginalGameQueue != nullptr;
    ID3D12CommandQueue* selectedQueue = useOriginalGameQueue ? dx12_hook_g_OriginalGameQueue : currentQueue;
    if (usesOriginalGameQueue) {
        *usesOriginalGameQueue = useOriginalGameQueue;
    }
    if (currentQueueSnapshot) {
        *currentQueueSnapshot = currentQueue;
    }
    if (!selectedQueue) {
        return {};
    }

    // Streamline can use a second D3D12 device. Derive the fence device from
    // the selected queue instead of pairing origGame with a volatile runtime
    // device pointer.
    ID3D12Device* queueDevice = nullptr;
    const HRESULT deviceHr = selectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (FAILED(deviceHr) || !queueDevice) {
        static std::atomic<int> s_prerenderQueueDeviceLogs{0};
        if (s_prerenderQueueDeviceLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender queue GetDevice failed queue=%p hr=0x%08X", selectedQueue, deviceHr);
        }
        return {};
    }
    DX12Context context(queueDevice, selectedQueue);
    queueDevice->Release();
    return context;
}

// Keep native driver limiters in sync with the DX12 device we already discover
// from queue/swapchain hooks. Ownership remains with the existing DX12 globals.
inline void DX12_PublishNativeLimiterDevice(ID3D12Device* device, ID3D12CommandQueue* queue, const char* source) {
    if (!device)
        return;

    g_ReflexLimiter.SetDevice(static_cast<IUnknown*>(device));

    bool ctxUpdated = false;
    bool ctxApiConflict = false;
    if (auto* ctx = ce::GetHookContext()) {
        std::lock_guard<std::mutex> ctxLock(ctx->initMutex);
        if (!ctx->shuttingDown.load(std::memory_order_acquire)) {
            if (ctx->activeAPI == ce::ActiveGraphicsAPI::None) {
                ctx->activeAPI = ce::ActiveGraphicsAPI::DX12;
            }
            if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
                ctx->graphicsData.dx12.device = device;
                ctx->graphicsData.dx12.commandQueue = queue;
                ctxUpdated = true;
            } else {
                ctxApiConflict = true;
            }
        }
    }

    static std::atomic<ID3D12Device*> s_lastPublishedDevice{nullptr};
    static std::atomic<ID3D12CommandQueue*> s_lastPublishedQueue{nullptr};
    static std::atomic<uint64_t> s_nativeLimiterPublishChangeCount{0};
    ID3D12Device* previousDevice = s_lastPublishedDevice.exchange(device, std::memory_order_acq_rel);
    ID3D12CommandQueue* previousQueue = s_lastPublishedQueue.exchange(queue, std::memory_order_acq_rel);
    const bool deviceChanged = previousDevice != device;
    const bool queueChanged = previousQueue != queue;
    if (deviceChanged || queueChanged) {
        const uint64_t changeCount = s_nativeLimiterPublishChangeCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (deviceChanged || changeCount <= 32 || (changeCount % 512) == 0) {
            HookLogImportant(
                "DX12: Published native limiter device from %s (device=%p queue=%p ctxUpdated=%d ctxApiConflict=%d "
                "deviceChanged=%d queueChanged=%d changeCount=%llu)",
                source && source[0] ? source : "unknown", device, queue, ctxUpdated ? 1 : 0, ctxApiConflict ? 1 : 0,
                deviceChanged ? 1 : 0, queueChanged ? 1 : 0, static_cast<unsigned long long>(changeCount));
        }
    }
}

inline std::atomic<int> dx12_hook_g_CommandListsExecutedThisFrame{0};

inline std::atomic<uint64_t> dx12_hook_g_FGDebugFrameCount{0};

inline std::atomic<int> dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak{0};

inline std::atomic<int> dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak{0};

inline std::atomic<bool> dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun{false};

inline std::atomic<bool> dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown{false};

// Set when native FSR contexts are destroyed while no fresh session replaced
// them. Together with the explicit-off latch this is the evidence that lets a
// later GAME-created swapchain creation end the runtime-owned teardown.
inline std::atomic<bool> dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain{false};

// Identity-only marker (raw pointer, never dereferenced) for the queue of the
// game-created swapchain that ended the runtime-owned native-FSR teardown.
// Lets the FG transition cooldown and the swapchain-change reinit guard skip
// blanking the overlay for the FSR->off recovery that already proved its
// present path.
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue{nullptr};

// Identity-only, one-Present proof for the game-created native swapchain that
// authoritatively replaced a retired DLSS/PostSL proxy after explicit FG OFF.
// It is never dereferenced and is consumed by the first matching Present.
inline std::atomic<IDXGISwapChain*> dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain{nullptr};

// Identity-only proof that the exact fresh Streamline proxy already owns a
// complete prewarmed RTV/sync backend. Its first matching Present consumes the
// proof instead of destroying that backend as an ordinary swapchain change.
inline std::atomic<IDXGISwapChain*> dx12_hook_g_PrewarmedPostSLHandoffSwapchain{nullptr};

inline std::atomic<bool> dx12_hook_g_NativeFSRStartupConfigureArmingPending{false};

inline std::atomic<bool> dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending{false};

inline std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips{0};

inline std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs{0};

inline std::atomic<ULONGLONG> dx12_hook_g_ProtectedOfficialFFXStartupBeginMs{0};

inline std::atomic<bool> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress{false};

inline std::atomic<ULONGLONG> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs{0};

inline std::atomic<bool> dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending{false};

inline std::mutex dx12_hook_g_DeferredOfficialFFXTakeoverMutex;

inline ID3D12CommandQueue* dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;

inline char dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[MAX_PATH] = {};

inline void ResetAuthoritativeFSRRealFrameOnlyStreak() {
    dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak.store(0, std::memory_order_release);
}

inline void ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak() {
    dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.store(0, std::memory_order_release);
}

inline void ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown() {
    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(false, std::memory_order_release);
}

inline bool HasResolvedOfficialFFXStartupPath() {
    return g_FGCompat.HasDirectFFXApiConfirmation() ||
           dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
}

inline void ResetProtectedOfficialFFXStartupProgressCounters() {
    dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
    dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
    dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(0, std::memory_order_release);
}

inline void ArmProtectedOfficialFFXStartupProgressTracking(const char* reason) {
    dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
    dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
    dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(GetTickCount64(), std::memory_order_release);
    HookLogImportant("DX12: Protected official FFX startup progress tracking armed (%s)",
                     reason && reason[0] ? reason : "unknown");
}

inline void ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
    dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.store(0, std::memory_order_release);
    if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared progress-resolved official FFX runtime-owned Present path assumption (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
}

inline void StoreDeferredOfficialFFXTakeoverSideEffects(ID3D12CommandQueue* queue, const char* modulePath,
                                                        const char* reason) {
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
        if (dx12_hook_g_DeferredOfficialFFXTakeoverQueue) {
            dx12_hook_g_DeferredOfficialFFXTakeoverQueue->Release();
            dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
        }
        if (queue) {
            queue->AddRef();
            dx12_hook_g_DeferredOfficialFFXTakeoverQueue = queue;
        }
        if (modulePath && modulePath[0]) {
            strncpy_s(dx12_hook_g_DeferredOfficialFFXTakeoverModulePath, sizeof(dx12_hook_g_DeferredOfficialFFXTakeoverModulePath),
                      modulePath, _TRUNCATE);
        } else {
            dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
        }
    }
    dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.store(true, std::memory_order_release);
    HookLogImportant(
        "DX12: Official FFX takeover side-effects staged until enabled ffxConfigure "
        "(queue=%p module=%s reason=%s)",
        queue, modulePath && modulePath[0] ? modulePath : "unknown", reason && reason[0] ? reason : "unknown");
}

inline ID3D12CommandQueue* ConsumeDeferredOfficialFFXTakeoverSideEffects(char* modulePathOut,
                                                                         size_t modulePathOutCount) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
    ID3D12CommandQueue* queue = dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
    dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, dx12_hook_g_DeferredOfficialFFXTakeoverModulePath, _TRUNCATE);
    }
    dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
    return queue;
}

inline ID3D12CommandQueue* ReferenceDeferredOfficialFFXTakeoverQueue() {
    if (!dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.load(std::memory_order_acquire)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
    if (!dx12_hook_g_DeferredOfficialFFXTakeoverQueue) {
        return nullptr;
    }

    dx12_hook_g_DeferredOfficialFFXTakeoverQueue->AddRef();
    return dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
}

inline void ClearDeferredOfficialFFXTakeoverSideEffects(const char* reason) {
    ID3D12CommandQueue* queue = nullptr;
    bool hadPending = dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_DeferredOfficialFFXTakeoverMutex);
        queue = dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
        dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;
        dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
    }
    if (queue) {
        queue->Release();
    }
    if (hadPending) {
        HookLogImportant("DX12: Cleared staged official FFX takeover side-effects (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
}

inline void ClearProtectedOfficialFFXStartupSwapchainPending(const char* reason) {
    if (dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared protected official FFX startup swapchain pass-through (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
    ResetProtectedOfficialFFXStartupProgressCounters();
}

inline void SetNativeFSRStartupConfigureArmingPending(bool pending, const char* reason) {
    const bool previous = dx12_hook_g_NativeFSRStartupConfigureArmingPending.exchange(pending, std::memory_order_acq_rel);
    if (previous != pending) {
        HookLogImportant("DX12: Native FSR startup configure arming %s (%s)", pending ? "pending" : "cleared",
                         reason && reason[0] ? reason : "unknown");
    }
}

// Primary game queue — set once from the first ECL call (always the game's queue,
// since the game creates its queue before any FG runtime).  Used to filter ECL
// counting: only game-queue ECL calls count toward frame classification.
// FG runtimes (FSR FG) create their own queues that share the vtable, so our ECL
// hook fires for them too.  Without this filter, interpolated frames look like
// real frames (similar ECL counts).
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_PrimaryGameQueue{nullptr};

inline std::atomic<bool> dx12_hook_g_KnownDLSSFGModuleSeen{false};

// Last swapchain reference for device change detection
inline IDXGISwapChain* dx12_hook_g_LastSwapChain = nullptr;

// Exact swapchain identity associated with the most recent successful
// g_SwapchainQueue capture. A global queue match is not evidence for some other
// concurrently live proxy swapchain.
inline std::atomic<IDXGISwapChain*> dx12_hook_g_LastSwapchainQueueCaptureSwapchain{nullptr};

// Raw identity only; never AddRef'd or dereferenced. This remembers the exact
// swapchain previously associated with the original Present queue so an
// existing native swapchain can return after FG without requiring a duplicate
// CreateSwapChain callback at that boundary.
inline std::atomic<IDXGISwapChain*> dx12_hook_g_LastProvenOriginalQueueSwapchain{nullptr};

inline void RememberOriginalQueueSwapchainIdentity(IDXGISwapChain* swapchain, const char* reason) {
    if (!swapchain) {
        return;
    }

    IDXGISwapChain* previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
    if (previous != swapchain) {
        previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.exchange(swapchain, std::memory_order_acq_rel);
        HookLogImportant("DX12: Remembered exact original-queue swapchain identity %p (previous=%p reason=%s)",
                         swapchain, previous, reason ? reason : "unspecified");
    }

    // The newest explicit association wins when one COM identity has served
    // both runtime and native routes at different points in its lifetime.
    IDXGISwapChain* expectedPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (expectedPostSLSwapchain == swapchain &&
        dx12_hook_g_LastSuccessfulPostSLSwapchain.compare_exchange_strong(expectedPostSLSwapchain, nullptr,
                                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
        HookLogImportant("DX12: Original-queue association superseded remembered PostSL ownership for swapchain %p",
                         swapchain);
    }
}

// Pending swapchain cleanup - released after ResizeBuffers completes
inline IDXGISwapChain* dx12_hook_g_PendingSwapChainCleanup = nullptr;

// Native-FSR callback rendering can outlive the last normal live swapchain COM object.
// Cache the last trusted live-swapchain HDR decision so the callback thread does not
// need to probe DXGI output state through a weak raw swapchain pointer after takeover.
inline std::atomic<bool> dx12_hook_g_LastKnownSwapchainHDRStateValid{false};

inline std::atomic<bool> dx12_hook_g_LastKnownSwapchainIsHDR{false};

inline std::atomic<int> dx12_hook_g_LastKnownSwapchainColorSpace{-1};

inline std::mutex dx12_hook_g_StreamlineStartupActivationSwapchainMutex;

inline IDXGISwapChain* dx12_hook_g_StreamlineStartupActivationSwapchain = nullptr;

inline void UpdateLastKnownSwapchainHDRStateCache(DXGI_FORMAT format, bool isActualHDR, int swapChainColorSpace,
                                                   bool presentationContractSupported) {
    (void)format;
    dx12_hook_g_LastKnownSwapchainColorSpace.store(swapChainColorSpace, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainIsHDR.store(isActualHDR, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainHDRStateValid.store(presentationContractSupported, std::memory_order_release);
}

inline bool IsReadableSwapchainPointer(const void* ptr) {
    if (!ptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }

    return true;
}

inline bool IsExecutableCodePointer(const void* ptr) {
    if (!ptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }

    const DWORD executableProtection =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executableProtection) != 0;
}

inline void* ResolveLoadedOrLoadableExport(const char* moduleName, const char* functionName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module) {
        module = LoadLibraryExA(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}

inline bool IsCrtPurecallFunctionPointer(const void* ptr) {
    static void* s_ucrtPurecall = ResolveLoadedOrLoadableExport("ucrtbase.dll", "_purecall");
    static void* s_msvcrtPurecall = ResolveLoadedOrLoadableExport("msvcrt.dll", "_purecall");
    return ptr && (ptr == s_ucrtPurecall || ptr == s_msvcrtPurecall);
}

inline bool IsUsableStartupActivationSwapchainPointer(IDXGISwapChain* swapchain) {
    if (!IsReadableSwapchainPointer(swapchain) || !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)swapchain))) {
        return false;
    }

    void** vtable = *(void***)swapchain;
    if (!vtable || !vtable[0] || !vtable[1] || !vtable[2] || !vtable[8]) {
        return false;
    }

    if (!IsExecutableCodePointer(vtable[0]) || !IsExecutableCodePointer(vtable[1]) ||
        !IsExecutableCodePointer(vtable[2]) || !IsExecutableCodePointer(vtable[8])) {
        return false;
    }

    if (IsCrtPurecallFunctionPointer(vtable[0]) || IsCrtPurecallFunctionPointer(vtable[1]) ||
        IsCrtPurecallFunctionPointer(vtable[2]) || IsCrtPurecallFunctionPointer(vtable[8])) {
        static std::atomic<int> s_purecallSwapchainRejectLogCount{0};
        const int logCount = s_purecallSwapchainRejectLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: Rejecting startup activation swapchain %p because its vtable resolves to CRT _purecall "
                "(qi=%p addRef=%p release=%p present=%p log=%d)",
                swapchain, vtable[0], vtable[1], vtable[2], vtable[8], logCount + 1);
        }
        return false;
    }

    return true;
}

inline void SafeReleaseStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain) {
        return;
    }

    if (!IsUsableStartupActivationSwapchainPointer(swapchain)) {
        static std::atomic<int> s_skipUnsafeSwapchainReleaseLogCount{0};
        const int logCount = s_skipUnsafeSwapchainReleaseLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: Skipping unsafe startup activation swapchain Release for stale pointer %p "
                "(source=%s log=%d)",
                swapchain, source ? source : "unknown", logCount + 1);
        }
        return;
    }

    swapchain->Release();
}

inline void ReleaseStreamlineStartupActivationSwapchain(const char* source) {
    IDXGISwapChain* oldSwapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
        oldSwapchain = dx12_hook_g_StreamlineStartupActivationSwapchain;
        dx12_hook_g_StreamlineStartupActivationSwapchain = nullptr;
    }

    if (oldSwapchain) {
        HookLogImportant("DX12: Released retained Streamline startup activation swapchain %p (source=%s)", oldSwapchain,
                         source ? source : "unknown");
        SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
    }
}

inline bool HasRetainedStreamlineStartupActivationSwapchain() {
    std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
    return dx12_hook_g_StreamlineStartupActivationSwapchain != nullptr;
}

inline bool HasUsableRetainedStreamlineStartupActivationSwapchainCandidate() {
    std::lock_guard<std::mutex> lock(dx12_hook_g_StreamlineStartupActivationSwapchainMutex);
    return IsUsableStartupActivationSwapchainPointer(dx12_hook_g_StreamlineStartupActivationSwapchain);
}

inline bool HasStartupActivationSwapchainCandidateForECLProbe() {
    return HasUsableRetainedStreamlineStartupActivationSwapchainCandidate();
}

// Track the game's Present thread ID. Captured from the first non-FG Present call.
// During SL FG, only this thread should run pre-SL overlay rendering.
// SL's FG worker threads call Present from different threads — they must NOT
// run ProcessFrame overlay rendering (wrong timing, wrong queue).
inline std::atomic<DWORD> dx12_hook_g_GamePresentThreadId{0};

// SL's COM wrapper queue for FG — captured in ECL detour when SL FG is active
// and the ECL is from a queue that's not origGame/scQueue/primaryQ.
// This queue routes through SL's ECL interception to the correct internal queue.
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_SLWrapperQueue{nullptr};

// Sticky wrapper queue for the current PostSL reactivation epoch.
// After FSR->DLSS, Streamline can churn through multiple wrapper queues within
// a few frames. Keep the post-FSR offscreen path on the first wrapper that was
// selected for the epoch instead of following later wrapper churn.
inline ID3D12CommandQueue* dx12_hook_g_PostSLPinnedSLWrapperQueue = nullptr;

// Real D3D12 queue behind SL's wrapper — captured from ECL detour when PostSL
// submits through SL's COM wrapper. Used for direct submission to bypass SL's
// metadata wrapping that causes cumulative DEVICE_REMOVED.
//
// DISCOVERY: Submitting command lists through SL's COM wrapper queue
// (g_SLWrapperQueue->ExecuteCommandLists) adds internal SL metadata per ECL.
// This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
// The damage rate depends on rendering frequency: 1/10 rate = no crash (damage
// drains), full rate = crash at ~500 frames.  Empty ECLs through the wrapper
// are safe (damage requires actual rendering content).
//
// FIX: Capture the real D3D12 queue behind SL's wrapper and submit directly
// via g_RealD3D12ECL(realQueue, ...).  This bypasses SL's internal tracking
// entirely.  Proven stable for 16,798+ frames during active DLSS FG.
//
// CAPTURE MECHANISM: When PostSL submits through SL's wrapper (bootstrap frame),
// our ECL detour sees the real D3D12 queue as pThis (SL's wrapper dispatches
// to it).  s_insidePostSLOverlayECL=true during bootstrap marks the capture.
//
// CAUTION: If SL recreates internal queues, this pointer becomes stale.
// Currently no known trigger for SL queue recreation during a session.
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_RealQueueBehindSLWrapper{nullptr};

inline std::atomic<bool> dx12_hook_g_PostSLCallbackExecutionEnabled{false};

inline std::atomic<uint32_t> dx12_hook_g_PostSLCallbackInFlight{0};

inline std::atomic<bool> dx12_hook_g_PostSLDeferredQueueCleanupPending{false};

inline std::atomic<bool> dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged{false};

inline bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue);

inline bool HookHasSafePostFSRBootstrapPathImpl();

inline void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain);

inline void ClearPostSLQueues(const char* reason);

inline void ResetFFXPresentCallbackOverlayBackend(const char* reason);

inline void SetPostSLCallbackInstalled(bool installed, const char* reason) {
    if (installed) {
        dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
        if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != &PostSLOverlayRenderGated) {
            DXGIShared::g_PostSLOverlayRenderCallback.store(&PostSLOverlayRenderGated, std::memory_order_release);
            HookLogImportant("%s — installed gated PostSL callback", reason);
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackInstalled,
                                        reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                        g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
        }
        return;
    }

    // Any authoritative disable (protected FFX quiesce, FFX takeover, resize,
    // shutdown, retirement itself) ends the make-before-break keep-alive: the
    // keep-alive paths deliberately skip calling this, so a call here means a
    // stronger teardown authority owns the transition now.
    dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);

    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
        DXGIShared::g_PostSLOverlayRenderCallback.store(nullptr, std::memory_order_release);
        HookLogImportant("%s — disabled PostSL callback", reason);
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackRemoved,
                                    reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
    }
}

inline void WaitForInFlightPostSLCallbacks(const char* reason) {
    for (int spin = 0; spin < 200; ++spin) {
        uint32_t inFlight = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
        if (inFlight == 0) {
            return;
        }

        if (spin == 0 || spin == 10 || spin == 50) {
            HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
        }
        Sleep(1);
    }

    uint32_t remaining = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
    if (remaining != 0) {
        HookLogImportant("%s — timed out waiting for %u in-flight PostSL callback(s)", reason, remaining);
    }
}

inline void WaitForOverlayGpuIdle(const char* reason) {
    if (!dx12_hook_g_State.fence || dx12_hook_g_State.currentFenceValue == 0) {
        return;
    }

    const UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!drainEvent) {
        return;
    }

    HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
    if (SUCCEEDED(drainHr)) {
        DWORD waitResult = WaitForSingleObject(drainEvent, 200);
        HookLogImportant("%s — drained overlay GPU work (fenceVal=%llu wait=%u)", reason, (unsigned long long)lastVal,
                         waitResult);
    } else {
        HookLogImportant("%s — fence drain failed hr=0x%08X", reason, drainHr);
    }
    CloseHandle(drainEvent);
}

inline void CleanupDeferredPostSLQueuesIfSafe(const char* reason);

inline void RealignInactiveCommandQueueToSwapchainQueue(const char* reason);

inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_DeferredCommandQueueRelease{nullptr};

inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_DeferredPostSLLockedQueueRelease{nullptr};

inline std::atomic<ULONGLONG> dx12_hook_g_PostSLRecentTeardownActivityUntilMs{0};

inline void WaitForOverlayGpuIdle(const char* reason);

inline void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain = false);

inline void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
    ID3D12CommandQueue* oldPinnedWrapperQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        oldPinnedWrapperQueue = dx12_hook_g_PostSLPinnedSLWrapperQueue;
        dx12_hook_g_PostSLPinnedSLWrapperQueue = nullptr;
    }

    if (oldPinnedWrapperQueue) {
        HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, oldPinnedWrapperQueue);
        oldPinnedWrapperQueue->Release();
    }
}

inline void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut) {
    if (lockedQueueOut) {
        *lockedQueueOut = nullptr;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (lockedQueueOut) {
        *lockedQueueOut = dx12_hook_g_PostSLLockedQueue;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = dx12_hook_g_PostSLDedicatedQueue;
    }
    dx12_hook_g_PostSLLockedQueue = nullptr;
    dx12_hook_g_PostSLDedicatedQueue = nullptr;
}

inline void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue,
                                        ID3D12CommandQueue* dedicatedQueue) {
    if (lockedQueue) {
        HookLogImportant("%s — releasing PostSL locked queue %p", reason, lockedQueue);
        lockedQueue->Release();
    }

    if (dedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, dedicatedQueue);
        dedicatedQueue->Release();
    }
}

inline void ClearPostSLQueues(const char* reason) {
    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);
    ReleaseDetachedPostSLQueues(reason, oldLockedQueue, oldDedicatedQueue);
}

inline void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
    ID3D12CommandQueue* deferredLockedQueue =
        dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredLockedQueue) {
        HookLogImportant("%s - releasing deferred PostSL locked queue %p", reason, deferredLockedQueue);
        deferredLockedQueue->Release();
    }

    ID3D12CommandQueue* deferredCommandQueue =
        dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredCommandQueue) {
        HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
        deferredCommandQueue->Release();
    }

    if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.load(std::memory_order_acquire)) {
        return;
    }

    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire) != 0) {
        return;
    }

    if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);

    if (oldLockedQueue) {
        ID3D12CommandQueue* previouslyDeferred =
            dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(oldLockedQueue, std::memory_order_acq_rel);
        if (previouslyDeferred) {
            HookLogImportant("%s - releasing superseded deferred PostSL locked queue %p", reason, previouslyDeferred);
            previouslyDeferred->Release();
        }
        HookLogImportant("%s - deferred PostSL locked queue release %p", reason, oldLockedQueue);
    }
    if (oldDedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, oldDedicatedQueue);
        oldDedicatedQueue->Release();
    }

    RealignInactiveCommandQueueToSwapchainQueue(reason);
}

inline void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
    if (!queue) {
        return;
    }

    constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
    dx12_hook_g_PostSLRecentTeardownActivityUntilMs.store(GetTickCount64() + kPostSLRecentTeardownActivityMs,
                                                std::memory_order_release);
    static std::atomic<int> s_postSLRecentTeardownLogCount{0};
    const int logCount = s_postSLRecentTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 128) == 0) {
        HookLogImportant("%s - marking PostSL queue %p as recently active during Streamline teardown (%llums)", reason,
                         queue, (unsigned long long)kPostSLRecentTeardownActivityMs);
    }
}

inline void InvalidateAllOverlayCachedFrames() {
    g_OverlayAdapter.InvalidateCachedFrame();
    dx12_hook_g_D3D11On12Adapter.InvalidateCachedFrame();
    dx12_hook_g_SLFGAdapter.InvalidateCachedFrame();
}

inline void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain) {
    dx12_hook_g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);

    if (deferQueueReleaseUntilCallbacksDrain) {
        SetPostSLCallbackInstalled(false, reason);
        WaitForInFlightPostSLCallbacks(reason);
        WaitForOverlayGpuIdle(reason);
        dx12_hook_g_PostSLDeferredQueueCleanupPending.store(true, std::memory_order_release);
    } else {
        dx12_hook_g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
        ClearPostSLQueues(reason);
    }

    ClearPostSLPinnedSLWrapperQueue(reason);

    if (clearRealQueueBehindSLWrapper) {
        ID3D12CommandQueue* oldRealQueue = dx12_hook_g_RealQueueBehindSLWrapper.exchange(nullptr, std::memory_order_acq_rel);
        if (oldRealQueue) {
            HookLogImportant("%s — cleared cached real queue behind SL wrapper %p", reason, oldRealQueue);
        }
    }
}

// Swapchain queue - captured at swapchain creation time, preferred for overlay
// rendering to ensure barriers execute on the queue DXGI synchronises with.
inline ID3D12CommandQueue* dx12_hook_g_SwapchainQueue = nullptr;

inline ULONGLONG dx12_hook_g_SwapchainQueueCaptureTime = 0;  // GetTickCount64() when scQueue was last set

// True when swapchain was (re)created on a queue != origGame.  This means an
// FG runtime (FSR FG / DLSS FG) owns the swapchain and its queue.  ANY GPU
// work we submit on that queue (ECLs, resource priming, even allocator/fence
// creation callbacks) can break the FG runtime's internal fence sync.
// Cleared when swapchain recreated back on origGame or FG heuristic is None
// for a sustained period.
inline bool dx12_hook_g_FGRuntimeOwnsSwapchain = false;

inline ULONGLONG dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;

inline void RealignInactiveCommandQueueToSwapchainQueue(const char* reason) {
    ID3D12CommandQueue* oldCommandQueue = nullptr;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* originalGameQueue = nullptr;
    bool realignedCommandQueue = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = dx12_hook_g_SwapchainQueue;
        originalGameQueue = dx12_hook_g_OriginalGameQueue;
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        bool actualFGActive = IsActualFrameGenerationActive();
        bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
                actualFGActive, streamlineFGRunning, swapchainQueue != nullptr, originalGameQueue != nullptr,
                currentCommandQueue != nullptr, currentCommandQueue == swapchainQueue,
                currentCommandQueue == originalGameQueue,
                currentCommandQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire))) {
            oldCommandQueue = currentCommandQueue;
            g_CommandQueue.store(swapchainQueue, std::memory_order_release);
            swapchainQueue->AddRef();
            realignedCommandQueue = true;
        }
    }

    if (realignedCommandQueue) {
        HookLogImportant("%s - realigned stale command queue %p -> swapchain queue %p (origGame=%p)", reason,
                         oldCommandQueue, swapchainQueue, originalGameQueue);
        if (oldCommandQueue) {
            ID3D12CommandQueue* previouslyDeferred =
                dx12_hook_g_DeferredCommandQueueRelease.exchange(oldCommandQueue, std::memory_order_acq_rel);
            if (previouslyDeferred) {
                HookLogImportant("%s - releasing superseded deferred stale command queue %p", reason,
                                 previouslyDeferred);
                previouslyDeferred->Release();
            }
        }
    }
}

// Guard flag: skip queue capture during temp swapchain creation
inline std::atomic<bool> dx12_hook_g_CreatingTempSwapchain{false};

struct ForwardedCreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    char callerModulePath[MAX_PATH] = {};
};

inline thread_local ForwardedCreateSwapchainForHwndCallerContext dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext;

class ScopedForwardedCreateSwapchainForHwndCallerContext {
public:
    ScopedForwardedCreateSwapchainForHwndCallerContext(const void* callerAddress, const char* callerModulePath)
        : previousContext_(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext) {
        dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext = {};
        dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress = callerAddress;
        if (callerModulePath && *callerModulePath) {
            strncpy_s(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath,
                      sizeof(dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath), callerModulePath,
                      _TRUNCATE);
        }
    }

    ~ScopedForwardedCreateSwapchainForHwndCallerContext() {
        dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext = previousContext_;
    }

private:
    ForwardedCreateSwapchainForHwndCallerContext previousContext_;
};

inline thread_local int dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = 0;

inline thread_local bool dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = false;

class ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard {
public:
    ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard()
        : previousDepth_(dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth),
          previousHandled_(dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled) {
        dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_ + 1;
        dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = false;
    }

    ~ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard() {
        dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_;
        dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = previousHandled_;
    }

    bool InlineHandledForwardedCall() const {
        return dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled;
    }

private:
    int previousDepth_ = 0;
    bool previousHandled_ = false;
};

inline void MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled() {
    if (dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth <= 0) {
        return;
    }
    dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = true;
}

inline bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleHandleOut = nullptr) {
    if (moduleHandleOut) {
        *moduleHandleOut = nullptr;
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

    if (moduleHandleOut) {
        *moduleHandleOut = callerModule;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount));
    }
    return true;
}

// ===================== DX12 API call trace diagnostic =====================
// Gated by env CE_DX12_TRACE=1 or a flag file "ce_dx12_trace" next to the hook DLL. When enabled, logs
// caller-attributed D3D12 device/queue calls (CreateCommandQueue / CreateCommittedResource /
// CreateDescriptorHeap / CreateSwapChain[ForHwnd] / ExecuteCommandLists / Signal) so the overlay's --
// and any co-resident injected module's -- interaction with the app's D3D12 device can be inspected
// (queue usage, resource/descriptor footprint, per-frame submission/fence pattern). This is a
// diagnostic aid for focus/mode-switch and overlay-coexistence investigations. Zero impact when
// disabled: the extra vtable hooks are not installed and no logging runs.
inline bool Dx12TraceEnabled() {
    static const bool s_enabled = []() -> bool {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("CE_DX12_TRACE", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            if (buf[0] == '1' || _stricmp(buf, "on") == 0 || _stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0) {
                return true;
            }
        }
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&Dx12TraceEnabled), &self) &&
            self) {
            char path[MAX_PATH] = {};
            DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                for (DWORD i = len; i > 0; --i) {
                    if (path[i - 1] == '\\' || path[i - 1] == '/') {
                        path[i] = '\0';
                        break;
                    }
                }
                char flagPath[MAX_PATH] = {};
                _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%sce_dx12_trace", path);
                HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    return true;
                }
            }
        }
        return false;
    }();
    return s_enabled;
}

inline bool Dx12TraceIsInfraModule(const char* base) {
    return _stricmp(base, "capture_hook_x86.dll") == 0 || _stricmp(base, "capture_hook_x64.dll") == 0 ||
           _stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "d3d12core.dll") == 0 ||
           _stricmp(base, "dxgi.dll") == 0 || _strnicmp(base, "nvwgf2um", 8) == 0 ||
           _stricmp(base, "kernelbase.dll") == 0 || _stricmp(base, "kernel32.dll") == 0 ||
           _stricmp(base, "ntdll.dll") == 0 || _stricmp(base, "win32u.dll") == 0;
}

// Capture the call stack, identify the originating module (first non-infra frame), and log a compact
// module trail. The trail + the queue/resource pointers in `details` are the ground truth for analysis
// (e.g. correlate ExecuteCommandLists/Signal queue pointers with the queue a given module created).
inline void Dx12TraceLog(const char* api, const char* details) {
    constexpr USHORT kMaxFrames = 24;
    void* frames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
    char originator[64] = "?";
    bool foundOriginator = false;
    char trail[256] = {};
    size_t trailLen = 0;
    char lastBase[64] = {};
    for (USHORT i = 0; i < frameCount; ++i) {
        char modPath[MAX_PATH] = {};
        if (!TryGetModulePathFromCodeAddress(frames[i], modPath, sizeof(modPath))) {
            continue;  // trampoline / non-module code region
        }
        const char* slash = strrchr(modPath, '\\');
        const char* base = slash ? slash + 1 : modPath;
        if (!foundOriginator && !Dx12TraceIsInfraModule(base)) {
            _snprintf_s(originator, sizeof(originator), _TRUNCATE, "%s", base);
            foundOriginator = true;
        }
        if (_stricmp(base, lastBase) != 0) {  // collapse consecutive duplicate frames
            int w =
                _snprintf_s(trail + trailLen, sizeof(trail) - trailLen, _TRUNCATE, "%s%s", trailLen ? ">" : "", base);
            if (w > 0) {
                trailLen += static_cast<size_t>(w);
            }
            _snprintf_s(lastBase, sizeof(lastBase), _TRUNCATE, "%s", base);
        }
        if (trailLen + 16 >= sizeof(trail)) {
            break;
        }
    }
    HookLogImportant("DX12 TRACE: %s orig=%s | %s | trail=%s", api, originator, details ? details : "", trail);
}

inline bool IsCurrentECLCallerFromThirdPartyOverlay(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    const void* callerAddress = CE_RETURN_ADDRESS();
    if (!callerAddress) {
        return false;
    }

    char localModulePath[MAX_PATH] = {};
    char* targetBuffer = (modulePathOut && modulePathOutCount > 0) ? modulePathOut : localModulePath;
    const size_t targetCount = (modulePathOut && modulePathOutCount > 0) ? modulePathOutCount : sizeof(localModulePath);
    if (!TryGetModulePathFromCodeAddress(callerAddress, targetBuffer, targetCount)) {
        return false;
    }

    return ce::overlay_compat::IsThirdPartyOverlayModulePath(targetBuffer);
}

struct CreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    bool callerFromFFXFGModule = false;
    bool callerFromThirdPartyOverlay = false;
    char callerModulePath[MAX_PATH] = {};
};

struct CreateSwapchainQueueCaptureEvidence {
    const void* callerAddress = nullptr;
    bool callerFromThirdPartyOverlay = false;
    bool authoritativeFFXRuntimeCreator = false;
    bool officialAMDFFXRuntimeCreator = false;
    bool authoritativeStreamlineRuntimeCreator = false;
    bool callerFromStreamlineFGModule = false;
    bool streamlineFrameGenerationInStack = false;
    char callerModulePath[MAX_PATH] = {};
    char ffxModulePath[MAX_PATH] = {};
};

inline CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence(
    const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule,
    bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    const char* callerModulePath, const char* ffxModulePath) {
    CreateSwapchainQueueCaptureEvidence evidence = {};
    evidence.callerAddress = callerAddress;
    evidence.callerFromThirdPartyOverlay = callerFromThirdPartyOverlay;
    evidence.authoritativeFFXRuntimeCreator =
        ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule,
                                                                                    ffxFrameGenerationInStack);
    evidence.authoritativeStreamlineRuntimeCreator = callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
    evidence.callerFromStreamlineFGModule = callerFromStreamlineFGModule;
    evidence.streamlineFrameGenerationInStack = streamlineFrameGenerationInStack;
    if (callerModulePath && *callerModulePath) {
        strncpy_s(evidence.callerModulePath, sizeof(evidence.callerModulePath), callerModulePath, _TRUNCATE);
    }
    const char* authoritativeFFXPath = (ffxModulePath && *ffxModulePath)
                                           ? ffxModulePath
                                           : (callerFromFFXFGModule && callerModulePath ? callerModulePath : nullptr);
    if (authoritativeFFXPath && *authoritativeFFXPath) {
        strncpy_s(evidence.ffxModulePath, sizeof(evidence.ffxModulePath), authoritativeFFXPath, _TRUNCATE);
        evidence.officialAMDFFXRuntimeCreator = ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(authoritativeFFXPath);
    }
    return evidence;
}

inline CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {
    CreateSwapchainForHwndCallerContext context = {};

    char immediateCallerModulePath[MAX_PATH] = {};
    const void* immediateCallerAddress = CE_RETURN_ADDRESS();
    TryGetModulePathFromCodeAddress(immediateCallerAddress, immediateCallerModulePath,
                                    sizeof(immediateCallerModulePath));

    const char* effectiveCallerModulePath = ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
        dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
    if (effectiveCallerModulePath && *effectiveCallerModulePath) {
        strncpy_s(context.callerModulePath, sizeof(context.callerModulePath), effectiveCallerModulePath, _TRUNCATE);
    }

    context.callerAddress = dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0]
                                ? dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress
                                : immediateCallerAddress;
    context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
    context.callerFromThirdPartyOverlay = ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
        dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
    return context;
}

inline bool ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
    const char* context, bool rawCallerFromThirdPartyOverlay, bool callerFromFFXFGModule,
    bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    const char* callerModulePath) {
    const bool authoritativeFGRuntimeSwapchainCreator =
        ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
            callerFromFFXFGModule, ffxFrameGenerationInStack, callerFromStreamlineFGModule,
            streamlineFrameGenerationInStack);
    if (rawCallerFromThirdPartyOverlay && authoritativeFGRuntimeSwapchainCreator) {
        static std::atomic<int> s_wrappedFGCreateCallerLogCount{0};
        const int logCount = s_wrappedFGCreateCallerLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20) {
            const char* runtimeKind = (callerFromFFXFGModule || ffxFrameGenerationInStack) ? "FFX" : "Streamline";
            if (callerFromStreamlineFGModule || callerFromFFXFGModule || ffxFrameGenerationInStack) {
                HookLogImportant(
                    "%s: %s frame-generation stack detected behind third-party overlay caller %s — treating "
                    "swapchain as authoritative runtime takeover",
                    context ? context : "CreateSwapChain", runtimeKind,
                    callerModulePath && *callerModulePath ? callerModulePath : "unknown");
            } else {
                HookLogImportant(
                    "%s: Streamline stack detected behind third-party overlay caller %s — deferring takeover "
                    "classification until queue identity is known",
                    context ? context : "CreateSwapChain",
                    callerModulePath && *callerModulePath ? callerModulePath : "unknown");
            }
        }
    }

    return rawCallerFromThirdPartyOverlay && !authoritativeFGRuntimeSwapchainCreator;
}

inline bool ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(
    const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    return ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(
        captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.officialAMDFFXRuntimeCreator,
        HasResolvedOfficialFFXStartupPath());
}

inline void StageProtectedOfficialFFXStartupQueueFromCreateDevice(
    IUnknown* createDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
    ID3D12CommandQueue* queue = nullptr;
    HRESULT qiHr = E_POINTER;
    if (createDevice) {
        qiHr = createDevice->QueryInterface(IID_PPV_ARGS(&queue));
    }

    bool hasDirectQueue = false;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    if (SUCCEEDED(qiHr) && queue) {
        queueDesc = queue->GetDesc();
        hasDirectQueue = queueDesc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    const bool shouldStage =
        ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, hasDirectQueue);
    const char* modulePath =
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : captureEvidence.callerModulePath;
    if (shouldStage) {
        StoreDeferredOfficialFFXTakeoverSideEffects(queue,
                                                    modulePath && modulePath[0] ? modulePath : "official FFX runtime",
                                                    "protected official FFX swapchain create queue staging");

        HookLogImportant(
            "%s: Protected official FFX startup staged runtime queue %p until enabled ffxConfigure "
            "(module=%s caller=%s)",
            context && context[0] ? context : "CreateSwapChain", queue,
            modulePath && modulePath[0] ? modulePath : "unknown",
            captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    } else {
        static std::atomic<int> s_stageQueueFailLogCount{0};
        const int logCount = s_stageQueueFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "%s: Protected official FFX startup could not stage runtime queue "
                "(createDevice=%p queue=%p qiHr=0x%08X queueType=%d module=%s log=%d)",
                context && context[0] ? context : "CreateSwapChain", createDevice, queue, (unsigned)qiHr,
                queue ? static_cast<int>(queueDesc.Type) : -1, modulePath && modulePath[0] ? modulePath : "unknown",
                logCount + 1);
        }
    }

    if (queue) {
        queue->Release();
    }
}

inline bool ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup() {
    return ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(
        dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
        HasResolvedOfficialFFXStartupPath());
}

inline bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
    IUnknown* pDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, ID3D12CommandQueue** queueOut) {
    if (queueOut) {
        *queueOut = nullptr;
    }
    if (!pDevice) {
        return false;
    }

    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))) || !pQueue) {
        return false;
    }

    ID3D12CommandQueue* originalGameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        originalGameQueue = dx12_hook_g_OriginalGameQueue;
    }
    const bool streamlineRuntimeAvailable = IsStreamlineLoaded() || g_FGCompat.HasStreamlineSupport() ||
                                            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
                                            captureEvidence.authoritativeStreamlineRuntimeCreator;
    const bool deferRefresh = ce::dx12_overlay_policy::ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(
        originalGameQueue != nullptr, pQueue == originalGameQueue, streamlineRuntimeAvailable, dx12_hook_g_HadFSRFGPhase,
        g_FGCompat.IsFSRFGApiActive(), g_FGCompat.GetRuntimeMode());
    if (deferRefresh && queueOut) {
        *queueOut = pQueue;
    } else {
        pQueue->Release();
    }
    return deferRefresh;
}

inline bool ShouldApplySwapchainDescriptorOverridesForCreate(
    const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    return ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
        captureEvidence.callerFromThirdPartyOverlay,
        captureEvidence.authoritativeFFXRuntimeCreator || captureEvidence.authoritativeStreamlineRuntimeCreator);
}

inline void PrepareForAuthoritativeFFXSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                      const char* context) {
    if (!ce::dx12_overlay_policy::ShouldReleaseRetainedStreamlineStartupActivationSwapchainForAuthoritativeFFXCreate(
            captureEvidence.authoritativeFFXRuntimeCreator, HasRetainedStreamlineStartupActivationSwapchain())) {
        return;
    }

    HookLogImportant(
        "%s: Authoritative FFX swapchain create is replacing a Streamline startup handoff — releasing retained "
        "Streamline activation swapchain before DXGI CreateSwapChainForHwnd to avoid stale HWND references "
        "(ffxModule=%s caller=%s)",
        context && context[0] ? context : "CreateSwapChainForHwnd",
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    ReleaseStreamlineStartupActivationSwapchain("DX12: authoritative FFX swapchain create");
}

inline void LogSkippedSwapchainDescriptorOverridesForRuntimeCreate(
    const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence, UINT bufferCount, UINT flags,
    DXGI_SWAP_EFFECT swapEffect) {
    if (!captureEvidence.authoritativeFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator) {
        return;
    }

    static std::atomic<int> s_runtimeDescriptorPassthroughLogCount{0};
    const int logCount = s_runtimeDescriptorPassthroughLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "%s: Preserving swapchain descriptor for authoritative FG runtime create "
            "(ffx=%d officialFFX=%d streamline=%d caller=%s BufferCount=%u Flags=0x%X SwapEffect=%d count=%d)",
            context && context[0] ? context : "CreateSwapChain", captureEvidence.authoritativeFFXRuntimeCreator ? 1 : 0,
            captureEvidence.officialAMDFFXRuntimeCreator ? 1 : 0,
            captureEvidence.authoritativeStreamlineRuntimeCreator ? 1 : 0,
            captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack", bufferCount, flags,
            static_cast<int>(swapEffect), logCount + 1);
    }
}

inline bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(HWND hWnd, IDXGISwapChain* swapchain,
                                                                  const char* context, HRESULT hr) {
    if (FAILED(hr) || !swapchain || !hWnd) {
        return false;
    }

    const bool outputWindowVisible = IsWindowVisible(hWnd) != FALSE;
    if (!ce::dx12_overlay_policy::ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain(
            true, outputWindowVisible)) {
        return false;
    }

    static std::atomic<int> s_invisibleWindowCreateSkipLogCount{0};
    const int logCount = s_invisibleWindowCreateSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "%s: Invisible-window swapchain %p for HWND=%p — bypassing CE swapchain side-effects "
            "(queue capture, Present refresh, cooldown, wrapper decisions skipped; hr=0x%08X count=%d)",
            context && context[0] ? context : "CreateSwapChainForHwnd", swapchain, hWnd, hr, logCount + 1);
    }
    return true;
}

inline void QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(
    IDXGISwapChain* swapchain, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
    const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postSLConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldQuiesceStreamlinePostSLDuringProtectedOfficialFFXStartup(
            true, HasResolvedOfficialFFXStartupPath(), callbackInstalled, postSLActive, postSLConfirmed,
            streamlineFGRunning, startupActivationPending)) {
        return;
    }

    const char* source = context && context[0] ? context : "protected official FFX startup";
    SetPostSLCallbackInstalled(false, "DX12: protected official FFX startup");
    const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGActive(false);
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ResetPostSLLifecycleForTransition("DX12: protected official FFX startup", true, true);
    ReleaseStreamlineStartupActivationSwapchain("DX12: protected official FFX startup");
    StreamlineHook::OnAuthoritativeFFXTakeover();
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    DXGIShared::ResetStreamlineStartupTransitionState();
    DXGIShared::DisableSLPresentRouting();

    HookLogImportant(
        "%s: Protected official FFX startup immediately quiesced Streamline/PostSL before AMD swapchain takeover "
        "(sc=%p callback=%d active=%d confirmed=%d startupPending=%d staleSL=%d module=%s caller=%s)",
        source, swapchain, callbackInstalled ? 1 : 0, postSLActive ? 1 : 0, postSLConfirmed ? 1 : 0,
        startupActivationPending ? 1 : 0, staleStreamlineSignal ? 1 : 0,
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
}

inline bool HandleProtectedOfficialFFXStartupSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                             IUnknown* createDevice, IDXGISwapChain* swapchain,
                                                             const char* context) {
    if (!ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence)) {
        return false;
    }

    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    SetNativeFSRStartupConfigureArmingPending(true, "protected official FFX swapchain create");
    dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.store(true, std::memory_order_release);
    ArmProtectedOfficialFFXStartupProgressTracking("protected official FFX swapchain create");
    ResetAuthoritativeFSRRealFrameOnlyStreak();
    if (!dx12_hook_g_HadFSRFGPhase) {
        dx12_hook_g_HadFSRFGPhase = true;
        HookLogImportant(
            "DX12: Protected official FFX swapchain create implies FSR FG history — latching post-FSR handoff state");
    }

    StageProtectedOfficialFFXStartupQueueFromCreateDevice(createDevice, captureEvidence, context);
    QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(swapchain, captureEvidence, context);

    HookLogImportant(
        "DX12: Protected official FFX startup swapchain pass-through via %s (sc=%p module=%s caller=%s) — "
        "deferring Present hook refresh, queue ownership, FFX export inspection, and heavy takeover side effects "
        "until enabled ffxConfigure; live Streamline/PostSL routing was quiesced immediately when present",
        context && context[0] ? context : "CreateSwapChain", swapchain,
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : "unknown",
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    return true;
}

inline void ApplyAuthoritativeFFXTakeoverSideEffects(ID3D12CommandQueue* capturedQueue, const char* callerModulePath,
                                                     const char* reason) {
    bool stagedQueueApplied = false;
    bool stagedQueueActivatedOwnership = false;
    ID3D12CommandQueue* liveSwapchainQueueAfterApply = nullptr;
    bool fgRuntimeOwnsAfterApply = false;
    if (capturedQueue) {
        stagedQueueActivatedOwnership = DX12_SetSwapchainQueue(capturedQueue, false, true);
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            liveSwapchainQueueAfterApply = dx12_hook_g_SwapchainQueue;
            fgRuntimeOwnsAfterApply = dx12_hook_g_FGRuntimeOwnsSwapchain;
        }
        stagedQueueApplied = liveSwapchainQueueAfterApply == capturedQueue;
        HookLogImportant(
            "DX12: FFX swapchain takeover applied staged runtime queue "
            "(captured=%p liveScQueue=%p applied=%d ownershipActivated=%d fgOwned=%d reason=%s)",
            capturedQueue, liveSwapchainQueueAfterApply, stagedQueueApplied ? 1 : 0,
            stagedQueueActivatedOwnership ? 1 : 0, fgRuntimeOwnsAfterApply ? 1 : 0,
            reason && reason[0] ? reason : "unknown");
    }

    const bool staleStreamlineSignal = DXGIShared::g_StreamlineFGRunning.exchange(false, std::memory_order_acq_rel);
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGActive(false);
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    SetPostSLCallbackInstalled(false, "DX12: FFX swapchain takeover");
    ResetPostSLLifecycleForTransition("DX12: FFX swapchain takeover", true, true);
    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: FFX swapchain takeover");
    ResetFFXPresentCallbackOverlayBackend("DX12: FFX swapchain takeover");
    StreamlineHook::OnAuthoritativeFFXTakeover();
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    DXGIShared::ResetStreamlineStartupTransitionState();
    HookLogImportant("DX12: FFX swapchain takeover — cleared stale Streamline startup handoff/transition state");
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "DX12::AuthoritativeFFXTakeover", capturedQueue, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
    DXGIShared::DisableSLPresentRouting();
    {
        ID3D12CommandQueue* oldWrapper = dx12_hook_g_SLWrapperQueue.exchange(nullptr, std::memory_order_acq_rel);
        if (oldWrapper) {
            HookLogImportant("DX12: FFX swapchain takeover — released stale SL wrapper queue %p", oldWrapper);
            oldWrapper->Release();
        }
    }

    HookLogImportant(
        "DX12: FFX swapchain takeover via %s "
        "(queue=%p stagedQueueApplied=%d liveScQueue=%p staleSL=%d reason=%s) — cleared Streamline/PostSL ownership",
        callerModulePath && callerModulePath[0] ? callerModulePath : "unknown", capturedQueue,
        stagedQueueApplied ? 1 : 0, liveSwapchainQueueAfterApply, staleStreamlineSignal ? 1 : 0,
        reason && reason[0] ? reason : "unknown");

    if (!g_FGCompat.HasDirectFFXApiConfirmation()) {
        HookLogImportant(
            "DX12: Authoritative FFX takeover has no direct ffxConfigure confirmation yet; keeping FFX hooks armed "
            "and waiting for a real runtime configure instead of issuing a synthetic partial ffxConfigure");
        FFXHook::Init();
    }
}

inline bool MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress(const char* source) {
    if (!dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire) ||
        HasResolvedOfficialFFXStartupPath()) {
        return false;
    }

    const uint32_t processFrameSkips = dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.load(std::memory_order_acquire);
    const uint32_t eclPassThroughs = dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.load(std::memory_order_acquire);
    if (processFrameSkips < ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupProcessFrameProgressThreshold() &&
        eclPassThroughs < ce::dx12_overlay_policy::GetProtectedOfficialFFXStartupECLProgressThreshold()) {
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldFinalizeProtectedOfficialFFXStartupAfterSustainedFrameProgress(
            true, false, processFrameSkips, eclPassThroughs)) {
        // The policy currently forbids progress-only finalization. Keep this
        // branch as a guardrail if that policy is ever revisited.
        return false;
    }

    static std::atomic<int> s_protectedOfficialFFXProgressOnlyLogCount{0};
    const int logCount = s_protectedOfficialFFXProgressOnlyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 600) == 0) {
        const ULONGLONG nowMs = GetTickCount64();
        const ULONGLONG beginMs = dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.load(std::memory_order_acquire);
        HookLogImportant(
            "DX12: Protected official FFX startup has sustained frame progress but remains quiesced until direct "
            "ffxConfigure/present-callback proof (source=%s elapsed=%llums processFrameSkips=%u eclPassThroughs=%u "
            "log=%d)",
            source && source[0] ? source : "unknown", beginMs ? static_cast<unsigned long long>(nowMs - beginMs) : 0ULL,
            processFrameSkips, eclPassThroughs, logCount + 1);
    }
    return false;
}

inline void ClearStaleStreamlineOwnershipForFSRTakeover(const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                                        bool runtimeOwnsSwapchain, bool runtimeOwnershipJustActivated,
                                                        ID3D12CommandQueue* capturedQueue) {
    char callerModulePath[MAX_PATH] = {};
    if (captureEvidence.callerModulePath[0]) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), captureEvidence.callerModulePath, _TRUNCATE);
    }

    const bool callerFromFFXFGModule = captureEvidence.authoritativeFFXRuntimeCreator;
    if (callerFromFFXFGModule &&
        (!callerModulePath[0] || ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath))) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), "FFX frame-generation runtime", _TRUNCATE);
    }
    char ffxModulePath[MAX_PATH] = {};
    if (captureEvidence.ffxModulePath[0]) {
        strncpy_s(ffxModulePath, sizeof(ffxModulePath), captureEvidence.ffxModulePath, _TRUNCATE);
    }
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    const bool staleStreamlineOwnershipCandidate = runtimeOwnsSwapchain && streamlineFGRunning &&
                                                   !streamlineStartupHandoffPending && runtimeOwnershipJustActivated;
    if (!ce::dx12_overlay_policy::ShouldForceEndStreamlineOwnershipForSwapchainTakeover(
            runtimeOwnsSwapchain, callerFromFFXFGModule, streamlineFGRunning, streamlineStartupHandoffPending,
            runtimeOwnershipJustActivated)) {
        if (staleStreamlineOwnershipCandidate && !callerFromFFXFGModule) {
            static std::atomic<int> s_nonFfxTakeoverPreserveLogCount{0};
            const int logCount = s_nonFfxTakeoverPreserveLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant(
                    "DX12: Runtime-owned swapchain transition on %p while Streamline FG is active had no FFX FG "
                    "module in caller stack (caller=%s) — preserving existing Streamline/PostSL ownership",
                    capturedQueue, callerModulePath[0] ? callerModulePath : "unknown");
            }
        }
        return;
    }

    if (!callerModulePath[0] && runtimeOwnershipJustActivated) {
        strncpy_s(callerModulePath, sizeof(callerModulePath), "runtime-owned swapchain transition", _TRUNCATE);
    }

    if (ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence)) {
        g_FGCompat.SetFSRFGSupportPresent(true);
        g_FGCompat.SetFSRFGMultiplier(2);
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        SetNativeFSRStartupConfigureArmingPending(true, "protected official FFX queue capture");
        dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.store(true, std::memory_order_release);
        ArmProtectedOfficialFFXStartupProgressTracking("protected official FFX queue capture");
        ResetAuthoritativeFSRRealFrameOnlyStreak();
        if (!dx12_hook_g_HadFSRFGPhase) {
            dx12_hook_g_HadFSRFGPhase = true;
            HookLogImportant(
                "DX12: Protected official FFX queue capture implies FSR FG history — latching post-FSR handoff state");
        }
        StoreDeferredOfficialFFXTakeoverSideEffects(capturedQueue, ffxModulePath[0] ? ffxModulePath : callerModulePath,
                                                    "protected official FFX queue capture");
        HookLogImportant(
            "DX12: Official FFX queue capture is protected until enabled ffxConfigure (queue=%p runtimeOwned=%d "
            "ffxModule=%s) — skipping FFX export inspection and Streamline/PostSL teardown",
            capturedQueue, runtimeOwnsSwapchain ? 1 : 0, ffxModulePath[0] ? ffxModulePath : "unknown");
        return;
    }

    // Native FFX can be unloaded and reloaded across repeated FG runs. Refresh
    // the FFX API hooks immediately on authoritative takeover so the next
    // configure call can re-arm the present-callback bridge on the live module
    // instead of waiting for the background hook scan.
    FFXHook::Init();

    g_FGCompat.SetFSRFGSupportPresent(true);
    g_FGCompat.SetFSRFGMultiplier(2);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    SetNativeFSRStartupConfigureArmingPending(true, "authoritative FFX swapchain takeover");
    ResetAuthoritativeFSRRealFrameOnlyStreak();
    if (!dx12_hook_g_HadFSRFGPhase) {
        dx12_hook_g_HadFSRFGPhase = true;
        HookLogImportant("DX12: FFX swapchain takeover implies FSR FG history — latching post-FSR handoff state");
    }

    if (ce::dx12_overlay_policy::ShouldDeferOfficialFFXTakeoverSideEffectsUntilEnabledConfigure(
            runtimeOwnsSwapchain, callerFromFFXFGModule, captureEvidence.officialAMDFFXRuntimeCreator,
            g_FGCompat.HasDirectFFXApiConfirmation())) {
        StoreDeferredOfficialFFXTakeoverSideEffects(capturedQueue, ffxModulePath[0] ? ffxModulePath : callerModulePath,
                                                    "authoritative official FFX swapchain takeover");
        HookLogImportant(
            "DX12: Official FFX takeover is in startup-arming mode; Streamline/PostSL teardown and SL route disable "
            "are deferred until enabled ffxConfigure (queue=%p runtimeOwned=%d ffxModule=%s)",
            capturedQueue, runtimeOwnsSwapchain ? 1 : 0, ffxModulePath[0] ? ffxModulePath : "unknown");
        return;
    }

    g_FGCompat.SetFSRFGActive(true);
    ApplyAuthoritativeFFXTakeoverSideEffects(capturedQueue, callerModulePath, "authoritative FFX swapchain takeover");
}

// LOCK HIERARCHY (MUST be acquired in this order to prevent deadlocks):
// 1. g_OverlayMutex (outermost - protects overlay state)
// 2. g_CommandQueueMutex (protects command queue pointer)
// 3. g_DX12CaptureMutex (innermost - protects capture state)
//
// Rule: When acquiring multiple locks, always acquire in order above.
//       Use std::lock_guard with std::adopt_lock when using try_lock().
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex dx12_hook_g_OverlayMutex;

inline bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue,
                                                          const char* context);

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex dx12_hook_g_DX12CaptureMutex;

inline OverlayConfig GetActiveDX12OverlayConfig(SharedMemoryLayout* shm) {
    OverlayConfig cfg{};
    cfg.captureIncludeOverlay = true;
    cfg.screenshotIncludeOverlay = true;
    if (shm) {
        cfg = shm->ReadOverlayConfig();
    }
    return cfg;
}

inline bool IsDX12ObserverOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverOnly(GetActiveDX12OverlayConfig(shm));
}

inline bool IsDX12ObserverPolicyOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverPolicyOnly(GetActiveDX12OverlayConfig(shm));
}

inline bool IsDX12ObserverStartupPresentOnlyModeActive(SharedMemoryLayout* shm) {
    return IsOverlayObserverStartupPresentOnly(GetActiveDX12OverlayConfig(shm));
}

inline void EnsurePostSLDisabledForObserverOnly(const char* reason, bool preserveStartupTransitionWindow = false) {
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLStableFrameCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain(reason);
    if (!preserveStartupTransitionWindow) {
        DXGIShared::ResetStreamlineStartupTransitionState();
    }
    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
        SetPostSLCallbackInstalled(false, reason);
    }
}

inline bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg) {
    return cfg.showOverlay && dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) &&
           dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}

inline void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm, uint64_t requestId,
                                           ID3D12CommandQueue* queueOverride = nullptr) {
    if (!sc3 || !shm || requestId == 0)
        return;

    bool queued = false;
    ID3D12Device* dx12Device = g_Device.load();
    ID3D12CommandQueue* dx12Queue = queueOverride ? queueOverride : g_CommandQueue.load();
    if (dx12Device && dx12Queue) {
        UINT bbIdx = sc3->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = nullptr;
        if (SUCCEEDED(sc3->GetBuffer(bbIdx, IID_PPV_ARGS(&backBuffer)))) {
            const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
            const auto presentationEncoding = DXGIShared::ResolveSwapChainPresentationEncoding(
                static_cast<IDXGISwapChain*>(sc3), resourceDesc.Format);
            queued = SaveDX12TextureAsScreenshotRaw(dx12Device, dx12Queue, backBuffer, shm, requestId,
                                                    presentationEncoding);
            backBuffer->Release();
        }
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

inline void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm,
                                     ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx,
                                     UINT currentBackBufferIdx) {
    if (!pSwapChain || !shm || !captureQueue)
        return;
    if (shm->throttleCapture.load(std::memory_order_acquire))
        return;

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
    if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
        presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
    }
    shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));

    std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
    ID3D12Device* captureDevice = g_Device.load(std::memory_order_acquire);
    if (!dx12_hook_g_SharedCaptureD3D12.IsInitializedFor(captureDevice, pSwapChain)) {
        if (!dx12_hook_g_SharedCaptureD3D12.Initialize(captureDevice, pSwapChain)) {
            return;
        }
        HookLogImportant("DX12: Shared capture initialized for swapchain generation sc=%p device=%p", pSwapChain,
                         captureDevice);
    }

    UINT bbIdx = 0;
    if (hasCurrentBackBufferIdx) {
        bbIdx = currentBackBufferIdx;
    } else {
        IDXGISwapChain3* sc3 = nullptr;
        pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
        bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
        if (sc3)
            sc3->Release();
    }

    if (!dx12_hook_g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx))
        return;

    SharedFrameDescriptor desc;
    if (!dx12_hook_g_SharedCaptureD3D12.GetCurrentFrame(&desc))
        return;

    for (UINT i = 0; i < SharedCaptureD3D12::kSharedTextureCount; ++i) {
        shm->SetSharedHandle(static_cast<int>(i), (uint64_t)dx12_hook_g_SharedCaptureD3D12.GetSharedHandle((int)i));
    }
    shm->SetFenceShareHandle((uint64_t)dx12_hook_g_SharedCaptureD3D12.GetFenceShareHandle());
    shm->SetWidth(desc.width);
    shm->SetHeight(desc.height);
    shm->SetFormat(desc.format);

    uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
    uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
    if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
        const bool ringWasEmpty = wIdx == shm->frameRing.ingestIndex.load(std::memory_order_acquire);
        FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
        slot.fenceValue = desc.fenceValue;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        slot.timestamp = desc.presentTime;
        slot.frameIndex = desc.frameNumber;
        slot.textureIndex = desc.textureIndex;
        slot.sourcePid = GetCurrentProcessId();
        std::atomic_thread_fence(std::memory_order_release);
        slot.valid.store(1, std::memory_order_release);
        shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
        if (ringWasEmpty && g_IPC) {
            g_IPC->SignalInjectFrameReady();
        }
        DXGIShared::SetLatestSourceFrameIndex(desc.frameNumber);
        static uint64_t s_lastPublishLineageLogTick = 0;
        uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastPublishLineageLogTick >= 1000) {
            HookLog("DX12: Publish frame=%u ring=%u tex=%d fence=%llu ts=%llu bb=%u depth=%u", desc.frameNumber, wIdx,
                    desc.textureIndex, static_cast<unsigned long long>(desc.fenceValue),
                    static_cast<unsigned long long>(desc.presentTime), bbIdx, static_cast<unsigned>(wIdx - rIdx));
            s_lastPublishLineageLogTick = nowTick;
        }
    } else {
        shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
        shm->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
    }
}

inline std::atomic<bool> dx12_hook_g_InSwapchainResizeCleanup{false};

inline std::atomic<bool> dx12_hook_g_PreserveOverlayAdapterAcrossResize{false};

inline std::atomic<ID3D12Device*> dx12_hook_g_OverlayAdapterBackendDevice{nullptr};

inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_OverlayAdapterBackendQueue{nullptr};

inline std::atomic<int> dx12_hook_g_OverlayAdapterBackendFormat{static_cast<int>(DXGI_FORMAT_UNKNOWN)};

// Frame counter for post-ImGui-init delay (skip first frame to let GPU
// stabilize)
inline std::atomic<int> dx12_hook_s_framesSinceInit{0};

inline std::atomic<int> dx12_hook_s_framesBeforeInit{0};

// CPU Prerender Limit State (DX12)
inline std::vector<ID3D12Fence*> dx12_hook_g_PrerenderFences;

inline std::vector<HANDLE> dx12_hook_g_PrerenderEvents;

inline uint64_t dx12_hook_g_PrerenderFrameIndex = 0;

inline std::mutex dx12_hook_g_PrerenderMutex;

inline ID3D12Device* dx12_hook_g_PrerenderDevice = nullptr;

inline ID3D12CommandQueue* dx12_hook_g_PrerenderQueue = nullptr;

// ECL piggyback overlay: for games (like GTA5 Enhanced) that reject separate ECL
// submissions touching backbuffers, render the overlay by appending our command
// list to the game's own ExecuteCommandLists call.
inline std::atomic<bool> dx12_hook_g_PiggybackOverlayActive{false};

inline ID3D12DescriptorHeap* dx12_hook_g_FFXPresentRtvHeap = nullptr;

struct FFXPresentCallbackBridgeState {
    ce::ffx_api::PresentCallback originalCallback = nullptr;
    void* originalUserContext = nullptr;
    bool installed = false;
};

inline std::mutex dx12_hook_g_FFXPresentCallbackBridgeMutex;

inline std::unordered_map<void*, FFXPresentCallbackBridgeState> dx12_hook_g_FFXPresentCallbackBridges;

// Deferred Signal: avoid the NVIDIA driver stall caused by Signal between our
// overlay ECL and Present.  Instead of calling Signal immediately after our ECL,
// wrapped Present paths flush it immediately after the real Present returns.
// This keeps the ECL->Present path clean while still giving focus-loss handling
// an authoritative fence for the overlay work that just touched the backbuffer.
inline std::atomic<UINT64> dx12_hook_g_deferredSignalValue{0};

inline std::atomic<int> dx12_hook_g_deferredSignalAllocIdx{-1};

// Track which queue the deferred ECL was submitted on, so the deferred Signal
// goes to the same queue.  When FG runtimes create swapchains with their own
// queue, this may differ from g_CommandQueue.
inline std::atomic<ID3D12CommandQueue*> dx12_hook_g_deferredSignalQueue{nullptr};

inline std::atomic<UINT64> dx12_hook_g_FocusLossPendingOverlayFenceValue{0};

inline std::atomic<bool> dx12_hook_g_FocusLossImmediateFenceDumpRequested{false};

inline std::atomic<bool> dx12_hook_g_FocusLossDeviceRemovalDumpRequested{false};

inline constexpr int dx12_hook_kFocusLossForegroundReacquirePresentProofFrames = 16;

inline constexpr int dx12_hook_kFocusLossRecentTransitionDumpWindowFrames = 300;

inline std::atomic<int> dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining{0};

inline std::atomic<int> dx12_hook_g_FocusLossRecentTransitionPresentWindow{0};

inline std::atomic<bool> dx12_hook_g_SwapchainPresentOccluded{false};

// occlusion signal; historically that only existed on the wrapped path, so vtable-hooked
// apps (e.g. dx12_test) never engaged the hold and drew to the backbuffer through the
// Alt+Tab iflip<->composited mode switch — the GPU-hang root cause. DetourPresent now also
// feeds the present result, so this gates the hold for both paths.
inline std::atomic<bool> dx12_hook_g_HaveD3D12PresentResultSignal{false};

// Focus-transition backbuffer-work hold (v10). DRED proved that ANY CE backbuffer
// touch — direct overlay draw (v8) OR the offscreen path's bb<->offscreen copies
// (v9) — pure-hangs the GPU (pageFaultVA=0) while the swapchain is mid
// iflip<->composited mode switch around a focus change (the backbuffer is
// transiently owned by DWM/the display). The hangs were observed at the refocus
// edge (composited->iflip). So CE holds ALL backbuffer overlay/capture work for a
// short window after EACH foreground-change edge (both directions), then resumes.
// Steady states — focused AND unfocused-but-visible — render directly, exactly
// like a lightweight inject overlay, so the overlay is only briefly absent during the actual mode switch
// (when the screen is transitioning anyway), never during steady visible use.
// Counter is set on the edge by DX12_NoteWrappedD3D12PresentResult and decremented
// per wrapped Present.
inline constexpr int dx12_hook_kFocusTransitionHoldFrames = 60;

inline std::atomic<int> dx12_hook_g_FocusTransitionHoldFrames{0};

struct DX12WrappedPresentFocusLossContext {
    bool valid = false;
    const char* presentName = nullptr;
    int callCount = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
};

inline thread_local DX12WrappedPresentFocusLossContext dx12_hook_s_WrappedPresentFocusLossContext = {};

inline const char* DX12WaitResultName(DWORD waitResult);

// Re-entrancy guard: set when the current thread is inside DetourECL.
// During Alt+Tab, D3D12's internal WaitImpl inside ECL can pump window messages
// (DefWindowProc), which may trigger Present → ProcessFrame.  If ProcessFrame
// submits an overlay ECL while the outer ECL is still inside WaitImpl, a second
// WaitImpl cascades and the render thread hangs.  ProcessFrame checks this flag
// and skips overlay rendering when it's set.
inline thread_local bool dx12_hook_s_insideECL = false;

// Flag to indicate the current thread is inside a PostSL overlay ECL virtual call.
// When we submit our overlay ECL through SL's COM wrapper (virtual call on origGame),
// SL dispatches to the real D3D12 queue, which re-enters our ECL detour.  The detour
// must NOT update queue tracking (g_CommandQueue, g_SLWrapperQueue, etc.) for these
// overlay submissions — they'd pollute the game's queue state.
inline thread_local bool dx12_hook_s_insidePostSLOverlayECL = false;

// Broader CE-owned overlay submission guard. Normal, Steam-deferred, FSR callback,
// and PostSL submissions can re-enter the ECL hook through Streamline/FFX/driver
// wrapper queues even when we call a saved "original" entrypoint. Those re-entrant
// calls must be forwarded, but they must not mutate game/runtime queue tracking.
inline thread_local int dx12_hook_s_insideCEOverlayECLDepth = 0;

inline thread_local const char* dx12_hook_s_insideCEOverlayECLReason = nullptr;

class ScopedCEOverlayECLSubmission {
public:
    explicit ScopedCEOverlayECLSubmission(const char* reason) : previousReason_(dx12_hook_s_insideCEOverlayECLReason) {
        ++dx12_hook_s_insideCEOverlayECLDepth;
        dx12_hook_s_insideCEOverlayECLReason = reason;
    }

    ~ScopedCEOverlayECLSubmission() {
        dx12_hook_s_insideCEOverlayECLReason = previousReason_;
        --dx12_hook_s_insideCEOverlayECLDepth;
    }

    ScopedCEOverlayECLSubmission(const ScopedCEOverlayECLSubmission&) = delete;
    ScopedCEOverlayECLSubmission& operator=(const ScopedCEOverlayECLSubmission&) = delete;

private:
    const char* previousReason_ = nullptr;
};

inline bool CanUseFSRFGHeuristics(const char** blockedReason = nullptr) {
    if (dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire) != nullptr) {
        if (blockedReason) {
            *blockedReason = "normal swapchain return is awaiting its authoritative queue baseline";
        }
        return false;
    }

    if (g_FGCompat.IsFSRFGApiActive()) {
        if (blockedReason) {
            *blockedReason = "authoritative FSR FG state is already active";
        }
        return false;
    }

    // Block when Streamline FG is running — SL creates internal queues that
    // trigger queue-change heuristics.  Without this check, enabling DLSS FG
    // causes false FSR FG detection (SL's queue ≠ origGame → "queue change"
    // heuristic fires → pre-SL renders on wrong queue → DEVICE_HUNG).
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        if (blockedReason) {
            *blockedReason = "Streamline FG is running (queue changes are from SL, not FSR)";
        }
        return false;
    }

    // Block during grace period after SL FG turns OFF.  The queue naturally
    // changes from SL's internal queue back to origGame — this must not be
    // misinterpreted as FSR FG.  The heuristic runs BEFORE the outer block in
    // ProcessFrame, so g_StreamlineFGRunning alone can't prevent the false
    // positive on the same frame SL OFF fires.
    // NOTE: Do NOT decrement here — this function is called per-ECL (thousands/sec).
    // The counter is decremented once per ProcessFrame in the queue-change heuristic.
    if (dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0) {
        if (blockedReason) {
            *blockedReason = "SL FG just turned OFF (grace period)";
        }
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringAuthoritativeStreamlineStartupHandoff(
            dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineStartupHandoffPending, runtimeMode)) {
        if (blockedReason) {
            *blockedReason = "fresh authoritative Streamline startup handoff is still runtime-inactive";
        }
        return false;
    }

    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
    }
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool postSLLastWorkingQueueStillActiveDuringRecentTeardown =
        dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
            postFSRNonFGRecovery, false, postSLLastWorkingQueueStillActiveDuringRecentTeardown)) {
        if (blockedReason) {
            *blockedReason = "post-FSR non-FG recovery is still seeing preserved PostSL teardown traffic";
        }
        return false;
    }

    // Only block when DLSS FG is confirmed active WITH a known multiplier.
    // When DLSS modules are merely loaded but FG is off (or API state is transiently
    // toggling — common when switching to FSR FG), heuristics are safe.  The
    // g_PrimaryGameQueue filter ensures only game-queue ECL calls are counted,
    // preventing false positives from FG runtime queues.
    if (g_FGCompat.IsDLSSFGApiActive()) {
        int mult = g_FGCompat.GetFGMultiplier();
        if (mult >= 2) {
            if (blockedReason) {
                *blockedReason = "DLSS FG is actively generating frames";
            }
            return false;
        }
    }

    if (blockedReason) {
        *blockedReason = nullptr;
    }
    return true;
}

inline bool IsFFXPresentCallbackStalled() {
    if (!dx12_hook_g_FFXPresentCallbackBridgeExpected.load(std::memory_order_acquire)) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    if (lastCallback != 0) {
        constexpr ULONGLONG kStallThresholdMs = 2000;
        return (now - lastCallback) > kStallThresholdMs;
    }
    // The callback has never fired since hook init.  If the runtime has owned
    // the swapchain for several seconds without a single callback, treat it as
    // stalled so the overlay does not stay invisible indefinitely.
    if (dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_FGRuntimeOwnsSwapchainSince != 0) {
        constexpr ULONGLONG kNeverFiredStallThresholdMs = 3000;
        return (now - dx12_hook_g_FGRuntimeOwnsSwapchainSince) > kNeverFiredStallThresholdMs;
    }
    const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) && assumedSince != 0) {
        constexpr ULONGLONG kProgressFallbackNeverFiredStallThresholdMs = 1500;
        return (now - assumedSince) > kProgressFallbackNeverFiredStallThresholdMs;
    }
    return false;
}

inline constexpr ULONGLONG dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs = 5000;

struct ProgressResolvedOfficialFFXOverlayFallbackProof {
    bool proof = false;
    bool progressResolved = false;
    bool hasSwapchainQueue = false;
    bool hasOriginalGameQueue = false;
    bool swapchainQueueMatchesOriginalGameQueue = false;
    bool hasDevice = false;
    ULONGLONG stableMs = 0;
    HRESULT deviceHr = E_POINTER;
};

inline ProgressResolvedOfficialFFXOverlayFallbackProof EvaluateProgressResolvedOfficialFFXOverlayFallbackProof() {
    ProgressResolvedOfficialFFXOverlayFallbackProof result{};
    result.progressResolved = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);

    const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    if (result.progressResolved && assumedSince != 0) {
        const ULONGLONG now = GetTickCount64();
        result.stableMs = (now >= assumedSince) ? (now - assumedSince) : 0;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        result.hasSwapchainQueue = dx12_hook_g_SwapchainQueue != nullptr;
        result.hasOriginalGameQueue = dx12_hook_g_OriginalGameQueue != nullptr;
        result.swapchainQueueMatchesOriginalGameQueue =
            result.hasSwapchainQueue && result.hasOriginalGameQueue && dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue;
    }

    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    result.hasDevice = device != nullptr;
    result.deviceHr = device ? device->GetDeviceRemovedReason() : E_POINTER;

    result.proof = result.progressResolved && result.stableMs >= dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs &&
                   result.swapchainQueueMatchesOriginalGameQueue && result.hasDevice && SUCCEEDED(result.deviceHr);
    return result;
}

// Tracks when the FFX present callback was first detected as stalled and has
// never fired.  Used by the long-timeout escape hatch in the policy function.
inline std::atomic<ULONGLONG> dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs{0};

// Tracks the timestamp when the overlay was first suppressed (set when
// ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain first returns true
// after having returned false).  Reset to 0 when the overlay is allowed
// to render via the normal non-FG path.  If suppression exceeds 2 seconds,
// the overlay is force-rendered regardless of FG state to guarantee a
// maximum 2-second overlay blackout across all FG transitions.
inline std::atomic<ULONGLONG> dx12_hook_g_OverlaySuppressedSinceMs{0};

inline std::atomic<uint32_t> dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount{0};

inline constexpr uint32_t dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents = 8;

inline void ResetFFXPresentCallbackFirstStallDetection() {
    dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.store(0, std::memory_order_release);
}

inline ULONGLONG GetFFXPresentCallbackStallDurationMs() {
    const ULONGLONG firstStallMs = dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.load(std::memory_order_acquire);
    if (firstStallMs == 0) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    return (now >= firstStallMs) ? (now - firstStallMs) : 0;
}

inline void UpdateFFXPresentCallbackFirstStallDetection(bool ffxPresentCallbackStalled) {
    if (!ffxPresentCallbackStalled) {
        return;
    }
    const bool callbackEverFired = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire) != 0;
    if (callbackEverFired) {
        // The callback fired at least once — the stall is transient, not a
        // never-fired scenario.  Do not arm the long-timeout escape hatch.
        return;
    }
    ULONGLONG expected = 0;
    dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.compare_exchange_strong(expected, GetTickCount64(),
                                                                         std::memory_order_acq_rel);
}

inline bool ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(bool ffxPresentCallbackStalled) {
    const bool explicitNativeFSROffPending =
        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
    const bool evaluateFFXCallbackFallback = ce::dx12_overlay_policy::ShouldEvaluateFFXPresentCallbackFallback(
        ffxPresentCallbackStalled, explicitNativeFSROffPending);
    UpdateFFXPresentCallbackFirstStallDetection(ffxPresentCallbackStalled);
    const bool progressResolvedOfficialFFXPresentPath =
        dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
    const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
    const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
        lastCallback, dx12_hook_g_SwapchainQueueCaptureTime, assumedSince);
    const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
        EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
    const ULONGLONG stallDurationMs = GetFFXPresentCallbackStallDurationMs();
    return ce::dx12_overlay_policy::ShouldAllowNormalOverlayFallbackForStalledFFXPresentCallback(
        evaluateFFXCallbackFallback, progressResolvedOfficialFFXPresentPath, directFFXApiConfirmation,
        currentFFXPresentCallbackProof, progressProof.proof, stallDurationMs, explicitNativeFSROffPending);
}

inline void LogSuppressedFFXPresentCallbackStallNormalOverlayFallback() {
    static std::atomic<int> s_suppressedStallFallbackLogCount{0};
    const int logCount = s_suppressedStallFallbackLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount >= 5 && (logCount % 600) != 0) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
    const ULONGLONG assumedSince = dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
    const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
        EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
    HookLogImportant(
        "DX12: FFX present callback appears stalled but normal overlay fallback is unsafe for "
        "this native FSR handoff until direct ffxConfigure/present-callback proof exists "
        "(lastCallback=%llu progressAssumedFor=%llums directFFX=%d explicitNativeOff=%d runtimeOwns=%d "
        "runtime=%s apiFSR=%d nativeFGPath=%d stableProof=%d stableFor=%llums requiredStable=%llums "
        "hasScQ=%d hasOrig=%d sameQueue=%d "
        "hasDevice=%d deviceHr=0x%08X scQueue=%p origGame=%p cmdQ=%p log=%d)",
        lastCallback, assumedSince ? (now - assumedSince) : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0,
        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
        dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
        g_FGCompat.IsFSRFGApiActive() ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
        progressProof.proof ? 1 : 0, progressProof.stableMs, dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs,
        progressProof.hasSwapchainQueue ? 1 : 0, progressProof.hasOriginalGameQueue ? 1 : 0,
        progressProof.swapchainQueueMatchesOriginalGameQueue ? 1 : 0, progressProof.hasDevice ? 1 : 0,
        static_cast<unsigned>(progressProof.deviceHr), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
        g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
}

inline bool ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(const char** reason = nullptr) {
    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        if (reason) {
            *reason = "protected official FFX startup";
        }
        return true;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool nativeFSRInternalNoCallbackComposition =
        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
    const bool ffxStalled = IsFFXPresentCallbackStalled();
    const bool explicitNativeFSROffPending =
        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
    const bool ffxStallAllowsNormalOverlay =
        ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(ffxStalled);
    const uint32_t streamlineNoFGPresentCount =
        dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
            dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
            dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents)) {
        if (reason) {
            *reason = "fresh runtime-owned Streamline no-FG swapchain";
        }
        static std::atomic<int> s_freshStreamlineNoFGSkipLogCount{0};
        const int logCount = s_freshStreamlineNoFGSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 16 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Deferring separate overlay GPU work on fresh runtime-owned Streamline no-FG swapchain "
                "(presentCount=%u settlePresents=%u scQueue=%p origGame=%p cmdQ=%p log=%d)",
                streamlineNoFGPresentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, dx12_hook_g_SwapchainQueue,
                dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
        }
        return true;
    }

    const bool skip = ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
        dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, authoritativeFSRActive,
        runtimeOwnedNativeFGPresentPath, ffxStallAllowsNormalOverlay, nativeFSRInternalNoCallbackComposition,
        DX12_IsFFXUiResourceCachedForBundle(), DX12_IsLiveSwapchainQueueOriginalGameQueue(),
        explicitNativeFSROffPending);
    if (!skip) {
        // Normal (non-override) path says don't skip — reset the suppression
        // timer so the next suppression episode gets a fresh 2-second window.
        dx12_hook_g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

        if (ffxStallAllowsNormalOverlay) {
            static std::atomic<int> s_stallFallbackLogCount{0};
            if (s_stallFallbackLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
                const ULONGLONG ownedSince = dx12_hook_g_FGRuntimeOwnsSwapchainSince;
                const ULONGLONG assumedSince =
                    dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.load(std::memory_order_acquire);
                const ProgressResolvedOfficialFFXOverlayFallbackProof progressProof =
                    EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
                const bool directFFXApiConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
                ULONGLONG currentFFXProofSince = dx12_hook_g_SwapchainQueueCaptureTime;
                if (assumedSince > currentFFXProofSince) {
                    currentFFXProofSince = assumedSince;
                }
                const bool currentFFXPresentCallbackProof = ce::dx12_overlay_policy::IsFFXPresentCallbackProofCurrent(
                    lastCallback, dx12_hook_g_SwapchainQueueCaptureTime, assumedSince);
                HookLogImportant(
                    "DX12: Native FSR fallback proof allows normal overlay rendering "
                    "(lastCallback=%llu ownedFor=%llums ffxStalled=%d "
                    "progressAssumedFor=%llums proofSince=%llu directFFX=%d explicitNativeOff=%d "
                    "currentCallbackProof=%d stableProof=%d stableFor=%llums sameQueue=%d deviceHr=0x%08X "
                    "internalNoCallback=%d) "
                    "— using the game Present path while native FSR presentation is suspended or its callback is quiet",
                    lastCallback, ownedSince ? (GetTickCount64() - ownedSince) : 0, ffxStalled ? 1 : 0,
                    assumedSince ? (GetTickCount64() - assumedSince) : 0,
                    static_cast<unsigned long long>(currentFFXProofSince), directFFXApiConfirmation ? 1 : 0,
                    explicitNativeFSROffPending ? 1 : 0, currentFFXPresentCallbackProof ? 1 : 0,
                    progressProof.proof ? 1 : 0, progressProof.stableMs,
                    progressProof.swapchainQueueMatchesOriginalGameQueue ? 1 : 0,
                    static_cast<unsigned>(progressProof.deviceHr), nativeFSRInternalNoCallbackComposition ? 1 : 0);
            }
        } else if (nativeFSRInternalNoCallbackComposition) {
            static std::atomic<int> s_internalNoCallbackRouteLogCount{0};
            const int logCount = s_internalNoCallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                // This branch (skip=false under no-callback composition) is reached ONLY when the bundle is
                // truly gone: a STALE latch after the game recreated its own native swapchain (live queue back
                // on origGame → backbuffer on the game's own queue is safe), or no UI resource was ever cached.
                // A no-callback SUSPENSION does NOT reach here — it keeps skip=true (the bundle composite draws
                // on CE's own fenced queue; the backbuffer submit would stall the app to ~1 fps, session
                // 20260703_210021).
                const bool uiCached = DX12_IsFFXUiResourceCachedForBundle();
                const bool liveIsOrigGame = DX12_IsLiveSwapchainQueueOriginalGameQueue();
                const char* fallbackReason = liveIsOrigGame ? "stale no-callback latch (live queue back on origGame)"
                                             : !uiCached    ? "no UI resource registered"
                                                            : "no-callback composition";
                HookLogImportant(
                    "DX12: Native FSR no-callback composition — allowing normal/backbuffer overlay rendering "
                    "(%s) (runtime=%s apiFSR=%d nativeFGPath=%d runtimeOwns=%d explicitNativeOff=%d uiCached=%d "
                    "liveQueueIsOrigGame=%d scQueue=%p origGame=%p cmdQ=%p log=%d)",
                    fallbackReason, ce::fg_runtime::GetRuntimeModeName(runtimeMode), authoritativeFSRActive ? 1 : 0,
                    runtimeOwnedNativeFGPresentPath ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                    explicitNativeFSROffPending ? 1 : 0, uiCached ? 1 : 0, liveIsOrigGame ? 1 : 0, dx12_hook_g_SwapchainQueue,
                    dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
            }
        }
        if (reason) {
            *reason = nullptr;
        }
        return false;
    }

    if (ffxStalled && !ffxStallAllowsNormalOverlay) {
        LogSuppressedFFXPresentCallbackStallNormalOverlayFallback();
    }

    if (skip && explicitNativeFSROffPending && runtimeOwnedNativeFGPresentPath) {
        static std::atomic<int> s_retainedNativeFSRSuspendSkipLogCount{0};
        const int logCount = s_retainedNativeFSRSuspendSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
            const ULONGLONG now = GetTickCount64();
            HookLogImportant(
                "DX12: Keeping separate overlay GPU work suppressed during native-FSR suspension; retained FFX "
                "present-callback bridge remains authoritative (runtime=%s callbackEver=%d lastCallbackAge=%llums "
                "ffxStalled=%d fallbackAllowed=%d log=%d)",
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), lastCallback != 0 ? 1 : 0,
                lastCallback && now >= lastCallback ? (now - lastCallback) : 0, ffxStalled ? 1 : 0,
                ffxStallAllowsNormalOverlay ? 1 : 0, logCount + 1);
        }
    }

    // 2-second max overlay suspension enforcement. Ordinary transition stalls
    // can use this as a visibility backstop, but native FSR without direct
    // ffxConfigure/callback proof must stay suppressed. GTA Enhanced removes
    // the device on the first normal overlay ECL in that unproven state.
    {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG suppressedSince = dx12_hook_g_OverlaySuppressedSinceMs.load(std::memory_order_acquire);
        if (suppressedSince == 0) {
            dx12_hook_g_OverlaySuppressedSinceMs.store(now, std::memory_order_release);
            suppressedSince = now;
        }
        constexpr ULONGLONG kMaxOverlaySuppressionMs = 2000;
        if (now >= suppressedSince && (now - suppressedSince) >= kMaxOverlaySuppressionMs) {
            const bool nativeFSRActive = authoritativeFSRActive || ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode);
            const ULONGLONG lastCallback = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
            const bool ffxPresentCallbackEverFired = lastCallback != 0;
            const bool timeoutOverrideAllowed =
                ce::dx12_overlay_policy::ShouldAllowOverlaySuppressionTimeoutOverrideForNativeFSR(
                    runtimeOwnedNativeFGPresentPath, nativeFSRActive, ffxStalled, ffxStallAllowsNormalOverlay);
            if (!timeoutOverrideAllowed) {
                static std::atomic<int> s_timeoutBlockedByNativeFSRLogCount{0};
                const int logCount = s_timeoutBlockedByNativeFSRLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 600) == 0) {
                    HookLogImportant(
                        "DX12: Overlay suppression exceeded 2s but native FSR owns presentation; keeping normal "
                        "overlay GPU work suppressed because the FFX present-callback path is %s "
                        "(elapsed=%llums runtime=%s apiFSR=%d nativeFGPath=%d nativeFSR=%d runtimeOwns=%d "
                        "explicitNativeOff=%d callbackEver=%d lastCallbackAge=%llums ffxStalled=%d "
                        "ffxStallAllows=%d log=%d)",
                        ffxPresentCallbackEverFired && !ffxStalled ? "active" : "not safe for fallback",
                        now - suppressedSince, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
                        g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
                        nativeFSRActive ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0,
                        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) ? 1 : 0,
                        ffxPresentCallbackEverFired ? 1 : 0,
                        ffxPresentCallbackEverFired && now >= lastCallback ? (now - lastCallback) : 0,
                        ffxStalled ? 1 : 0, ffxStallAllowsNormalOverlay ? 1 : 0, logCount + 1);
                }
                if (reason) {
                    *reason = "native FSR present-callback path owns overlay after 2s suppression timeout";
                }
                return true;
            }

            // Do NOT reset g_OverlaySuppressedSinceMs here — keep it so all
            // call sites in the same frame see the same expired timer and
            // independently force-render.  It will be reset when the normal
            // (non-override) path returns false on a future frame.
            if (reason) {
                *reason = "overlay suppression exceeded 2s max duration";
            }
            if (suppressedSince == now) {
                // First frame of suppression — only just started the timer,
                // do not force-render yet.
                return true;
            }
            const auto elapsed = now - suppressedSince;
            HookLogImportant(
                "DX12: Overlay suppression exceeded 2s (%llums) — forcing normal overlay rendering "
                "(runtime=%s apiFSR=%d nativeFGPath=%d runtimeOwns=%d ffxStalled=%d ffxStallAllows=%d)",
                elapsed, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
                g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
                dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, ffxStalled ? 1 : 0, ffxStallAllowsNormalOverlay ? 1 : 0);
            return false;
        }
    }

    if (reason) {
        if (authoritativeFSRActive || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG) {
            *reason = "runtime-owned native FSR FG swapchain";
        } else if (runtimeOwnedNativeFGPresentPath) {
            *reason = "runtime-owned native FSR Present teardown window";
        } else {
            *reason = "runtime-owned swapchain";
        }
    }
    return true;
}

inline void SyncSecondaryDx12OverlayColorState(DXGI_FORMAT format) {
    const bool isHdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
    dx12_hook_g_D3D11On12Adapter.SetHDR(isHdr, static_cast<int>(format));

    static std::atomic<int> s_lastFormat{-1};
    static std::atomic<int> s_lastHdr{-1};
    const int previousFormat = s_lastFormat.exchange(static_cast<int>(format), std::memory_order_acq_rel);
    const int previousHdr = s_lastHdr.exchange(isHdr ? 1 : 0, std::memory_order_acq_rel);
    if (previousFormat != static_cast<int>(format) || previousHdr != (isHdr ? 1 : 0)) {
        HookLogImportant("DX12: Secondary overlay color contract synchronized format=%d hdr=%d",
                         static_cast<int>(format), isHdr ? 1 : 0);
    }
}

inline bool ResolveSwapchainOutputHDRState(IDXGISwapChain* swapchain, DXGI_FORMAT format, const char* logPrefix,
                                           int* outColorSpace = nullptr, bool* outSupported = nullptr) {
    if (outColorSpace) {
        *outColorSpace = -1;
    }
    if (outSupported)
        *outSupported = false;

    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool hasTrackedColorSpace = false;
    const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(
        swapchain, format, &colorSpace, &hasTrackedColorSpace);
    const bool supported = encoding != ce::presentation_color::Encoding::Unsupported;
    const bool isActualHDR = ce::presentation_color::IsHDR(encoding);
    if (outColorSpace && hasTrackedColorSpace)
        *outColorSpace = static_cast<int>(colorSpace);
    if (outSupported)
        *outSupported = supported;
    if (logPrefix) {
        HookLogImportant("%s - format=%d tracked=%d colorSpace=%d encoding=%s isHDR=%d", logPrefix,
                         static_cast<int>(format), hasTrackedColorSpace ? 1 : 0, static_cast<int>(colorSpace),
                         ce::presentation_color::Describe(encoding), isActualHDR ? 1 : 0);
    }
    return isActualHDR;
}

inline void ResetFFXPresentCallbackOverlayBackend(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);
    if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
        HookLogImportant("%s — resetting native FSR present-callback overlay adapter", reason);
        dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
    }
    dx12_hook_g_FFXPresentOverlayDevice = nullptr;
    dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
    if (dx12_hook_g_FFXPresentRtvHeap) {
        dx12_hook_g_FFXPresentRtvHeap->Release();
        dx12_hook_g_FFXPresentRtvHeap = nullptr;
    }
}

inline void ForceClearNativeFSRInternalNoCallbackComposition(const char* reason) {
    if (dx12_hook_g_NativeFSRInternalNoCallbackComposition.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared retained native FSR internal no-callback composition route (%s)", reason);
        // Re-arm the VEH breakpoint for the next FG-on transition (one-shot detection cycle reset).
        FFXHook_ResetVehDisarmAndRearm();
    }
}

// ---- FFX UI-resource overlay composition (no-app-callback FSR FG; rides the game's UI composition) ----
// AMD's FfxFrameInterpolationSwapchain composites a registered "UI resource" onto BOTH real and generated
// frames POST-interpolation on AMD's OWN queue. GTA Enhanced registers its HUD this way EVERY frame
// (ffxConfigure type=0x00030002 on the swapchain context). CE intercepts that configure and draws the
// inject overlay ONTO the registered UI texture, submitting on the GAME's original queue — NOT AMD's
// runtime present queue. This is the ONLY route that puts the overlay on FG frames with zero perturbation
// of AMD's pacing-critical present queue (so no ffxQuery wedge) and no ghosting (composited after
// interpolation). It replaces the old separate-ECL-on-AMD's-runtime-queue route, which wedged AMD's
// presenter in ~200-300 frames (session 20260618_201038), and the synthesized-callback route (~8 frames).
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex dx12_hook_g_FFXUiCompositeMutex;

// Step 2 revised: CE's OWN dedicated queue for the UI-composite submit. AMD does NOT track this queue
// (it tracks the game queue and its own runtime present queue). Submitting here + signaling the fence here
// + CPU-waiting for completion before forwarding RegisterUiResource means zero extra ECL and zero extra
// Signal on any AMD-tracked queue → no ffxQuery pacing wedge. The UI texture is a game-owned committed
// resource (not a swapchain backbuffer), so cross-queue writes from a DIRECT queue are legal with barriers.
inline ID3D12CommandQueue* dx12_hook_g_FFXUiCompositeQueue = nullptr;

inline ID3D12Fence* dx12_hook_g_FFXUiCompositeFence =
    nullptr;  // signaled on g_FFXUiCompositeQueue (CE's own queue, not the game queue)

inline UINT64 dx12_hook_g_FFXUiCompositeFenceVal = 0;

inline int dx12_hook_g_FFXUiCompositeFrame = 0;

// --- FFX UI-composite timeline ring buffer (freeze diagnosis) -----------------------------------------
// Records the last kFFXUiCompositeTimelineSize composite calls with QPC stamps, fence state, and game-ECL
// context so a freeze dump shows the PROGRESSION into the wedge (not just the final frame's state).
// Written from the render thread under g_FFXUiCompositeMutex; read by the freeze watchdog at freeze time
// (render thread is stuck, so the race is benign — a snapshot scan of all slots).
struct FFXUiCompositeTimelineEntry {
    uint64_t frame;           // g_FFXUiCompositeFrame at record time
    uint64_t fenceVal;        // g_FFXUiCompositeFenceVal at record time (0 if no game-queue Signal)
    uint64_t fenceCompleted;  // g_FFXUiCompositeFence->GetCompletedValue() at record time
    uint64_t submitQpc;       // QPC at ECL submit
    uint64_t returnQpc;       // QPC at composite-function return (or wait-return if a wait occurred)
    uint32_t waitTimedOut;    // 1 if the completion wait timed out
    uint32_t slot;            // allocator slot used
    uint32_t gameEclCount;    // g_CommandListsExecutedThisFrame at composite time
    void* uiTexture;          // the UI texture pointer
    uint32_t ffxState;        // the FFX resource state
    void* queue;              // the queue used for submission
};

inline constexpr int dx12_hook_kFFXUiCompositeTimelineSize = 32;

// The FFX game/presentation queue is normally captured from the DX12 FrameGenerationSwapChain creation
// descriptor. If that call was already in flight when interception became live, the retained pre-FSR original
// game queue is the recoverable equivalent; the nested DXGI create queue is FFX's internal presentQueue and is
// never bound here. A proven Streamline wrapper also retains CE's real original game queue so target-device
// validation can select its underlying submission path. Bindings are keyed by raw proxy identity without
// retaining the proxy itself: AddRef'ing a startup/takeover swapchain pins its HWND and can make replacement
// creation fail.
struct NativeFSRSwapchainQueueBinding {
    void* context = nullptr;
    ID3D12CommandQueue* descriptorQueue = nullptr;
    ID3D12CommandQueue* underlyingGameQueue = nullptr;
    bool descriptorQueueUsesAcceptedStreamlineDevice = false;
    bool recoveredOriginalGameQueue = false;
};

struct AcquiredNativeFSROwnerQueue {
    ID3D12CommandQueue* queue = nullptr;
    ce::dx12_overlay_policy::NativeFSROwnerQueueRoute route =
        ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kUnavailable;
};

inline bool UpdateHeuristicFSRFGState(bool active, const char* source) {
    if (active && ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(
                      dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                      dx12_hook_g_FGRuntimeOwnsSwapchain)) {
        g_FGCompat.SetHeuristicFSRFGActive(false);

        static std::atomic<int> s_explicitOffSuppressedLogCount{0};
        if (s_explicitOffSuppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "DX12: Suppressing %s FSR FG heuristic because native FSR explicitly turned FG off while the "
                "runtime-owned "
                "swapchain teardown is still active",
                source ? source : "unknown");
        }
        return false;
    }

    const char* blockedReason = nullptr;
    if (!CanUseFSRFGHeuristics(&blockedReason)) {
        g_FGCompat.SetHeuristicFSRFGActive(false);

        if (active) {
            static std::atomic<int> s_suppressedLogCount{0};
            if (s_suppressedLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                HookLog("DX12: Suppressing %s FSR FG heuristic because %s", source,
                        blockedReason ? blockedReason : "it is unsafe");
            }
        }
        return false;
    }

    g_FGCompat.SetHeuristicFSRFGActive(active);
    return true;
}

inline void CleanupOverlay() {
    CleanupOverlay(false);
}

inline void DrawOverlay(ID3D12GraphicsCommandList* list, bool isRealFrame, UINT bufferIdx,
                        D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride = nullptr);

inline constexpr ULONGLONG dx12_hook_kStartupOverlayWindowPollMs = 100;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayInitGraceMs = 500;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayWarmupMs = 500;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayQuietPeriodMs = 200;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostResumeSettleMs = 100;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostBackendInitSettleMs = 0;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostRTVInitSettleMs = 0;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostSyncInitSettleMs = 100;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostResourcePrimeSettleMs = 100;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayFirstDrawProbeSettleMs = 0;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayLoadedRenderModuleMaxBlockMs = 500;

inline constexpr ULONGLONG dx12_hook_kStartupOverlayRenderModuleQuietPeriodMs = 500;

inline constexpr DWORD dx12_hook_kOverlayCrossQueueWaitMs = 16;

inline StartupOverlayActivationStage dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;

inline StartupOverlayFirstDrawProbeStage dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;

inline ULONGLONG dx12_hook_s_startupOverlayActivationStageMs = 0;

inline ULONGLONG dx12_hook_s_startupOverlaySyncInitMs = 0;

inline ULONGLONG dx12_hook_s_startupOverlayResourcePrimeMs = 0;

inline ULONGLONG dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;

inline std::atomic<ULONGLONG> dx12_hook_s_lastStartupBlockingRenderModuleActivityMs{0};

// Once we have observed a stable real-game overlay draw, later Rockstar/EOS
// popups should be treated as normal coexistence instead of re-entering the
// fragile startup-only suppression path. Later swapchain/sync reinitializations
// should also keep the normal allocator pool.
inline std::atomic<bool> dx12_hook_s_startupOverlayCompatSettled{false};

inline std::atomic<bool> dx12_hook_s_startupOverlayObservedAnyFG{false};

inline std::atomic<bool> dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff{false};

inline void ResetStartupOverlayBackendActivationStage();

inline bool IsStartupOverlayCompatibilityActive() {
    return ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(
        ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr, IsActualFrameGenerationActive(),
        dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
        dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_FGRuntimeOwnsSwapchain);
}

inline bool ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff() {
    return ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
        dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit,
        dx12_hook_g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
        g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
        dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_HadFSRFGPhase, dx12_hook_g_OriginalGameQueue != nullptr);
}

inline void UpdateStartupOverlayCompatibilityState() {
    const bool actualFGActive = IsActualFrameGenerationActive();

    if (actualFGActive) {
        dx12_hook_s_startupOverlayObservedAnyFG.store(true, std::memory_order_release);
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(false, std::memory_order_release);
        return;
    }

    const bool startupBlockingOverlayLoaded = ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr;
    if (!startupBlockingOverlayLoaded || !dx12_hook_g_FGRuntimeOwnsSwapchain) {
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
    }

    const bool observedAnyFrameGenerationActivity = dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire);
    const bool startupCompatSettled = dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire);
    const bool lateRuntimeOwnedHandoffJustObserved =
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.exchange(false, std::memory_order_acq_rel);
    const bool preserveLiveOverlayDuringHandoff =
        ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
    if (!ce::dx12_overlay_policy::ShouldRearmStartupOverlayCompatibilityForLateRuntimeOwnedSwapchain(
            startupBlockingOverlayLoaded, actualFGActive, startupCompatSettled, dx12_hook_g_FGRuntimeOwnsSwapchain,
            observedAnyFrameGenerationActivity, lateRuntimeOwnedHandoffJustObserved,
            preserveLiveOverlayDuringHandoff)) {
        if (lateRuntimeOwnedHandoffJustObserved && preserveLiveOverlayDuringHandoff) {
            HookLogImportant(
                "DX12: Keeping settled startup overlay live through runtime-inactive Streamline handoff "
                "(overlayInit=%d syncInit=%d runtime=%s origGame=%p)",
                dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0,
                ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_OriginalGameQueue);
        }
        return;
    }

    if (dx12_hook_s_startupOverlayCompatSettled.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Re-arming startup overlay compatibility after late runtime-owned swapchain handoff before any real "
            "FG activity");
        ResetStartupOverlayBackendActivationStage();
    }
}

inline const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage) {
    switch (stage) {
        case StartupOverlayFirstDrawProbeStage::kBackbufferTouchOnly:
            return "backbuffer touch";
        case StartupOverlayFirstDrawProbeStage::kPipelineStateOnly:
            return "pipeline state setup";
        case StartupOverlayFirstDrawProbeStage::kActualRender:
            return "real overlay draw";
        case StartupOverlayFirstDrawProbeStage::kComplete:
            return "complete";
        case StartupOverlayFirstDrawProbeStage::kNone:
        default:
            return "overlay probe";
    }
}

inline void ResetStartupOverlayBackendActivationStage() {
    dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
    dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
    dx12_hook_s_startupOverlayActivationStageMs = 0;
    dx12_hook_s_startupOverlaySyncInitMs = 0;
    dx12_hook_s_startupOverlayResourcePrimeMs = 0;
    dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
    dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.store(0, std::memory_order_release);
}

inline bool IsActualFrameGenerationActive() {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    return runtimeMode == ce::fg_runtime::RuntimeMode::kDLSSFG || runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
}

inline bool IsFSRFrameGenerationActive() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kFSRFG;
}

inline bool IsNvidiaSmoothMotionActiveRuntime() {
    return g_FGCompat.GetRuntimeMode() == ce::fg_runtime::RuntimeMode::kNvidiaSmoothMotion;
}

inline ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);

inline bool IsStreamlineLoaded();

inline bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule = nullptr) {
    const char* overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
    const bool actualFGActive = IsActualFrameGenerationActive();
    const bool fsrFGActive = IsFSRFrameGenerationActive();
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool runtimeOwnsSwapchain = dx12_hook_g_FGRuntimeOwnsSwapchain;
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();

    // When Streamline FG is active, do NOT use a dedicated overlay queue.
    // D3D12 rejects cross-queue access to swapchain backbuffers with
    // DXGI_ERROR_ACCESS_DENIED during SL FG (SL takes exclusive control
    // of the swapchain queue association).  Render on the game queue
    // instead, skipping fence operations to avoid interfering with SL's
    // internal frame synchronization.
    if (streamlineFGRunning) {
        if (disabledByOverlayModule)
            *disabledByOverlayModule = nullptr;
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldDisableDedicatedOverlayQueueForRuntimeOwnedFrameGeneration(
            actualFGActive, fsrFGActive, streamlineFGRunning, runtimeOwnsSwapchain, runtimeOwnedNativeFGPresentPath)) {
        static std::atomic<int> s_runtimeOwnedDedicatedQueueDisableLogCount{0};
        int logCount = s_runtimeOwnedDedicatedQueueDisableLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Dedicated overlay queue disabled for native/runtime-owned FG "
                "(fsrFG=%d runtimeOwns=%d nativePresentPath=%d scQueue=%p origGame=%p)",
                fsrFGActive ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0, runtimeOwnedNativeFGPresentPath ? 1 : 0,
                dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue);
        }
        if (disabledByOverlayModule)
            *disabledByOverlayModule = nullptr;
        return false;
    }

    const bool shouldUseDedicatedQueue =
        ce::overlay_compat::ShouldUseDedicatedDX12OverlayQueue(actualFGActive, processNeedsDelay, overlayModule);
    if (disabledByOverlayModule) {
        *disabledByOverlayModule = shouldUseDedicatedQueue ? nullptr : overlayModule;
    }

    return shouldUseDedicatedQueue;
}

inline bool WaitForGameQueueBeforeDedicatedOverlaySubmission(ID3D12CommandQueue* gameQueue, const char* phase) {
    if (!dx12_hook_g_State.overlayQueue || !dx12_hook_g_State.crossQueueFence || !dx12_hook_g_State.crossQueueFenceEvent) {
        return true;
    }
    if (!gameQueue) {
        HookLogImportant("DX12: Cannot synchronize dedicated overlay queue before %s because the game queue is null",
                         phase ? phase : "overlay submission");
        return false;
    }

    const UINT64 waitValue = dx12_hook_g_State.crossQueueFenceValue + 1;
    HRESULT signalHr = gameQueue->Signal(dx12_hook_g_State.crossQueueFence, waitValue);
    if (FAILED(signalHr)) {
        HookLogImportant("DX12: Failed to signal game queue before %s on dedicated overlay queue hr=0x%08X",
                         phase ? phase : "overlay submission", signalHr);
        return false;
    }

    dx12_hook_g_State.crossQueueFenceValue = waitValue;
    if (dx12_hook_g_State.crossQueueFence->GetCompletedValue() >= waitValue) {
        return true;
    }

    HRESULT setHr = dx12_hook_g_State.crossQueueFence->SetEventOnCompletion(waitValue, dx12_hook_g_State.crossQueueFenceEvent);
    if (FAILED(setHr)) {
        HookLogImportant("DX12: Failed to arm cross-queue wait before %s hr=0x%08X",
                         phase ? phase : "overlay submission", setHr);
        return false;
    }

    DWORD waitHr = WaitForSingleObject(dx12_hook_g_State.crossQueueFenceEvent, dx12_hook_kOverlayCrossQueueWaitMs);
    if (waitHr == WAIT_OBJECT_0) {
        static std::atomic<int> s_crossQueueWaitSuccessLogCount{0};
        if (s_crossQueueWaitSuccessLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant("DX12: Dedicated overlay queue synchronized with game queue for %s (value=%llu)",
                             phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
        }
        return true;
    }

    if (waitHr == WAIT_TIMEOUT) {
        HookLogImportant("DX12: Timed out waiting for game queue before %s on dedicated overlay queue (value=%llu)",
                         phase ? phase : "overlay submission", static_cast<unsigned long long>(waitValue));
    } else {
        HookLogImportant("DX12: WaitForSingleObject failed before %s on dedicated overlay queue result=%lu",
                         phase ? phase : "overlay submission", waitHr);
    }
    return false;
}

// Probe the real D3D12 ECL by creating a temporary COMPUTE queue.
// SL only vtable-hooks DIRECT queues for FG; COMPUTE queues keep the
// pristine d3d12.dll function pointer.  When DIRECT and COMPUTE queues
// share the same vtable (all hooks applied to the shared vtable), we
// fall back to scanning SL's hook for an indirect JMP/CALL target.
inline void ProbeRealD3D12ECL(ID3D12Device* device) {
    if (dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire))
        return;
    if (!device)
        return;

    // Create a temporary COMPUTE queue
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    ID3D12CommandQueue* probeQueue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue));
    if (FAILED(hr) || !probeQueue) {
        HookLogImportant("DX12: ECL probe - COMPUTE queue creation failed (hr=0x%08X)", (unsigned)hr);
        return;
    }

    void** probeVtable = *(void***)probeQueue;
    void* probeECL = probeVtable[10];
    void* probeSignal = probeVtable[14];  // Signal is at vtable[14] on ID3D12CommandQueue

    // Check which module owns the COMPUTE queue's ECL
    HMODULE probeModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)probeECL, &probeModule);
    char probeMod[MAX_PATH] = {};
    if (probeModule)
        GetModuleFileNameA(probeModule, probeMod, MAX_PATH);

    // Check which module owns the COMPUTE queue's Signal
    HMODULE probeSignalModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)probeSignal, &probeSignalModule);
    char probeSignalMod[MAX_PATH] = {};
    if (probeSignalModule)
        GetModuleFileNameA(probeSignalModule, probeSignalMod, MAX_PATH);

    // Compare with the current DIRECT queue's vtable[10] (our hooked version)
    ID3D12CommandQueue* directQueue = dx12_hook_g_SwapchainQueue;
    void* directECL = nullptr;
    char directMod[MAX_PATH] = {};
    if (directQueue) {
        void** directVtable = *(void***)directQueue;
        directECL = directVtable[10];
        HMODULE dMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)directECL, &dMod);
        if (dMod)
            GetModuleFileNameA(dMod, directMod, MAX_PATH);
    }

    bool sameVtable = (probeVtable == (directQueue ? *(void***)directQueue : nullptr));
    bool sameECL = (probeECL == directECL);
    bool probeIsD3D12 = (strstr(probeMod, "d3d12") != nullptr || strstr(probeMod, "D3D12") != nullptr);
    bool probeSignalIsD3D12 =
        (strstr(probeSignalMod, "d3d12") != nullptr || strstr(probeSignalMod, "D3D12") != nullptr);

    HookLogImportant("DX12: ECL probe - COMPUTE ECL=%p (%s), DIRECT ECL=%p (%s), sameVtable=%d sameECL=%d isD3D12=%d",
                     probeECL, probeMod, directECL, directMod, sameVtable ? 1 : 0, sameECL ? 1 : 0,
                     probeIsD3D12 ? 1 : 0);

    if (probeIsD3D12) {
        dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)probeECL, std::memory_order_release);
        HookLogImportant("DX12: Real D3D12 ECL found via COMPUTE probe: %p", probeECL);
    }

    // Probe the real D3D12 Signal from the COMPUTE queue's vtable
    if (probeSignalIsD3D12 && !dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
        dx12_hook_g_RealD3D12Signal.store(reinterpret_cast<SignalPtr>(probeSignal), std::memory_order_release);
        HookLogImportant("DX12: Real D3D12 Signal found via COMPUTE probe: %p (%s)", probeSignal, probeSignalMod);
    }

    // Always check saved original — in GTA V both COMPUTE and DIRECT share
    // the same vtable (sameECL=1) so our hook is on both, but
    // oExecuteCommandLists still holds the real D3D12 function.
    if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            HMODULE origMod = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)savedOrig, &origMod);
            char origModName[MAX_PATH] = {};
            if (origMod)
                GetModuleFileNameA(origMod, origModName, MAX_PATH);
            bool origIsD3D12 = (strstr(origModName, "d3d12") != nullptr || strstr(origModName, "D3D12") != nullptr);
            HookLogImportant("DX12: ECL probe - saved oECL=%p (%s) isD3D12=%d", (void*)savedOrig, origModName,
                             origIsD3D12 ? 1 : 0);
            if (origIsD3D12) {
                dx12_hook_g_RealD3D12ECL.store(savedOrig, std::memory_order_release);
                HookLogImportant("DX12: Real D3D12 ECL found via saved original: %p", (void*)savedOrig);
            }
        }
    }

    // If still not found, try to follow the saved original's JMP chain
    if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
        if (savedOrig) {
            const uint8_t* fn = (const uint8_t*)savedOrig;
            void* target = nullptr;
            // Check for E9 rel32 (JMP rel32) — SL's hook might be a simple JMP
            if (fn[0] == 0xE9) {
                int32_t rel = *(const int32_t*)(fn + 1);
                target = (void*)(fn + 5 + rel);
            }
            // Check for FF 25 (JMP [rip+disp32]) — indirect JMP
            else if (fn[0] == 0xFF && fn[1] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 2);
                void** addr = (void**)(fn + 6 + disp);
                target = *addr;
            }
            // Check for 48 FF 25 (REX.W JMP [rip+disp32])
            else if (fn[0] == 0x48 && fn[1] == 0xFF && fn[2] == 0x25) {
                int32_t disp = *(const int32_t*)(fn + 3);
                void** addr = (void**)(fn + 7 + disp);
                target = *addr;
            }

            if (target) {
                HMODULE targetMod = nullptr;
                GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)target, &targetMod);
                char targetModName[MAX_PATH] = {};
                if (targetMod)
                    GetModuleFileNameA(targetMod, targetModName, MAX_PATH);
                bool isD3D12 = (strstr(targetModName, "d3d12") != nullptr || strstr(targetModName, "D3D12") != nullptr);
                HookLogImportant("DX12: ECL probe - followed JMP chain: target=%p (%s) isD3D12=%d", target,
                                 targetModName, isD3D12 ? 1 : 0);
                if (isD3D12) {
                    dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)target, std::memory_order_release);
                    HookLogImportant("DX12: Real D3D12 ECL found via JMP chain: %p", target);
                }
            }
        }
    }

    if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
        HookLogImportant(
            "DX12: ECL probe - FAILED to find real D3D12 ECL! "
            "Overlay will be disabled during SL FG to prevent crash");
    }

    probeQueue->Release();
}

inline bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex,
                                     const char* phase, bool requireGameQueueDrain) {
    // Use the dedicated queue only when FG is actually active.  The queue stays
    // alive across FG mode switches to avoid destructive reinit, but submissions
    // go to the game queue when FG is inactive.
    bool useDedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
    ID3D12CommandQueue* submitQueue = useDedicated ? dx12_hook_g_State.overlayQueue : gameQueue;
    if (!submitQueue || !list) {
        HookLogImportant("DX12: Cannot submit %s (submitQueue=%p, list=%p)", phase ? phase : "overlay command list",
                         submitQueue, list);
        return false;
    }

    if (requireGameQueueDrain && submitQueue != gameQueue &&
        !WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, phase)) {
        return false;
    }

    static std::atomic<int> s_submitLogCount{0};
    if (s_submitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLogImportant("DX12: Submitting %s on %s queue (submitQueue=%p, gameQueue=%p, allocator=%d)",
                         phase ? phase : "overlay command list",
                         submitQueue == gameQueue ? "game" : "dedicated overlay", submitQueue, gameQueue,
                         allocatorIndex);
    }

    ID3D12CommandList* lists[] = {list};

    // When using the dedicated overlay queue during SL FG, use the REAL
    // D3D12 ECL (bypassing SL's vtable hook) to prevent SL's internal
    // state tracking from seeing our overlay command lists.
    // ALSO prefer realECL in non-FG mode to avoid going through stale
    // SL/hook vtable entries after FG teardown (same logic as main path).
    ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    bool slActive = IsStreamlineLoaded() && IsActualFrameGenerationActive();
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard(phase ? phase : "overlay command list");
        if (realECL && (!useDedicated || slActive)) {
            realECL(submitQueue, 1, lists);
        } else {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
            if (origECL) {
                origECL(submitQueue, 1, lists);
            } else {
                submitQueue->ExecuteCommandLists(1, lists);
            }
        }
    }

    if (dx12_hook_g_State.fence) {
        UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
        HRESULT signalHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
        if (SUCCEEDED(signalHr)) {
            dx12_hook_g_State.currentFenceValue = next;
            if (allocatorIndex >= 0 && allocatorIndex < static_cast<int>(dx12_hook_g_State.fenceValues.size())) {
                dx12_hook_g_State.fenceValues[allocatorIndex] = next;
            }
        } else {
            HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X", phase ? phase : "overlay command list",
                    signalHr);
        }
    }

    return true;
}

inline void NoteStartupBlockingRenderModuleActivityFromECL(ID3D12CommandQueue* queue, const void* callerAddress) {
    // Fast early-out: once overlay probe is complete, no need to track anymore
    if (dx12_hook_s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kComplete) {
        return;
    }

    if (!queue || !callerAddress ||
        !ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) ||
        IsActualFrameGenerationActive() ||
        g_FGCompat.IsFGActive()) {  // Also skip for heuristic FG (FSR FG) — avoids GetModuleHandleExA overhead
        return;
    }

    const char* blockingRenderModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
    if (!blockingRenderModule) {
        return;
    }

    // Cache the blocking module handle to avoid GetModuleHandleA kernel call
    // on every ECL invocation. The module won't unload during gameplay.
    static HMODULE s_cachedBlockingModule = nullptr;
    static bool s_cachedBlockingModuleLookedUp = false;
    if (!s_cachedBlockingModuleLookedUp) {
        s_cachedBlockingModule = GetModuleHandleA(blockingRenderModule);
        s_cachedBlockingModuleLookedUp = true;
    }
    HMODULE blockingModuleHandle = s_cachedBlockingModule;
    if (!blockingModuleHandle) {
        // Module not loaded yet — retry next time
        s_cachedBlockingModuleLookedUp = false;
        return;
    }

    HMODULE callerModuleHandle = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(callerAddress), &callerModuleHandle) ||
        !callerModuleHandle || callerModuleHandle != blockingModuleHandle) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.store(now, std::memory_order_release);

    static std::atomic<int> s_blockingModuleActivityLogCount{0};
    if (s_blockingModuleActivityLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        char modulePath[MAX_PATH] = {};
        const char* moduleForLog = blockingRenderModule;
        if (GetModuleFileNameA(callerModuleHandle, modulePath, MAX_PATH) > 0) {
            moduleForLog = modulePath;
        }
        HookLogImportant(
            "DX12: Startup-blocking render module activity detected via ExecuteCommandLists (module=%s, queue=%p, "
            "caller=%p)",
            moduleForLog, queue, callerAddress);
    }
}

inline bool ShouldSuppressOverlayForStartupCompat(
    HWND gameWindow, const char** overlayModule = nullptr, ULONGLONG* remainingMs = nullptr,
    ce::overlay_compat::AuxiliaryProcessWindowInfo* activeWindow = nullptr) {
    const bool startupCompatActive = IsStartupOverlayCompatibilityActive();
    const char* blockingOverlayModule =
        startupCompatActive ? ce::overlay_compat::GetStartupBlockingOverlayModuleName() : nullptr;
    const bool actualFGActive = IsActualFrameGenerationActive();
    static ULONGLONG s_firstOverlayDetectedMs = 0;
    static ULONGLONG s_lastPollMs = 0;
    static ULONGLONG s_lastVisibleMs = 0;
    static bool s_auxiliaryWindowVisible = false;
    static ce::overlay_compat::AuxiliaryProcessWindowInfo s_auxiliaryWindow = {};
    if (overlayModule) {
        *overlayModule = blockingOverlayModule;
    }
    if (remainingMs) {
        *remainingMs = 0;
    }
    if (activeWindow) {
        *activeWindow = {};
    }

    if (!startupCompatActive || !blockingOverlayModule || actualFGActive || !IsWindow(gameWindow)) {
        s_firstOverlayDetectedMs = 0;
        s_lastPollMs = 0;
        s_lastVisibleMs = 0;
        s_auxiliaryWindowVisible = false;
        s_auxiliaryWindow = {};
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstOverlayDetectedMs == 0) {
        s_firstOverlayDetectedMs = now;
    }

    if (s_lastPollMs == 0 || now - s_lastPollMs >= dx12_hook_kStartupOverlayWindowPollMs) {
        s_lastPollMs = now;

        ce::overlay_compat::AuxiliaryProcessWindowInfo visibleWindow = {};
        s_auxiliaryWindowVisible =
            ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), gameWindow, &visibleWindow);
        if (s_auxiliaryWindowVisible) {
            s_lastVisibleMs = now;
            s_auxiliaryWindow = visibleWindow;
        } else {
            s_auxiliaryWindow = {};
        }
    }

    if (activeWindow) {
        *activeWindow = s_auxiliaryWindow;
    }

    const ULONGLONG msSinceOverlayDetected = now - s_firstOverlayDetectedMs;
    const ULONGLONG msSinceLastVisible = s_lastVisibleMs == 0 ? dx12_hook_kStartupOverlayQuietPeriodMs : (now - s_lastVisibleMs);
    const bool suppress = ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(
        true, actualFGActive, s_auxiliaryWindowVisible, msSinceOverlayDetected, dx12_hook_kStartupOverlayWarmupMs,
        msSinceLastVisible, dx12_hook_kStartupOverlayQuietPeriodMs);
    if (remainingMs && suppress) {
        ULONGLONG warmupRemaining =
            msSinceOverlayDetected < dx12_hook_kStartupOverlayWarmupMs ? (dx12_hook_kStartupOverlayWarmupMs - msSinceOverlayDetected) : 0;
        ULONGLONG quietRemaining = !s_auxiliaryWindowVisible && msSinceLastVisible < dx12_hook_kStartupOverlayQuietPeriodMs
                                       ? (dx12_hook_kStartupOverlayQuietPeriodMs - msSinceLastVisible)
                                       : 0;
        *remainingMs = std::max(warmupRemaining, quietRemaining);
    }
    return suppress;
}

inline bool ShouldDeferOverlayInitForStartupCompat(HWND gameWindow, ULONGLONG* remainingMs = nullptr) {
    static ULONGLONG s_firstDeferredInitEligibleMs = 0;
    if (remainingMs) {
        *remainingMs = 0;
    }

    if (!IsStartupOverlayCompatibilityActive() || !IsWindow(gameWindow)) {
        s_firstDeferredInitEligibleMs = 0;
        return false;
    }

    if (dx12_hook_g_State.overlayInit || ce::overlay_compat::GetStartupBlockingOverlayModuleName()) {
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_firstDeferredInitEligibleMs == 0) {
        s_firstDeferredInitEligibleMs = now;
    }

    const ULONGLONG elapsedMs = now - s_firstDeferredInitEligibleMs;
    if (elapsedMs >= dx12_hook_kStartupOverlayInitGraceMs) {
        return false;
    }

    if (remainingMs) {
        *remainingMs = dx12_hook_kStartupOverlayInitGraceMs - elapsedMs;
    }
    return true;
}

inline bool ShouldDelayOverlayInitAfterStartupResumeCompat(bool allowOverlayRender, HWND gameWindow,
                                                           bool runtimeOwnedSwapchainActive,
                                                           ULONGLONG* remainingMs = nullptr) {
    static bool s_hadStartupSuppression = false;
    static ULONGLONG s_resumeStableSinceMs = 0;
    static HWND s_resumeWindow = nullptr;
    static HWND s_loggedSameProcessResumeWindow = nullptr;
    static HWND s_loggedUnusableResumeGameWindow = nullptr;
    static HWND s_loggedUnusableResumeForegroundWindow = nullptr;
    if (remainingMs) {
        *remainingMs = 0;
    }

    const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
    const bool actualFGActive = IsActualFrameGenerationActive();
    if (!processNeedsDelay || actualFGActive) {
        s_hadStartupSuppression = false;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        s_loggedSameProcessResumeWindow = nullptr;
        s_loggedUnusableResumeGameWindow = nullptr;
        s_loggedUnusableResumeForegroundWindow = nullptr;
        return false;
    }

    if (!allowOverlayRender) {
        s_hadStartupSuppression = true;
        s_resumeStableSinceMs = 0;
        s_resumeWindow = nullptr;
        s_loggedSameProcessResumeWindow = nullptr;
        s_loggedUnusableResumeGameWindow = nullptr;
        s_loggedUnusableResumeForegroundWindow = nullptr;
        return false;
    }

    if (!s_hadStartupSuppression || !IsWindow(gameWindow)) {
        return false;
    }

    if (runtimeOwnedSwapchainActive) {
        // Require a fresh stable post-startup window after runtime queue
        // ownership returns to the game. GTA 5 Enhanced can briefly leave the
        // Social Club startup window while the live swapchain is still bound to
        // a runtime-owned queue, and drawing in that handoff window trips
        // ERR_GFX_STATE.
        s_resumeStableSinceMs = 0;
        s_resumeWindow = gameWindow;
        return true;
    }

    RECT clientRect = {};
    LONG width = 0;
    LONG height = 0;
    if (GetClientRect(gameWindow, &clientRect)) {
        width = clientRect.right - clientRect.left;
        height = clientRect.bottom - clientRect.top;
    }

    const DWORD expectedProcessId = GetCurrentProcessId();
    const HWND foregroundWindow = GetForegroundWindow();
    LONG foregroundWidth = 0;
    LONG foregroundHeight = 0;
    const bool exactWindowForeground = (foregroundWindow == gameWindow);
    const ULONGLONG now = GetTickCount64();
    const bool usableSameProcessForegroundWindow =
        !exactWindowForeground && ce::overlay_compat::IsUsableSameProcessForegroundWindow(
                                      foregroundWindow, expectedProcessId, &foregroundWidth, &foregroundHeight);
    bool usingSameProcessForegroundWindow = false;
    const bool trackableForegroundWindow = ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
        exactWindowForeground, usableSameProcessForegroundWindow, width, height, foregroundWidth, foregroundHeight,
        &width, &height, &usingSameProcessForegroundWindow);
    if (!trackableForegroundWindow) {
        if (s_loggedUnusableResumeGameWindow != gameWindow ||
            s_loggedUnusableResumeForegroundWindow != foregroundWindow) {
            HookLogImportant(
                "DX12: Startup-overlay resume still waiting for a usable foreground window "
                "(swapchainWindow=%p foregroundWindow=%p exact=%d sameProcessUsable=%d gameSize=%ldx%ld "
                "foregroundSize=%ldx%ld)",
                gameWindow, foregroundWindow, exactWindowForeground ? 1 : 0, usableSameProcessForegroundWindow ? 1 : 0,
                width, height, foregroundWidth, foregroundHeight);
            s_loggedUnusableResumeGameWindow = gameWindow;
            s_loggedUnusableResumeForegroundWindow = foregroundWindow;
        }
        s_resumeStableSinceMs = 0;
        s_resumeWindow = gameWindow;
        return true;
    }

    s_loggedUnusableResumeGameWindow = nullptr;
    s_loggedUnusableResumeForegroundWindow = nullptr;


    // GTA can switch from the swapchain HWND to another same-process foreground
    // window while the Social Club startup path unwinds. Track either stable
    // candidate so the post-resume delay can count down instead of latching at
    // remaining=0ms forever when the exact swapchain window no longer owns the
    // foreground.
    const bool windowForeground = true;
    const HWND stableWindow = usingSameProcessForegroundWindow ? foregroundWindow : gameWindow;
    if (usingSameProcessForegroundWindow && s_loggedSameProcessResumeWindow != stableWindow) {
        HookLogImportant(
            "DX12: Startup-overlay resume tracking usable same-process foreground window %p instead of "
            "swapchain window %p (foregroundSize=%ldx%ld)",
            stableWindow, gameWindow, width, height);
        s_loggedSameProcessResumeWindow = stableWindow;
    } else if (!usingSameProcessForegroundWindow) {
        s_loggedSameProcessResumeWindow = nullptr;
        if (s_loggedUnusableResumeGameWindow != gameWindow) {
            HookLogImportant(
                "DX12: Startup-overlay resume falling back to swapchain window %p (size=%ldx%ld) because "
                "foreground window %p is not a usable same-process window",
                gameWindow, width, height, foregroundWindow);
            s_loggedUnusableResumeGameWindow = gameWindow;
        }
    }

    if (s_resumeWindow != stableWindow || s_resumeStableSinceMs == 0) {
        s_resumeWindow = stableWindow;
        s_resumeStableSinceMs = now;
    }

    const ULONGLONG msSinceResumeReady = now - s_resumeStableSinceMs;
    if (ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(
            processNeedsDelay, s_hadStartupSuppression, actualFGActive, runtimeOwnedSwapchainActive, windowForeground,
            width, height, msSinceResumeReady, dx12_hook_kStartupOverlayPostResumeSettleMs)) {
        if (remainingMs) {
            *remainingMs =
                dx12_hook_kStartupOverlayPostResumeSettleMs - std::min(msSinceResumeReady, dx12_hook_kStartupOverlayPostResumeSettleMs);
        }
        return true;
    }

    s_hadStartupSuppression = false;
    s_resumeStableSinceMs = 0;
    s_resumeWindow = nullptr;
    return false;
}

inline bool ApplyOverlayStartupCompatMode(HWND gameWindow) {
    const char* overlayModule = nullptr;
    ULONGLONG remainingMs = 0;
    ce::overlay_compat::AuxiliaryProcessWindowInfo activeWindow = {};
    const bool suppressOverlay =
        ShouldSuppressOverlayForStartupCompat(gameWindow, &overlayModule, &remainingMs, &activeWindow);
    const bool allowOverlay = !suppressOverlay;
    static bool s_overlayCompatSuppressed = false;
    static bool s_loggedVisibleWindowSuppression = false;
    static bool s_loggedKeepVisibleDuringSuppression = false;
    static HWND s_loggedWindowHandle = nullptr;

    if (!allowOverlay) {
        if (ce::overlay_compat::ShouldKeepDX12OverlayVisibleDuringStartupSuppression(dx12_hook_g_State.overlayInit &&
                                                                                     dx12_hook_g_State.syncInit)) {
            if (!s_loggedKeepVisibleDuringSuppression) {
                HookLogImportant(
                    "DX12: Continuing DX12 overlay submissions while startup-overlay compatibility window is active "
                    "(overlay=%s, backend already initialized)",
                    overlayModule ? overlayModule : "module");
                s_loggedKeepVisibleDuringSuppression = true;
            }
            return true;
        }
        if (activeWindow.hwnd) {
            if (!s_overlayCompatSuppressed || !s_loggedVisibleWindowSuppression ||
                s_loggedWindowHandle != activeWindow.hwnd) {
                HookLogImportant(
                    "DX12: Pausing DX12 overlay submissions while startup window from %s is visible "
                    "(hwnd=%p visible=%d class='%s' title='%s')",
                    overlayModule ? overlayModule : "module", activeWindow.hwnd, activeWindow.visible ? 1 : 0,
                    activeWindow.className[0] ? activeWindow.className : "<unknown>",
                    activeWindow.title[0] ? activeWindow.title : "<untitled>");
                s_loggedVisibleWindowSuppression = true;
                s_loggedWindowHandle = activeWindow.hwnd;
            }
        } else if (!s_overlayCompatSuppressed || s_loggedVisibleWindowSuppression) {
            HookLogImportant(
                "DX12: Keeping DX12 overlay submissions paused for startup-overlay warm-up/cool-down "
                "(overlay=%s remaining=%llums)",
                overlayModule ? overlayModule : "module", remainingMs);
            s_loggedVisibleWindowSuppression = false;
            s_loggedWindowHandle = nullptr;
        }
        if (!s_overlayCompatSuppressed) {
            s_overlayCompatSuppressed = true;
        }
        return false;
    }

    if (s_overlayCompatSuppressed) {
        HookLogImportant("DX12: Resuming DX12 overlay after startup overlay windows settled");
        s_overlayCompatSuppressed = false;
        s_loggedVisibleWindowSuppression = false;
        s_loggedKeepVisibleDuringSuppression = false;
        s_loggedWindowHandle = nullptr;
    } else if (s_loggedKeepVisibleDuringSuppression) {
        s_loggedKeepVisibleDuringSuppression = false;
    }

    return true;
}

inline void DisableDedicatedOverlayQueueForOverlayCompat() {
    // When FG goes inactive, we keep the dedicated overlay queue alive to avoid
    // a destructive teardown/rebuild cycle during FG mode switches (e.g. 2x→3x).
    // Destroying and recreating queue + fence + allocators mid-transition causes
    // ERR_GFX_STATE because InitOverlaySync releases D3D12 objects while the GPU
    // still has in-flight work (deferred Signal not yet flushed).
    //
    // The queue sits idle when FG is inactive (submissions go to the game queue).
    // When FG reactivates, the queue is ready — no reinit needed.
    if (ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!dx12_hook_g_State.overlayQueue) {
        return;
    }

    static bool s_loggedSuspend = false;
    if (!s_loggedSuspend) {
        const char* overlayModule = nullptr;
        ShouldUseDedicatedOverlayQueue(&overlayModule);
        if (overlayModule) {
            HookLogImportant(
                "DX12: Suspending dedicated overlay queue (FG inactive, external overlay %s) — queue kept alive",
                overlayModule);
        } else {
            HookLogImportant("DX12: Suspending dedicated overlay queue (FG inactive) — queue kept alive");
        }
        s_loggedSuspend = true;
    }
}

inline void EnsureDedicatedOverlayQueueForFGCompat() {
    if (!ShouldUseDedicatedOverlayQueue()) {
        return;
    }

    if (!dx12_hook_g_State.syncInit || dx12_hook_g_State.overlayQueue) {
        // Queue already exists or not yet initialized — nothing to do.
        return;
    }

    // Non-SL FG cases (e.g., FSR FG with third-party overlay) may still need
    // a dedicated queue.  For SL FG, ShouldUseDedicatedOverlayQueue() returns
    // false so we never reach here; overlay draws are skipped instead.
    HookLogImportant(
        "DX12: FG active with overlay compat — dedicated overlay queue not yet created, forcing sync reinit");
    dx12_hook_g_State.syncInit = false;
    dx12_hook_g_State.syncDevice = nullptr;
    dx12_hook_g_State.overlayInit = false;
}

inline ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
    if (!queue)
        return oExecuteCommandLists;

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl)
        return oExecuteCommandLists;

    void** cachedVtable = dx12_hook_g_LastExecuteCommandListsVTable.load(std::memory_order_acquire);
    if (cachedVtable == vtbl) {
        ExecuteCommandListsPtr cachedOriginal = dx12_hook_g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire);
        if (cachedOriginal)
            return cachedOriginal;
    }

    ExecuteCommandListsPtr original = oExecuteCommandLists;
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
        auto it = dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl);
        if (it != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end())
            original = it->second;
    }

    if (original) {
        dx12_hook_g_LastExecuteCommandListsOriginal.store(original, std::memory_order_release);
        dx12_hook_g_LastExecuteCommandListsVTable.store(vtbl, std::memory_order_release);
    }
    return original;
}

inline bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue) {
    if (!queue) {
        return false;
    }

    void** vtbl = *reinterpret_cast<void***>(queue);
    if (!vtbl) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
    return dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl) != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end();
}

inline bool HookHasSafePostFSRBootstrapPathImpl() {
    if (!dx12_hook_g_HadFSRFGPhase) {
        return false;
    }

    const bool hasRealQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire) != nullptr;
    const bool hasRealD3D12ECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
    const bool hasSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire) != nullptr;
    const bool wrapperBootstrapSafe = !ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
        dx12_hook_g_HadFSRFGPhase, hasRealQueueBehindWrapper, hasRealD3D12ECL, hasSLWrapperQueue);
    if (wrapperBootstrapSafe) {
        return true;
    }

    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12CommandQueue* originalGameQueue = nullptr;
    bool hasTrackedSwapchainQueueSubmitPath = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = dx12_hook_g_SwapchainQueue;
        commandQueue = g_CommandQueue.load(std::memory_order_acquire);
        originalGameQueue = dx12_hook_g_OriginalGameQueue;
        hasTrackedSwapchainQueueSubmitPath = HasTrackedExecuteCommandListsOriginal(swapchainQueue);
    }
    const bool hasRuntimeOwnedSwapchainQueue = swapchainQueue != nullptr && swapchainQueue != originalGameQueue;
    const bool hasRealD3D12SubmitPath = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
    const bool hasSwapchainQueueSubmitPath = hasTrackedSwapchainQueueSubmitPath || hasRealD3D12SubmitPath;
    const bool commandQueueMatchesSwapchainQueue =
        commandQueue != nullptr && swapchainQueue != nullptr && commandQueue == swapchainQueue;
    const bool streamlineHandoffOrActive = DXGIShared::IsStreamlineStartupHandoffPending() ||
                                           DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool runtimeOwnedSwapchainBootstrapSafe =
        ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(
            dx12_hook_g_HadFSRFGPhase, hasRuntimeOwnedSwapchainQueue, streamlineHandoffOrActive, hasSwapchainQueueSubmitPath);
    if (runtimeOwnedSwapchainBootstrapSafe &&
        !dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Safe post-FSR bootstrap path available via runtime-owned Streamline swapchain queue "
            "(scQueue=%p cmdQ=%p origGame=%p realECL=%p wrapper=%p realBehindWrapper=%p trackedSubmit=%d "
            "cmdMatches=%d streamlineHandoffOrActive=%d)",
            swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
            dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire),
            dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire), hasTrackedSwapchainQueueSubmitPath ? 1 : 0,
            commandQueueMatchesSwapchainQueue ? 1 : 0, streamlineHandoffOrActive ? 1 : 0);
    } else if (hasRuntimeOwnedSwapchainQueue && streamlineHandoffOrActive && !runtimeOwnedSwapchainBootstrapSafe) {
        static std::atomic<int> s_runtimeOwnedBootstrapUnsafeLogCount{0};
        const int logCount = s_runtimeOwnedBootstrapUnsafeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 120) == 0) {
            HookLogImportant(
                "DX12: Runtime-owned Streamline swapchain queue not yet safe for post-FSR bootstrap "
                "(scQueue=%p cmdQ=%p origGame=%p realECL=%p trackedSubmit=%d cmdMatches=%d "
                "streamlineHandoffOrActive=%d log=%d)",
                swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
                hasTrackedSwapchainQueueSubmitPath ? 1 : 0, commandQueueMatchesSwapchainQueue ? 1 : 0,
                streamlineHandoffOrActive ? 1 : 0, logCount + 1);
        }
    }
    return runtimeOwnedSwapchainBootstrapSafe;
}

// Device-removed flag: once set, skip overlay rendering AND heartbeats so the
// freeze watchdog can detect the stuck state and create a diagnostic dump.
inline std::atomic<bool> dx12_hook_g_DeviceRemoved{false};

inline bool ShouldReserveInactiveFGOverlaySpaceNow() {
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
    }

    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool postSLRecentTeardownActivity =
        GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    return ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(
        postFSRNonFGRecovery, recentStreamlineTeardown, postSLRecentTeardownActivity);
}

inline ID3D12CommandQueue* GetFrameClassificationQueue() {
    ID3D12CommandQueue* primaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* originalQueue = dx12_hook_g_OriginalGameQueue;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    bool actualFGActive = false;
    bool streamlineFGRunning = false;
    bool recoveringPostFSRNonFG = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = dx12_hook_g_SwapchainQueue;
        actualFGActive = IsActualFrameGenerationActive();
        streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        recoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
            swapchainQueue != nullptr);
    }

    if (ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
            recoveringPostFSRNonFG, actualFGActive, streamlineFGRunning, swapchainQueue != nullptr,
            originalQueue != nullptr, primaryQueue != nullptr, originalQueue == primaryQueue)) {
        static std::atomic<int> s_postFSRClassificationPrimaryLogCount{0};
        int logCount = s_postFSRClassificationPrimaryLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Frame classification using primary queue %p during post-FSR non-FG recovery "
                "(origGame=%p scQ=%p lastWorking=%p offscreen=%d)",
                primaryQueue, originalQueue, swapchainQueue, dx12_hook_g_PostSLLastWorkingQueue,
                dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0);
        }
        return primaryQueue;
    }

    if (originalQueue) {
        return originalQueue;
    }

    return primaryQueue;
}

inline bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx) {
    if (!sc3 || !g_IPC || !g_IPC->IsCaptureRequested()) {
        return false;
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!shm) {
        return false;
    }

    const int captureFps = shm->fpsLimiter.GetCaptureFps();
    if (captureFps <= 0) {
        return false;
    }

    const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(captureFps);
    const int64_t suppressWindowUs = std::clamp((targetIntervalUs * 3) / 4, 1500LL, 7000LL);
    const int64_t nowUs = PerfLogger::GetQpcUs();
    IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(sc3);

    static std::atomic<IDXGISwapChain*> s_lastAcceptedSwapchain{nullptr};
    static std::atomic<uint32_t> s_lastAcceptedBackBufferIdx{UINT32_MAX};
    static std::atomic<int64_t> s_lastAcceptedPresentUs{0};
    static std::atomic<uint64_t> s_suppressedPresentCount{0};

    IDXGISwapChain* lastSwapchain = s_lastAcceptedSwapchain.load(std::memory_order_acquire);
    uint32_t lastBackBufferIdx = s_lastAcceptedBackBufferIdx.load(std::memory_order_acquire);
    int64_t lastAcceptedPresentUs = s_lastAcceptedPresentUs.load(std::memory_order_acquire);
    int64_t sinceLastUs = nowUs - lastAcceptedPresentUs;

    if (lastSwapchain == swapchain && lastBackBufferIdx == backBufferIdx && lastAcceptedPresentUs != 0 &&
        sinceLastUs > 0 && sinceLastUs < suppressWindowUs) {
        uint64_t suppressCount = s_suppressedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressCount <= 10 || (suppressCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Suppressing likely duplicate top-level Present #%llu "
                "(sc=%p bb=%u since=%lldus window=%lldus captureFps=%d)",
                static_cast<unsigned long long>(suppressCount), swapchain, backBufferIdx,
                static_cast<long long>(sinceLastUs), static_cast<long long>(suppressWindowUs), captureFps);
        }
        return true;
    }

    s_lastAcceptedSwapchain.store(swapchain, std::memory_order_release);
    s_lastAcceptedBackBufferIdx.store(backBufferIdx, std::memory_order_release);
    s_lastAcceptedPresentUs.store(nowUs, std::memory_order_release);
    return false;
}

inline bool ShouldSkipCaptureForTargetCadence() {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    return ShouldSkipCaptureForTargetCadence(shm, "DX12");
}

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
inline __attribute__((noinline)) void DX12_SetCommandQueueInternal(ID3D12CommandQueue* pQueue,
                                                                   bool callerFromThirdPartyOverlay,
                                                                   const char* callerModulePath) {
    if (!pQueue)
        return;

    // Safety: during FG transitions, SL may call ECL on a queue that's
    // concurrently being freed.  Freed COM objects have null vtable pointers.
    // Use volatile to prevent compiler from caching the vtable across calls.
    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr)
        return;

    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        static std::atomic<int> s_protectedOfficialFFXSetQueueSkipLogCount{0};
        const int logCount = s_protectedOfficialFFXSetQueueSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - skipping SetCommandQueue side effects "
                "(queue=%p callerOverlay=%d caller=%s count=%d)",
                pQueue, callerFromThirdPartyOverlay ? 1 : 0,
                callerModulePath && callerModulePath[0] ? callerModulePath : "unknown", logCount + 1);
        }
        return;
    }

    // ExecuteCommandLists may hit this many times per frame on the same queue.
    // Once we've captured the active DIRECT queue, avoid the repeated GetDesc /
    // lock / QueryInterface work on the hot path.
    if (g_CommandQueue.load(std::memory_order_acquire) == pQueue)
        return;

    ID3D12CommandQueue* primaryQ = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
    }

    if (ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(
            callerFromThirdPartyOverlay, dx12_hook_g_OriginalGameQueue != nullptr, pQueue == primaryQ,
            pQueue == dx12_hook_g_OriginalGameQueue, pQueue == currentSwapchainQueue)) {
        static std::atomic<int> s_overlayQueueIgnoreLogCount{0};
        int logCount = s_overlayQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: SetCommandQueue ignoring foreign overlay queue %p from caller %s "
                "(primary=%p orig=%p scQ=%p current=%p)",
                pQueue, (callerModulePath && *callerModulePath) ? callerModulePath : "unknown", primaryQ,
                dx12_hook_g_OriginalGameQueue, currentSwapchainQueue, g_CommandQueue.load(std::memory_order_acquire));
        }
        return;
    }

    const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool lastWorkingQueueStillActiveDuringRecentTeardown =
        dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
            recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
            pQueue == primaryQ, pQueue == dx12_hook_g_OriginalGameQueue, pQueue == currentSwapchainQueue,
            pQueue == dx12_hook_g_PostSLLastWorkingQueue)) {
        if (ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(
                recentStreamlineTeardown, dx12_hook_g_PostSLLastWorkingQueue && pQueue == dx12_hook_g_PostSLLastWorkingQueue,
                streamlineFGRunning, postSLActive)) {
            MarkPostSLRecentTeardownActivity("DX12: SetCommandQueue recent PostSL teardown activity", pQueue);
        }
        static std::atomic<int> s_recentSLTeardownSetQueueIgnoreLogCount{0};
        int logCount = s_recentSLTeardownSetQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 256) == 0) {
            HookLogImportant(
                "DX12: SetCommandQueue ignoring departed queue %p during Streamline teardown / post-FSR recovery "
                "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d postSLRecent=%d postFSR=%d)",
                pQueue, primaryQ, dx12_hook_g_OriginalGameQueue, currentSwapchainQueue,
                g_CommandQueue.load(std::memory_order_acquire), dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire),
                lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0);
        }
        return;
    }

    // CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
    // Strange Brigade and other DX12 games use Async Compute queues.
    // Submitting overlay (Direct) commands to a Compute queue causes a device
    // lost/crash.
    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
        return;
    }

    // Set primary game queue once — the first DIRECT queue seen is always the
    // game's queue (created before any FG runtime initializes).  Used to filter
    // ECL counting for accurate real-vs-interpolated frame classification.
    ID3D12CommandQueue* expected = nullptr;
    dx12_hook_g_PrimaryGameQueue.compare_exchange_strong(expected, pQueue, std::memory_order_acq_rel);

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (g_CommandQueue.load() != pQueue) {
        if (g_CommandQueue.load())
            g_CommandQueue.load()->Release();
        g_CommandQueue.store(pQueue);
        pQueue->AddRef();

        // Re-check vtable before GetDevice — another thread may have freed
        // the queue between GetDesc and here.  Volatile prevents caching.
        auto vtblRecheck = *reinterpret_cast<void* volatile const*>(pQueue);
        if (!vtblRecheck) {
            HookLogImportant("DX12: SetCommandQueue — queue %p freed during registration (vtable null after store)",
                             pQueue);
            return;
        }

        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
            DX12_PublishNativeLimiterDevice(dev, pQueue, "command queue");
            if (g_Device.load() != dev) {
                if (g_Device.load())
                    g_Device.load()->Release();
                g_Device.store(dev);

                // Clear device-removed flag — a new device means recovery.
                dx12_hook_g_DeviceRemoved.store(false, std::memory_order_release);
                DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                g_RenderWatchdog.SetForceMonitor(false);

                // Reset primary game queue — new device means new queues.
                dx12_hook_g_PrimaryGameQueue.store(pQueue, std::memory_order_release);
                dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);

                // Report GPU LUID for host metrics (PDH counter filtering).
                // ID3D12Device has GetAdapterLuid() directly — don't use
                // IDXGIDevice (D3D12 devices don't implement it).
                LUID adapterLuid = dev->GetAdapterLuid();
                ReportLUID(adapterLuid.LowPart, adapterLuid.HighPart);
                HookLog("DX12: Reported LUID %08x-%08x", adapterLuid.HighPart, adapterLuid.LowPart);
            } else
                dev->Release();
        }
    }

    // CRITICAL FIX: Hook queue vtable lazily here instead of during swapchain
    // creation This prevents hangs during DXGI internal operations
    DX12_HookQueueVTable(pQueue);
}

// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
inline bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue,
                                   bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain,
                                   IDXGISwapChain* associatedSwapchain, bool authoritativeNormalSwapchainReturn) {
    if (!pQueue)
        return false;

    // Safety: freed COM objects have null vtable — skip
    void** vtblCheck = *reinterpret_cast<void***>(pQueue);
    if (!vtblCheck)
        return false;

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return false;

    bool runtimeOwnershipJustActivated = false;

    // Diagnostic: log the queue's device to detect cross-device issues
    ID3D12Device* queueDev = nullptr;
    if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&queueDev)))) {
        auto* curDev = g_Device.load(std::memory_order_acquire);
        if (queueDev != curDev) {
            HookLogImportant("DX12: SetSwapchainQueue — queue %p device %p DIFFERS from g_Device %p", pQueue, queueDev,
                             curDev);
        }
        DX12_PublishNativeLimiterDevice(queueDev, pQueue, "swapchain queue");
        queueDev->Release();
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(associatedSwapchain, std::memory_order_release);
    if (associatedSwapchain && dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue) {
        IDXGISwapChain* expectedOriginalSwapchain = associatedSwapchain;
        if (dx12_hook_g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
                expectedOriginalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
            HookLogImportant(
                "DX12: Non-original queue association superseded remembered native ownership for swapchain %p "
                "(queue=%p origGame=%p)",
                associatedSwapchain, pQueue, dx12_hook_g_OriginalGameQueue);
        }
    }
    if (dx12_hook_g_SwapchainQueue != pQueue) {
        if (dx12_hook_g_SwapchainQueue)
            dx12_hook_g_SwapchainQueue->Release();
        dx12_hook_g_SwapchainQueue = pQueue;
        dx12_hook_g_SwapchainQueue->AddRef();
        dx12_hook_g_SwapchainQueueCaptureTime = GetTickCount64();


        // Track whether an FG runtime owns this swapchain/queue
        bool runtimeOwns = (dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue);

        if (authoritativeNormalSwapchainReturn) {
            runtimeOwns = false;
            if (dx12_hook_g_OriginalGameQueue != pQueue) {
                ID3D12CommandQueue* oldOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                dx12_hook_g_OriginalGameQueue = pQueue;
                dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
                pQueue->AddRef();
                HookLogImportant(
                    "DX12: Re-baselined original game queue to authoritative normal-return queue %p "
                    "(was %p; the game replaced the retired Streamline presentation topology)",
                    pQueue, oldOriginalGameQueue);
                if (oldOriginalGameQueue) {
                    oldOriginalGameQueue->Release();
                }
            }
            const bool ownershipWasHeld = dx12_hook_g_FGRuntimeOwnsSwapchain;
            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
            HookLogImportant(
                "DX12: Authoritative normal swapchain return ended retired Streamline queue ownership "
                "(queue=%p origGame=%p ownershipWasHeld=%d)",
                pQueue, dx12_hook_g_OriginalGameQueue, ownershipWasHeld ? 1 : 0);
        }

        // A GAME-created swapchain (caller is neither an FG runtime nor a
        // third-party overlay) arriving while explicit native-FSR OFF/destroy
        // evidence is pending is the stronger off signal the runtime-owned
        // teardown was waiting for. Games that recreate their swapchain on a
        // FRESH queue never satisfy the origGame-return teardown end below and
        // would otherwise stay misclassified as runtime-owned, blanking the
        // overlay through FG cooldowns and re-latching FSR heuristics on a
        // plain game queue (20260611_191950 FSR->OFF).
        const bool endNativeFGTeardownOnGameSwapchainCreation =
            ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(
                gameCreatedSwapchain, dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.load(std::memory_order_acquire),
                dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
        if (endNativeFGTeardownOnGameSwapchainCreation) {
            runtimeOwns = false;
            dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
            dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(pQueue, std::memory_order_release);
            // The game retired its previous present queue and created this
            // swapchain on a fresh one with game provenance. Re-baseline the
            // original-game-queue anchor so frame classification counts the
            // game's real ECLs again (a stale dead anchor classifies every
            // present as zero-ECL/interpolated and starves ProcessFrame —
            // 20260612_000936: overlay disappeared forever after FSR->off),
            // and so future FG cycles' takeover/teardown proofs compare
            // against the queue that actually presents. Games that recreate
            // on the SAME queue (Talos-style) hit the pointer-equality no-op.
            if (dx12_hook_g_OriginalGameQueue != pQueue) {
                ID3D12CommandQueue* oldOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                dx12_hook_g_OriginalGameQueue = pQueue;
                dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
                pQueue->AddRef();
                HookLogImportant(
                    "DX12: Re-baselined original game queue to game-created recovery queue %p "
                    "(was %p; old queue retired by the game itself)",
                    pQueue, oldOriginalGameQueue);
                if (oldOriginalGameQueue) {
                    oldOriginalGameQueue->Release();
                }
            }
            const bool ownershipWasHeld = dx12_hook_g_FGRuntimeOwnsSwapchain;
            if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            }
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
            ForceClearNativeFSRInternalNoCallbackComposition(
                "game-created swapchain after explicit native FSR OFF/destroy");
            g_FGCompat.SetHeuristicFSRFGActive(false);
            RequestFGDetectionHeuristicReset();
            ResetAuthoritativeFSRRealFrameOnlyStreak();
            SetNativeFSRStartupConfigureArmingPending(false,
                                                      "game-created swapchain after explicit native FSR OFF/destroy");
            ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
                "game-created swapchain after explicit native FSR OFF/destroy");
            if (g_FGCompat.IsFSRFGApiActive()) {
                g_FGCompat.SetFSRFGActive(false);
                g_FGCompat.SetFSRFGMultiplier(0);
            }
            HookLogImportant(
                "DX12: Game-created swapchain after explicit native FSR OFF/destroy — ending runtime-owned "
                "native-FG teardown so the overlay resumes without FG cooldowns (queue=%p origGame=%p "
                "ownershipWasHeld=%d caller=%s)",
                pQueue, dx12_hook_g_OriginalGameQueue, ownershipWasHeld ? 1 : 0, "game");
        }

        if (runtimeOwns && !dx12_hook_g_FGRuntimeOwnsSwapchain) {
            dx12_hook_g_FGRuntimeOwnsSwapchain = true;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(true, std::memory_order_release);
            dx12_hook_g_FGRuntimeOwnsSwapchainSince = GetTickCount64();
            runtimeOwnershipJustActivated = true;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(true, std::memory_order_release);
            HookLogImportant(
                "DX12: FG runtime now owns swapchain queue %p (origGame=%p) — dedicated/cross-queue overlay work is "
                "disabled on this queue",
                pQueue, dx12_hook_g_OriginalGameQueue);
        } else if (!runtimeOwns && dx12_hook_g_FGRuntimeOwnsSwapchain) {
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
            const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
            const bool explicitNativeFSROffPending =
                dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
            const bool ffxPresentCallbackStalled = IsFFXPresentCallbackStalled();
            // Overlay fallback permission is a rendering transport decision, not
            // an ownership teardown signal.  Keep preserving active native FSR
            // ownership until an explicit OFF/device/swapchain transition proves
            // the runtime has really left the FG path.
            const bool preserveAuthoritativeFSRBase =
                ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
                    true, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), runtimeMode,
                    authoritativeFSRActive, runtimeOwnedNativeFGPresentPath, false);
            const bool endNativeFGTeardownOnOrigGame =
                ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
                    pQueue == dx12_hook_g_OriginalGameQueue, explicitNativeFSROffPending, authoritativeFSRActive, runtimeMode,
                    runtimeOwnedNativeFGPresentPath);
            const bool preserveAuthoritativeFSR = preserveAuthoritativeFSRBase && !endNativeFGTeardownOnOrigGame;
            if (preserveAuthoritativeFSR) {
                HookLogImportant(
                    "DX12: Swapchain queue returned to origGame %p while authoritative/runtime-owned FSR state is "
                    "still active (runtime=%s explicitNativeOff=%d nativeFGPath=%d stalled=%d) — preserving FG "
                    "runtime ownership until a stronger off signal arrives",
                    pQueue, ce::fg_runtime::GetRuntimeModeName(runtimeMode), explicitNativeFSROffPending ? 1 : 0,
                    runtimeOwnedNativeFGPresentPath ? 1 : 0, ffxPresentCallbackStalled ? 1 : 0);
                HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                                 dx12_hook_g_OriginalGameQueue, (pQueue == dx12_hook_g_OriginalGameQueue) ? 1 : 0,
                                 dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
                return runtimeOwnershipJustActivated;
            }

            if (endNativeFGTeardownOnOrigGame) {
                HookLogImportant(
                    "DX12: Explicit native FSR OFF plus origGame swapchain return ending runtime-owned native-FG "
                    "teardown (queue=%p origGame=%p runtime=%s nativeFGPath=%d callbackStalled=%d)",
                    pQueue, dx12_hook_g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                    runtimeOwnedNativeFGPresentPath ? 1 : 0, ffxPresentCallbackStalled ? 1 : 0);
                g_FGCompat.SetHeuristicFSRFGActive(false);
                ResetAuthoritativeFSRRealFrameOnlyStreak();
                ForceClearNativeFSRInternalNoCallbackComposition(
                    "explicit native FSR OFF plus origGame swapchain return");
            }

            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
            if (g_FGCompat.IsFSRFGApiActive()) {
                HookLogImportant("DX12: Swapchain returned to origGame queue %p — ending authoritative FSR FG state",
                                 pQueue);
                SetNativeFSRStartupConfigureArmingPending(false, "swapchain returned to origGame");
                ClearOfficialFFXRuntimeOwnedPresentPathAssumption("swapchain returned to origGame");
                g_FGCompat.SetFSRFGActive(false);
                g_FGCompat.SetFSRFGMultiplier(0);
                ResetAuthoritativeFSRRealFrameOnlyStreak();
            }
            HookLogImportant("DX12: Swapchain returned to origGame queue %p — FG runtime ownership cleared", pQueue);
        }

        // If the swapchain queue changed and FSR is no longer active, the old explicit
        // native-FSR OFF teardown flag is stale — it referred to the previous queue's
        // runtime-owned Present path, not this new one.  Keeping it true defers overlay
        // init indefinitely when the game creates a new menu swapchain after FSR OFF.
        if (dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) &&
            !g_FGCompat.IsFSRFGApiActive() && g_FGCompat.GetRuntimeMode() != ce::fg_runtime::RuntimeMode::kFSRFG) {
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            HookLogImportant(
                "DX12: Swapchain queue changed to %p while FSR is no longer active — cleared stale explicit native FSR "
                "OFF teardown flag (origGame=%p runtime=%s)",
                pQueue, dx12_hook_g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()));
        }

        HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                         dx12_hook_g_OriginalGameQueue, (pQueue == dx12_hook_g_OriginalGameQueue) ? 1 : 0,
                         dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
    }

    // Only hook the vtable if this is the game's original queue (or we haven't
    // captured origGame yet).  FG runtimes (FSR FG) create their own queues and
    // rely on tight ECL timing.  Hooking their vtable adds overhead to every ECL
    // call (safety checks, heartbeat, queue tracking, lock acquisition, etc.).
    // This cumulative overhead breaks FSR FG's internal fence synchronization,
    // causing ffxQuery to spin-wait or WaitForSingleObject indefinitely.
    // We already hook origGame's queue for watchdog/heartbeat — that's sufficient.
    const bool shouldHookQueueVTable = ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(
        dx12_hook_g_OriginalGameQueue != nullptr, pQueue == dx12_hook_g_OriginalGameQueue, authoritativeStreamlineRuntimeQueue,
        authoritativeFFXRuntimeQueue);
    if (shouldHookQueueVTable) {
        if (authoritativeStreamlineRuntimeQueue && dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue) {
            HookLogImportant(
                "DX12: Hooking authoritative Streamline runtime queue vtable %p (origGame=%p) to keep runtime-owned "
                "ECL tracking visible",
                pQueue, dx12_hook_g_OriginalGameQueue);
        }
        DX12_HookQueueVTable(pQueue);
    } else {
        if (authoritativeFFXRuntimeQueue && authoritativeStreamlineRuntimeQueue && dx12_hook_g_OriginalGameQueue &&
            pQueue != dx12_hook_g_OriginalGameQueue) {
            HookLogImportant(
                "DX12: Skipping vtable hook for FFX-owned runtime queue %p despite stale Streamline provenance "
                "(origGame=%p) — preserving FSR timing",
                pQueue, dx12_hook_g_OriginalGameQueue);
        } else {
            HookLogImportant("DX12: Skipping vtable hook for FG runtime queue %p (origGame=%p) — preserving FSR timing",
                             pQueue, dx12_hook_g_OriginalGameQueue);
        }
    }

    return runtimeOwnershipJustActivated;
}

inline bool IsDX12Swapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    ID3D12Device* pDX12Device = nullptr;
    HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDX12Device));
    if (FAILED(hr) || !pDX12Device)
        return false;

    pDX12Device->Release();
    return true;
}

inline bool InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(const char* context,
                                                                        ID3D12CommandQueue* newSwapchainQueue,
                                                                        ID3D12CommandQueue* previousSwapchainQueue,
                                                                        ID3D12CommandQueue* originalGameQueue) {
    ID3D12CommandQueue* lockedQueue = nullptr;
    ID3D12CommandQueue* lastWorkingQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        lockedQueue = dx12_hook_g_PostSLLockedQueue;
        lastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
    }

    const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool newQueueMatchesPreviousSwapchainQueue =
        newSwapchainQueue != nullptr && newSwapchainQueue == previousSwapchainQueue;
    const bool invalidateConfirmed =
        ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
            true, postSLConfirmedRendering, newQueueMatchesPreviousSwapchainQueue);
    const bool clearLastWorking =
        ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(
            true, lastWorkingQueue != nullptr, newSwapchainQueue != nullptr && lastWorkingQueue == newSwapchainQueue);
    const bool clearLocked = ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(
        true, lockedQueue != nullptr, newSwapchainQueue != nullptr && lockedQueue == newSwapchainQueue);

    DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);

    if (invalidateConfirmed) {
        const int previousStableFrames = dx12_hook_g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
        dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
        dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
        dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        HookLogImportant(
            "DX12: Fresh authoritative Streamline handoff invalidated stale PostSL confirmation "
            "(source=%s newScQueue=%p prevScQueue=%p origGame=%p locked=%p lastWorking=%p stableFrames=%d)",
            context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, originalGameQueue, lockedQueue,
            lastWorkingQueue, previousStableFrames);
    }

    if (invalidateConfirmed || clearLocked) {
        WaitForOverlayGpuIdle("DX12: Fresh authoritative Streamline handoff");
        ResetPostSLLifecycleForTransition("DX12: Fresh authoritative Streamline handoff", true);
    }

    if (clearLastWorking) {
        HookLogImportant(
            "DX12: Fresh authoritative Streamline handoff cleared stale PostSL lastWorking queue %p "
            "(newScQueue=%p prevScQueue=%p origGame=%p)",
            lastWorkingQueue, newSwapchainQueue, previousSwapchainQueue, originalGameQueue);
        SetPostSLLastWorkingQueue(nullptr);
    }

    bool overlayWasLive = false;
    bool overlaySwapchainStateRetired = false;
    if (dx12_hook_g_State.overlayInit || dx12_hook_g_State.syncInit) {
        std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
        overlayWasLive = dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && g_OverlayAdapter.IsInitialized();
        const bool preserveLiveOverlayDuringHandoff =
            ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
                dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit,
                dx12_hook_g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
                dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_HadFSRFGPhase,
                dx12_hook_g_OriginalGameQueue != nullptr);
        if (preserveLiveOverlayDuringHandoff) {
            dx12_hook_g_State.cachedSwapChain = nullptr;
            dx12_hook_g_State.cachedSC3 = nullptr;
            HookLogImportant(
                "DX12: Fresh authoritative Streamline no-FG handoff preserved live overlay backend "
                "(source=%s newScQueue=%p prevScQueue=%p origGame=%p)",
                context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, originalGameQueue);
        } else {
            // The adapter is device/format scoped and does not retain swapchain buffers or submit through its
            // initialization queue. Keep it warm while retiring only the old swapchain-scoped RTV/sync state.
            // The fresh post-FSR Streamline handoff can then prewarm the replacement state before DLSS is enabled,
            // rather than rebuilding the backend inside the first generated Present.
            dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            dx12_hook_g_State.overlayInit = false;
            dx12_hook_g_State.syncInit = false;
            dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
            CleanupRTVs();
            overlaySwapchainStateRetired = true;
            HookLogImportant(
                "DX12: Fresh authoritative Streamline handoff invalidated PostSL swapchain resources while "
                "preserving the warm device-scoped backend (source=%s newScQueue=%p prevScQueue=%p live=%d)",
                context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, overlayWasLive ? 1 : 0);
        }
    }
    return overlayWasLive && overlaySwapchainStateRetired;
}

inline void PublishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
    SetPostSLCallbackInstalled(false, reason);
    // Publish cancellation before waiting for an already-entered callback. The
    // callback compares this epoch before every GPU submission point, exits,
    // and releases the render mutex without a polling delay.
    dx12_hook_g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
}

inline int FinishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
    std::lock_guard<std::mutex> renderLock(dx12_hook_g_PostSLRenderMutex);

    const int previousStableFrames = dx12_hook_g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(false, std::memory_order_release);
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ResetPostSLLifecycleForTransition(reason, true);
    SetPostSLLastWorkingQueue(nullptr);
    ReleaseStreamlineStartupActivationSwapchain(reason);

    if (dx12_hook_g_State.overlayInit || dx12_hook_g_State.syncInit) {
        std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        dx12_hook_g_State.overlayInit = false;
        dx12_hook_g_State.syncInit = false;
        dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
        CleanupRTVs();
    }

    return previousStableFrames;
}

inline int RetirePostSLRouteForNormalSwapchainReturn(const char* reason) {
    PublishPostSLRouteRetirementForNormalSwapchainReturn(reason);
    return FinishPostSLRouteRetirementForNormalSwapchainReturn(reason);
}

inline bool HandlePostSLRouteForNormalSwapchainReturn(const char* context, ID3D12CommandQueue* returnedQueue,
                                                      IDXGISwapChain* returnedSwapchain,
                                                      ID3D12CommandQueue* originalGameQueue,
                                                      const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    const bool originalQueueNormalSwapchainReturn =
        ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
            captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.callerFromStreamlineFGModule,
            captureEvidence.streamlineFrameGenerationInStack,
            dx12_hook_g_StreamlineEnableCallsInFlight.load(std::memory_order_acquire) != 0, originalGameQueue != nullptr,
            returnedQueue == originalGameQueue);
    const bool gameCreatedSwapchain =
        !captureEvidence.callerFromThirdPartyOverlay && !captureEvidence.authoritativeFFXRuntimeCreator &&
        !captureEvidence.officialAMDFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator;
    const bool gameSwapchainAfterExplicitDLSSOff =
        ce::dx12_overlay_policy::ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
            gameCreatedSwapchain, dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
            IsActualFrameGenerationActive(), DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
    const bool normalSwapchainReturn = originalQueueNormalSwapchainReturn || gameSwapchainAfterExplicitDLSSOff;
    if (!normalSwapchainReturn) {
        return false;
    }

    if (gameSwapchainAfterExplicitDLSSOff && returnedSwapchain) {
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(returnedSwapchain, std::memory_order_release);
        HookLogImportant(
            "[OVERLAY VISIBILITY] Armed exact native swapchain takeover after authoritative DLSS OFF "
            "(swapchain=%p queue=%p)",
            returnedSwapchain, returnedQueue);
    } else {
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    }

    // A proven return is also an authoritative queue-topology boundary even
    // when no PostSL route happens to remain armed. Seed the queue heuristic
    // before the first Present so the departed Streamline queue can never be
    // mistaken for the baseline of a new FSR epoch.
    g_FGCompat.SetHeuristicFSRFGActive(false);
    RequestFGDetectionHeuristicReset(returnedQueue);

    ID3D12CommandQueue* lockedQueue = nullptr;
    ID3D12CommandQueue* lastWorkingQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        lockedQueue = dx12_hook_g_PostSLLockedQueue;
        lastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
    }
    const bool routeArmed = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                            dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                            dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
                            dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || lockedQueue != nullptr ||
                            lastWorkingQueue != nullptr;
    const bool hasDistinctQueueProof =
        (lockedQueue && lockedQueue != returnedQueue) || (lastWorkingQueue && lastWorkingQueue != returnedQueue);
    const char* normalReturnProof = gameSwapchainAfterExplicitDLSSOff
                                        ? "Game-created replacement swapchain validated normal return after "
                                          "explicit DLSS off"
                                        : "Original game queue validated normal swapchain return behind "
                                          "Streamline stack";
    if (!ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(
            normalSwapchainReturn, routeArmed, hasDistinctQueueProof || gameSwapchainAfterExplicitDLSSOff)) {
        HookLogImportant("%s: %s (queue=%p locked=%p lastWorking=%p routeArmed=%d) — no stale PostSL route to retire",
                         context ? context : "CreateSwapChain", normalReturnProof, returnedQueue, lockedQueue,
                         lastWorkingQueue, routeArmed ? 1 : 0);
        return true;
    }

    const int previousStableFrames = RetirePostSLRouteForNormalSwapchainReturn("DX12: normal swapchain return");

    HookLogImportant(
        "%s: %s — retired stale PostSL route and invalidated swapchain-scoped overlay state "
        "(queue=%p locked=%p lastWorking=%p stableFrames=%d caller=%s)",
        context ? context : "CreateSwapChain", normalReturnProof, returnedQueue, lockedQueue, lastWorkingQueue,
        previousStableFrames, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "unknown");
    return true;
}

inline void CaptureSwapchainQueueFromCreateDevice(IUnknown* pDevice, IDXGISwapChain* pSwapChain, const char* context,
                                                  const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    if (!pDevice || !pSwapChain)
        return;

    ID3D12CommandQueue* pQueue = nullptr;
    HRESULT qiHr = pDevice->QueryInterface(IID_PPV_ARGS(&pQueue));
    if (SUCCEEDED(qiHr) && pQueue) {
        ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
        ID3D12CommandQueue* currentSwapchainQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            currentOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
        }
        const bool preserveCurrentGameQueue =
            ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(
                captureEvidence.callerFromThirdPartyOverlay, currentOriginalGameQueue != nullptr,
                pQueue == currentOriginalGameQueue);
        const bool authoritativeFFXRuntimeQueue =
            ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(
                captureEvidence.authoritativeFFXRuntimeCreator, currentOriginalGameQueue != nullptr,
                pQueue == currentOriginalGameQueue);
        const bool authoritativeStreamlineRuntimeQueue =
            !authoritativeFFXRuntimeQueue &&
            ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(
                captureEvidence.authoritativeStreamlineRuntimeCreator, currentOriginalGameQueue != nullptr,
                pQueue == currentOriginalGameQueue);
        const bool freshAuthoritativeStreamlineHandoff =
            ce::dx12_overlay_policy::ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
                authoritativeStreamlineRuntimeQueue, pQueue == currentSwapchainQueue);
        const bool normalSwapchainReturn = HandlePostSLRouteForNormalSwapchainReturn(
            context, pQueue, pSwapChain, currentOriginalGameQueue, captureEvidence);

        HookLogImportant("%s: QI for queue succeeded (queue=%p)", context, pQueue);
        if (preserveCurrentGameQueue) {
            HookLogImportant(
                "%s: Ignoring foreign swapchain queue %p from third-party overlay caller %s "
                "(origGame=%p) — preserving live game queue ownership",
                context, pQueue, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "unknown",
                currentOriginalGameQueue);
            pQueue->Release();
            return;
        }
        if (authoritativeFFXRuntimeQueue && captureEvidence.authoritativeStreamlineRuntimeCreator) {
            static std::atomic<int> s_ffxOverridesStreamlineQueueAuthorityLogCount{0};
            const int logCount = s_ffxOverridesStreamlineQueueAuthorityLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20) {
                HookLogImportant(
                    "%s: Authoritative FFX ownership overrides stale Streamline runtime queue authority "
                    "(queue=%p caller=%s)",
                    context, pQueue, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
            }
        }
        // A caller that is neither an FG runtime, an FFX stack, nor a
        // third-party overlay is the game itself creating its swapchain. This
        // provenance is what lets explicit native-FSR OFF/destroy teardowns end
        // on game swapchain recreation instead of waiting for an origGame queue
        // return that fresh-queue games never deliver.
        const bool gameCreatedSwapchain =
            normalSwapchainReturn ||
            (!captureEvidence.callerFromThirdPartyOverlay && !captureEvidence.authoritativeFFXRuntimeCreator &&
             !captureEvidence.officialAMDFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator);
        // DX12_SetSwapchainQueue publishes the queue and this exact swapchain
        // under one lock boundary, so ProcessFrame cannot observe a mismatched
        // queue/identity pair at the transition edge.
        const bool runtimeOwnershipJustActivated =
            DX12_SetSwapchainQueue(pQueue, authoritativeStreamlineRuntimeQueue, authoritativeFFXRuntimeQueue,
                                   gameCreatedSwapchain, pSwapChain, normalSwapchainReturn);
        bool capturedOnOriginalQueue = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            capturedOnOriginalQueue = gameCreatedSwapchain && dx12_hook_g_OriginalGameQueue != nullptr &&
                                      pQueue == dx12_hook_g_OriginalGameQueue && dx12_hook_g_SwapchainQueue == pQueue &&
                                      (normalSwapchainReturn || !dx12_hook_g_FGRuntimeOwnsSwapchain);
            if (capturedOnOriginalQueue) {
                RememberOriginalQueueSwapchainIdentity(pSwapChain, "CreateSwapChain original-queue association");
            }
        }
        if (freshAuthoritativeStreamlineHandoff) {
            {
                std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
                    false, true, context ? context : "fresh authoritative Streamline handoff");
            }
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(
                    IsDX12Swapchain(pSwapChain), freshAuthoritativeStreamlineHandoff,
                    DXGIShared::DoesFGRuntimeOwnSwapchain())) {
                DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                                "DX12: fresh authoritative Streamline handoff");
            }
            const bool retiredLiveOverlayState = InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(
                context, pQueue, currentSwapchainQueue, currentOriginalGameQueue);
            const bool hadSuccessfulPostSLPhase = dx12_hook_g_HadSuccessfulPostSLPhase.load(std::memory_order_acquire);
            const bool prewarmPostSL = ce::dx12_overlay_policy::ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(
                freshAuthoritativeStreamlineHandoff, dx12_hook_g_HadFSRFGPhase, hadSuccessfulPostSLPhase,
                DXGIShared::DoesFGRuntimeOwnSwapchain(),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), retiredLiveOverlayState,
                IsDX12Swapchain(pSwapChain));
            dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
            if (prewarmPostSL) {
                const bool prewarmReady = PrewarmPostSLOverlayForFreshStreamlineHandoff(pSwapChain, pQueue, context);
                if (prewarmReady) {
                    dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(pSwapChain, std::memory_order_release);
                    HookLogImportant(
                        "[OVERLAY VISIBILITY] Armed exact prewarmed PostSL handoff backend for its first Present "
                        "(swapchain=%p queue=%p hadFSR=%d priorPostSL=%d)",
                        pSwapChain, pQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0, hadSuccessfulPostSLPhase ? 1 : 0);
                }
            }
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            StreamlineHook::OnAuthoritativeStreamlineStartupHandoff();
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                        context ? context : "DX12::AuthoritativeStreamlineStartupHandoff", pQueue,
                                        pSwapChain, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
            if (ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(
                    freshAuthoritativeStreamlineHandoff, dx12_hook_g_HadFSRFGPhase,
                    dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr)) {
                ID3D12Device* handoffDevice = nullptr;
                const HRESULT handoffDeviceHr = pQueue->GetDevice(IID_PPV_ARGS(&handoffDevice));
                if (SUCCEEDED(handoffDeviceHr) && handoffDevice) {
                    // Defer probe if the Streamline startup window is active — creating
                    // a temporary COMPUTE queue during SL's critical init can crash SL
                    // with a null pointer call (same as the other probe deferral sites).
                    if (!DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
                        ProbeRealD3D12ECL(handoffDevice);
                        HookLogImportant(
                            "%s: Re-probed real D3D12 ECL for fresh authoritative Streamline handoff after FSR "
                            "(queue=%p realECL=%p dev=%p)",
                            context, pQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire), handoffDevice);
                    } else {
                        dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                        HookLogImportant(
                            "%s: Deferred realECL reprobe for fresh authoritative Streamline handoff after FSR "
                            "(queue=%p dev=%p, startup window active)",
                            context, pQueue, handoffDevice);
                    }
                    handoffDevice->Release();
                } else {
                    HookLogImportant(
                        "%s: Failed to get handoff device for post-FSR realECL reprobe "
                        "(queue=%p hr=0x%08X)",
                        context, pQueue, (unsigned)handoffDeviceHr);
                }
            }
            HookLogImportant(
                "%s: Armed Streamline startup transition window after authoritative runtime-owned swapchain handoff "
                "(queue=%p prevScQueue=%p origGame=%p caller=%s)",
                context, pQueue, currentSwapchainQueue, currentOriginalGameQueue,
                captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
        }
        ClearStaleStreamlineOwnershipForFSRTakeover(
            captureEvidence, currentOriginalGameQueue != nullptr && pQueue != currentOriginalGameQueue,
            runtimeOwnershipJustActivated, pQueue);
        pQueue->Release();
        return;
    }

    // CreateSwapChainForHwnd is shared by DX10/11/12. Avoid treating arbitrary
    // DXGI callers as ID3D12CommandQueue objects when QI already proved they are not.
    if (IsDX12Swapchain(pSwapChain)) {
        HookLogImportant(
            "%s: DX12 swapchain created with device=%p but ID3D12CommandQueue QI failed (hr=0x%08X) — "
            "leaving swapchain queue unchanged",
            context, pDevice, qiHr);
    } else {
        HookLogImportant("%s: Non-DX12 swapchain for device=%p (queue QI hr=0x%08X) — skipping queue capture", context,
                         pDevice, qiHr);
    }
}

// Forward declarations
inline void InstallGlobalVTableHooks();

inline void HookSwapchainVTableViaTempSwapchain(bool presentOnly = false);

inline void EnsurePresentInlineHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || DXGIShared::HasPresentDetourHooks()) {
        return;
    }

    static std::atomic<int> s_installAttemptCount{0};
    const int attempt = s_installAttemptCount.fetch_add(1, std::memory_order_relaxed) + 1;
    HookLog("DX12: Installing Present inline hooks via %s swapchain #%d (swapchain=%p)", source ? source : "real",
            attempt, pSwapChain);

    if (!DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
        HookLog("DX12: Present inline hook installation via %s swapchain failed", source ? source : "real");
        return;
    }

    if (DXGIShared::HasPresentInlineHooks()) {
        HookLogImportant("DX12: Present inline hooks are active via %s swapchain", source ? source : "real");
    } else if (DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present detour hooks are active via %s swapchain (external overlay-compatible path)",
                         source ? source : "real");
    } else {
        HookLog("DX12: Present inline hook installation via %s swapchain deferred to existing external hook chain",
                source ? source : "real");
    }
}

inline void RefreshPresentHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return;
    }

    HookLogImportant("DX12: Refreshing Present hook path via %s swapchain %p", source ? source : "real", pSwapChain);
    {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
                static std::atomic<int> s_backbufferLogCount{0};
                int idx = s_backbufferLogCount.fetch_add(1, std::memory_order_relaxed);
                if (idx < 24) {
                    HookLogImportant(
                        "DX12: Swapchain buffer count source=%s sc=%p actual=%u requested=%d "
                        "size=%ux%u swapEffect=%d (#%d)",
                        source ? source : "real", pSwapChain, desc.BufferCount, gfx.backbufferCount,
                        desc.BufferDesc.Width, desc.BufferDesc.Height, desc.SwapEffect, idx + 1);
                }
            }
        }
    }
    EnsurePresentInlineHooksForRealSwapchain(pSwapChain, source);
    DXGIShared::InstallHooks(pSwapChain, /*presentOnly=*/true);
    DXGIShared::RepairVTableHooksIfNeeded();
}

inline PFN_CreateSwapChain dx12_hook_oCreateSwapChainGlobal = nullptr;

inline PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwndGlobal = nullptr;

// Inline hook trampoline for CreateSwapChainForHwnd (code-level hook in dxgi.dll)
// This catches ALL calls regardless of which factory vtable is used (including
// Streamline SL proxy factories that bypass our vtable hooks).
inline PFN_CreateSwapChainForHwnd dx12_hook_s_oCreateSCForHwndInline = nullptr;

// Address of the real CreateSwapChainForHwnd in dxgi.dll (for deep hook removal)
inline void* dx12_hook_s_realCreateSCForHwndAddr = nullptr;

// Deep hook trampoline for calling the real CreateSwapChainForHwnd
inline PFN_CreateSwapChainForHwnd dx12_hook_s_deepHookTrampoline = nullptr;

// Overlay suspension: cooldown after swapchain creation (FG switch, resize, etc.)
// to reduce our D3D12 footprint while the game's internal state machine stabilizes.
inline std::atomic<int64_t> dx12_hook_g_OverlayCooldownUntilQpc{0};

inline constexpr int64_t dx12_hook_kTransitionCooldownMs = 1500;  // 1.5 s

inline void StartTransitionCooldown() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    dx12_hook_g_OverlayCooldownUntilQpc.store(now.QuadPart + freq.QuadPart * dx12_hook_kTransitionCooldownMs / 1000,
                                    std::memory_order_release);
    // Discard any pending deferred Signal — the queue may change during FG switch
    dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    HookLogImportant("DX12: Overlay transition cooldown started (%lldms)", (long long)dx12_hook_kTransitionCooldownMs);
}

inline bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
    bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source) {
    const bool streamlineStartupHandoffPending =
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool nativeFSRInternalNoCallbackComposition = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    if (!ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(
            dx12_hook_g_HadFSRFGPhase, explicitSetOptionsActivation, authoritativeStreamlineHandoff,
            authoritativeFSRActive, dx12_hook_g_SwapchainQueue != nullptr,
            dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue, streamlineStartupHandoffPending,
            runtimeOwnedNativeFGPresentPath,
            nativeFSRInternalNoCallbackComposition)) {
        return false;
    }

    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
        "proven Streamline takeover cleared stale native-FG Present ownership");
    ForceClearNativeFSRInternalNoCallbackComposition(
        "proven Streamline takeover cleared stale native-FG Present ownership");
    dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
    dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
    HookLogImportant(
        "DX12: Proven Streamline takeover after FSR — cleared stale native-FG Present ownership "
        "(source=%s proof=%s explicit=%d authoritativeHandoff=%d fsrApi=%d handoffPending=%d "
        "scQueue=%p origGame=%p nativeFGPath=%d noCallback=%d)",
        source ? source : "Streamline activation",
        authoritativeStreamlineHandoff ? "authoritative-handoff" : "explicit-setoptions",
        explicitSetOptionsActivation ? 1 : 0, authoritativeStreamlineHandoff ? 1 : 0,
        authoritativeFSRActive ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0, dx12_hook_g_SwapchainQueue,
        dx12_hook_g_OriginalGameQueue, runtimeOwnedNativeFGPresentPath ? 1 : 0,
        nativeFSRInternalNoCallbackComposition ? 1 : 0);
    return true;
}

// HWND → swapchain tracking for diagnostics and E_ACCESSDENIED recovery.
// We do NOT AddRef tracked swapchains — this avoids extending their lifetime
// beyond what the game intends, which previously caused UE5 assertion crashes
// during FG switching (our AddRef kept the old SC alive, holding the HWND,
// and our forced destruction happened at the wrong time in UE5's lifecycle).
inline std::mutex dx12_hook_s_hwndSwapchainMutex;

inline std::map<HWND, std::vector<IDXGISwapChain*>> dx12_hook_s_hwndSwapchainMap;

inline void MarkThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath = nullptr) {
    DXGIShared::DX12_RegisterThirdPartyOverlaySwapchain(pSwapChain, creatorModulePath);
}

inline void MarkThirdPartyOverlaySwapchain(IDXGISwapChain1* pSwapChain, const char* creatorModulePath = nullptr) {
    MarkThirdPartyOverlaySwapchain(static_cast<IDXGISwapChain*>(pSwapChain), creatorModulePath);
}

inline void ForgetSwapchainFromTracking(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }

    DXGIShared::DX12_UnregisterThirdPartyOverlaySwapchain(pSwapChain);

    std::lock_guard<std::mutex> hwndLock(dx12_hook_s_hwndSwapchainMutex);
    for (auto it = dx12_hook_s_hwndSwapchainMap.begin(); it != dx12_hook_s_hwndSwapchainMap.end();) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), pSwapChain), vec.end());
        if (vec.empty()) {
            it = dx12_hook_s_hwndSwapchainMap.erase(it);
        } else {
            ++it;
        }
    }
}

// Track a swapchain's HWND association (called from ProcessFrame and deep hook).
// NO AddRef — raw pointer tracking only. Pointers may become stale when the
// game destroys the swapchain, which is fine because we only use them for
// reactive E_ACCESSDENIED recovery with SEH protection.
inline void TrackSwapchainHwnd(IDXGISwapChain* pSwapChain, HWND hWnd) {
    if (!hWnd || !pSwapChain)
        return;
    std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
    auto& vec = dx12_hook_s_hwndSwapchainMap[hWnd];
    for (auto* sc : vec) {
        if (sc == pSwapChain)
            return;  // Already tracked
    }
    vec.push_back(pSwapChain);
}

// Forward declaration — defined below near DetourCreateSwapChainGlobal
inline bool IsStreamlineLoaded();

// Deep hook wrapper for CreateSwapChainForHwnd.
// Intercepts ALL callers (including Streamline's internal trampoline calls).
// Uses REACTIVE E_ACCESSDENIED recovery: tries the call first, only intervenes
// if it fails. This avoids destroying swapchains prematurely (which caused UE5
// assertion crashes when our proactive pre-check destroyed SCs that UE5's
// deferred viewport code still referenced).
inline HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (dx12_hook_s_deepHookTrampoline)
            return dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    // Skip side-effects for temp swapchains created during hook installation
    if (dx12_hook_g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("DeepHook: Temp swapchain creation — passthrough (no tracking)");
        return dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    const CreateSwapchainForHwndCallerContext callerContext = ResolveCreateSwapchainForHwndCallerContext();
    const void* callerAddress = callerContext.callerAddress;
    const bool callerFromFFXFGModule = callerContext.callerFromFFXFGModule;
    const bool rawCallerFromThirdPartyOverlay = callerContext.callerFromThirdPartyOverlay;
    const char* callerModulePath = callerContext.callerModulePath;
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "DeepHook", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    HookLogImportant("DeepHook: CreateSwapChainForHwnd ENTER factory=%p device=%p hwnd=%p BufferCount=%u SwapEffect=%d",
                     pThis, pDevice, hWnd, pDesc ? pDesc->BufferCount : 0, pDesc ? (int)pDesc->SwapEffect : -1);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DeepHook", captureEvidence, pDesc->BufferCount,
                                                               pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant("DeepHook: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("DeepHook: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "DeepHook");

    // Try the call first — let the game/SL handle SC lifecycle naturally
    HRESULT hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("DeepHook: Trampoline returned hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));

    const bool protectedOfficialFFXStartupCreate =
        ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence);
    if (SUCCEEDED(hr) && ppSC && *ppSC && !callerFromThirdPartyOverlay && !protectedOfficialFFXStartupCreate) {
        // Only start transition cooldown when a swapchain recreation actually
        // succeeded. Starting it on E_ACCESSDENIED leaves the overlay in a
        // half-transitioned state while Streamline/game keeps the old chain.
        StartTransitionCooldown();
    }

    // Reactive recovery: if E_ACCESSDENIED, an old SC still holds the HWND.
    // DON'T force-destroy — that invalidates game-held references and causes
    // delayed UE5 assertion crashes. For ordinary callers we can clean up our
    // overlay refs and do a very brief retry. For runtime-managed Streamline /
    // authoritative FFX takeover paths, return the error untouched so the
    // runtime can manage its own swapchain state machine.
    if (hr == E_ACCESSDENIED && hWnd) {
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForRuntimeManagedFG =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        const char* passThroughReason =
            callerFromThirdPartyOverlay
                ? "third-party overlay caller"
                : ((callerFromFFXFGModule || ffxFrameGenerationInStack) ? "authoritative FFX takeover"
                                                                        : "Streamline active");
        if (ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery(passThroughForRuntimeManagedFG,
                                                                               callerFromThirdPartyOverlay) ==
            ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate) {
            // See the INLINE variant: a failed runtime-managed create is fatal to the game (null swapchain
            // deref, session 20260702_092933). Minimal CE unpin (retained startup-activation swapchain) +
            // retry first; full overlay cleanup only as the last resort before returning the error.
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — %s, minimal CE unpin + retry "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d retained=%d module=%s)",
                hWnd, passThroughReason, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0,
                HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
                callerModulePath[0] ? callerModulePath : "unknown");
            ReleaseStreamlineStartupActivationSwapchain("DeepHook: E_ACCESSDENIED runtime-managed minimal recovery");
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: runtime-managed minimal-recovery retry %d succeeded hr=0x%08X sc=%p",
                                     attempt, hr, (ppSC && *ppSC) ? (void*)*ppSC : nullptr);
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "DeepHook: runtime-managed minimal recovery still E_ACCESSDENIED — escalating to full overlay "
                    "cleanup for HWND=%p",
                    hWnd);
                {
                    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                    dx12_hook_g_LastSwapChain = nullptr;
                    CleanupOverlay();
                    CleanupRTVs();
                    dx12_hook_g_State.overlayInit = false;
                }
                {
                    std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                    dx12_hook_s_hwndSwapchainMap.erase(hWnd);
                }
                if (ppSC && *ppSC) {
                    ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
                }
                for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                    Sleep(20);
                    hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                    if (SUCCEEDED(hr)) {
                        HookLogImportant("DeepHook: escalated full-cleanup retry %d succeeded hr=0x%08X", attempt, hr);
                    }
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "DeepHook: E_ACCESSDENIED persists after CE unpin + full cleanup — returning the error to the "
                    "caller (HWND=%p)",
                    hWnd);
            }
        } else {
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED for HWND=%p — cleaning up overlay refs "
                "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
                hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
                streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
                ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

            // Clean up ALL overlay resources so we don't hold stale refs that
            // prevent DXGI from releasing the HWND association.  Must match the
            // cleanup sequence in DX12_OnSwapchainResizeBegin which successfully
            // avoids E_ACCESSDENIED before ResizeBuffers.
            {
                std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                dx12_hook_g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                dx12_hook_g_State.overlayInit = false;
            }
            // The retained Streamline startup-activation swapchain is an
            // AddRef'd swapchain reference; while CE pins it, DXGI refuses a
            // new swapchain on the same HWND (session 20260613_032326: the
            // app's native recreate after DLSS->OFF failed E_ACCESSDENIED
            // through all retries and stopped its main loop).
            ReleaseStreamlineStartupActivationSwapchain("DeepHook: CreateSwapChainForHwnd E_ACCESSDENIED recovery");
            HookLogImportant("DeepHook: Released overlay + RTV refs for HWND=%p", hWnd);

            // Clear our tracking entries (raw pointers, no Release needed)
            {
                std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                dx12_hook_s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }

            // Retry: 10 attempts × 20ms = 200ms max.  FSR FG activation may
            // need time for the game to release its own swapchain refs after
            // we've released ours.
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: Retry attempt %d succeeded hr=0x%08X sc=%p", attempt, hr,
                                     (ppSC ? *ppSC : nullptr));
                    break;
                }
            }
            if (FAILED(hr)) {
                HookLogImportant("DeepHook: All retries exhausted — returning E_ACCESSDENIED to caller (HWND=%p)",
                                 hWnd);
            }
        }
    }

    // Post-track: record the new swapchain for future reactive recovery
    if (SUCCEEDED(hr) && ppSC && *ppSC && hWnd) {
        if (callerFromThirdPartyOverlay) {
            MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
            HookLogImportant(
                "DeepHook: Third-party overlay caller %s created swapchain %p for HWND=%p — leaving CE queue and "
                "transition state unchanged",
                callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
            return hr;
        }
        IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
        if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, newSC, "DeepHook", hr)) {
            return hr;
        }
        if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, newSC, "DeepHook")) {
            return hr;
        }

        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("DeepHook: Created & tracked swapchain %p for HWND=%p", *ppSC, hWnd);

        // SL (or game) just created a new swapchain. Refresh the full Present
        // hook path on it — the new swapchain may expose a different Present
        // implementation or vtable than the one we initially hooked.
        RefreshPresentHooksForRealSwapchain(newSC, "DeepHook");

        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "DeepHook", captureEvidence);
    } else if (FAILED(hr)) {
        HookLogImportant("DeepHook: CreateSwapChainForHwnd FAILED hr=0x%08X hwnd=%p", hr, hWnd);
    }

    return hr;
}

// Inline hook detour for CreateSwapChainForHwnd.
// This code-level hook fires for ALL calls to the real DXGI function,
// including internal calls by Streamline's DLFG module (linkSwapchainToCmdQueue).
// When E_ACCESSDENIED occurs (HWND already has a flip-model swapchain), we
// only do CE-owned cleanup/retry for non-runtime-managed cases. Streamline and
// authoritative FFX takeover paths must manage that handoff themselves.
inline HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndInline(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    if (HookIsShuttingDown()) {
        if (dx12_hook_s_oCreateSCForHwndInline)
            return dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    // Skip side-effects for temp swapchains created during hook installation
    if (dx12_hook_g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
        HookLog("CreateSwapChainForHwnd INLINE: Temp swapchain — passthrough");
        return dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }

    MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled();

    const CreateSwapchainForHwndCallerContext callerContext = ResolveCreateSwapchainForHwndCallerContext();
    const void* callerAddress = callerContext.callerAddress;
    const bool callerFromFFXFGModule = callerContext.callerFromFFXFGModule;
    const bool rawCallerFromThirdPartyOverlay = callerContext.callerFromThirdPartyOverlay;
    const char* callerModulePath = callerContext.callerModulePath;
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "CreateSwapChainForHwnd INLINE", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule,
        ffxFrameGenerationInStack, callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    HookLogImportant("CreateSwapChainForHwnd INLINE: factory=%p device=%p hwnd=%p", pThis, pDevice, hWnd);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("CreateSwapChainForHwnd INLINE", captureEvidence,
                                                               pDesc->BufferCount, pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant("INLINE: Skipping BufferCount override %u < game's %u (flip model)", requested,
                                 modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("INLINE: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "CreateSwapChainForHwnd INLINE");

    ID3D12CommandQueue* deferredStreamlineHandoffQueue = nullptr;
    const bool deferPresentHookRefreshForStreamlineHandoff =
        ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(pDevice, captureEvidence,
                                                                        &deferredStreamlineHandoffQueue);
    auto deferredStreamlineHandoffQueueRelease = ce::make_scope_guard([&]() {
        if (deferredStreamlineHandoffQueue) {
            deferredStreamlineHandoffQueue->Release();
            deferredStreamlineHandoffQueue = nullptr;
        }
    });
    if (deferPresentHookRefreshForStreamlineHandoff) {
        DXGIShared::ReleaseSwapchainPresentVTableHooksForRuntimeHandoff("post-FSR Streamline runtime swapchain create");
        HookLogImportant(
            "CreateSwapChainForHwnd INLINE: Released CE Present vtable ownership before post-FSR Streamline runtime "
            "handoff (queue=%p origGame=%p runtime=%s fsrApi=%d hadFSR=%d streamlineLoaded=%d)",
            deferredStreamlineHandoffQueue, dx12_hook_g_OriginalGameQueue,
            ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
            dx12_hook_g_HadFSRFGPhase ? 1 : 0, IsStreamlineLoaded() ? 1 : 0);
        // MAKE-BEFORE-BREAK for the runtime's replacement swapchain: the retained startup-activation
        // swapchain is an AddRef'd COM reference to the OLD (pre-handoff) chain. Holding it across this
        // create pins the old chain's HWND association, so DXGI fails the runtime's replacement create
        // with E_ACCESSDENIED and the game crashes dereferencing the null swapchain (GTA FSR->DLSS apply,
        // session 20260702_092933). Its purpose — PostSL startup recovery on the OLD chain — is moot once
        // the runtime replaces the swapchain, so release it BEFORE forwarding the create.
        ReleaseStreamlineStartupActivationSwapchain(
            "CreateSwapChainForHwnd INLINE: pre post-FSR Streamline runtime swapchain create");
    }

    HRESULT hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
    HookLogImportant("CreateSwapChainForHwnd INLINE: result hr=0x%08X sc=%p", hr, (ppSC && *ppSC) ? *ppSC : nullptr);

    if (hr == E_ACCESSDENIED && hWnd) {
        // When a frame-generation runtime is managing swapchain lifecycle,
        // don't interfere. Our CleanupOverlay() flushes the GPU (200ms
        // Signal+Wait) and destroys overlay resources, which disrupts the
        // runtime's internal handoff state machine.
        const bool streamlineModuleLoaded = IsStreamlineLoaded();
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
        const bool passThroughForRuntimeManagedFG =
            ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
                streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
                ffxFrameGenerationInStack);
        const char* passThroughReason =
            callerFromThirdPartyOverlay
                ? "third-party overlay caller"
                : ((callerFromFFXFGModule || ffxFrameGenerationInStack) ? "authoritative FFX takeover"
                                                                        : "Streamline active");
        if (ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery(passThroughForRuntimeManagedFG,
                                                                               callerFromThirdPartyOverlay) ==
            ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate) {
            // A failed runtime-managed create is FATAL to the game — GTA dereferences the null swapchain
            // and crashes (session 20260702_092933) — so the old blind pass-through is not acceptable.
            // Recover with the MINIMAL CE-owned unpin first: drop the retained startup-activation
            // swapchain reference (an AddRef'd COM ref that pins the old chain's HWND association) and
            // retry, WITHOUT the full overlay teardown / GPU flush that could disturb the runtime's own
            // handoff state machine. Escalate to the full cleanup only if the HWND stays pinned — a
            // disturbed handoff beats a guaranteed crash.
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — %s, minimal CE unpin + retry "
                "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d retained=%d module=%s)",
                hWnd, passThroughReason, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
                callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0,

                HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
                callerModulePath[0] ? callerModulePath : "unknown");
            ReleaseStreamlineStartupActivationSwapchain(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED runtime-managed minimal recovery");
            for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant(
                        "CreateSwapChainForHwnd INLINE: runtime-managed minimal-recovery retry %d succeeded "
                        "hr=0x%08X sc=%p",
                        attempt, hr, (ppSC && *ppSC) ? (void*)*ppSC : nullptr);
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: runtime-managed minimal recovery still E_ACCESSDENIED — "
                    "escalating to full overlay cleanup for HWND=%p",
                    hWnd);
                {
                    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                    dx12_hook_g_LastSwapChain = nullptr;
                    CleanupOverlay();
                    CleanupRTVs();
                    dx12_hook_g_State.overlayInit = false;
                }
                {
                    std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                    dx12_hook_s_hwndSwapchainMap.erase(hWnd);
                }
                if (ppSC && *ppSC) {
                    ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
                }
                for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                    Sleep(20);
                    hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                    if (SUCCEEDED(hr)) {
                        HookLogImportant(
                            "CreateSwapChainForHwnd INLINE: escalated full-cleanup retry %d succeeded hr=0x%08X",
                            attempt, hr);
                    }
                }
            }
            if (hr == E_ACCESSDENIED) {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED persists after CE unpin + full cleanup — "
                    "returning the error to the caller (HWND=%p)",
                    hWnd);
            }
        } else {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: E_ACCESSDENIED for HWND=%p — "
                "cleaning up overlay refs "
                "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
                hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
                streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
                ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

            // Clean up ALL overlay resources — same sequence as deep hook and
            // DX12_OnSwapchainResizeBegin to fully release the HWND association.
            {
                std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                dx12_hook_g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                dx12_hook_g_State.overlayInit = false;
            }
            // See the deep-hook recovery above: a retained startup-activation
            // swapchain pins the HWND association and makes every retry fail.
            ReleaseStreamlineStartupActivationSwapchain("CreateSwapChainForHwnd INLINE: E_ACCESSDENIED recovery");
            HookLogImportant("CreateSwapChainForHwnd INLINE: Released overlay + RTV refs for HWND=%p", hWnd);
            {
                std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                dx12_hook_s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }

            // Retry: 10 attempts × 20ms = 200ms max
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_oCreateSCForHwndInline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("CreateSwapChainForHwnd INLINE: Retry attempt %d succeeded hr=0x%08X", attempt,
                                     hr);
                    break;
                }
            }
            if (FAILED(hr)) {
                HookLogImportant(
                    "CreateSwapChainForHwnd INLINE: All retries exhausted — returning E_ACCESSDENIED to caller "
                    "(HWND=%p)",
                    hWnd);
            }
        }
    }

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        if (callerFromThirdPartyOverlay) {
            MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: Third-party overlay caller %s created swapchain %p for HWND=%p — "
                "leaving CE queue and transition state unchanged",
                callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
            return hr;
        }
        IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
        if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, newSC, "CreateSwapChainForHwnd INLINE", hr)) {
            return hr;
        }
        if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, newSC,
                                                             "CreateSwapChainForHwnd INLINE")) {
            return hr;
        }

        TrackSwapchainHwnd(*ppSC, hWnd);
        HookLogImportant("CreateSwapChainForHwnd INLINE: Created swapchain %p for HWND=%p", *ppSC, hWnd);

        // A later runtime-created DX12 swapchain can expose a different Present
        // implementation than the one we patched during startup. Refresh the
        // full per-swapchain Present hook path here so top-level Present traffic
        // stays visible after a Streamline handoff.  For the post-FSR Streamline
        // handoff, however, Streamline must establish its outer Present chain
        // first; CE remains available through the inline/re-entrant PostSL path.
        if (deferPresentHookRefreshForStreamlineHandoff) {
            HookLogImportant(
                "CreateSwapChainForHwnd INLINE: Deferring CE Present hook refresh for post-FSR Streamline runtime "
                "handoff (sc=%p queue=%p)",
                newSC, deferredStreamlineHandoffQueue);
        } else {
            RefreshPresentHooksForRealSwapchain(newSC, "CreateSwapChainForHwnd INLINE");
        }

        CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "CreateSwapChainForHwnd INLINE", captureEvidence);
    }

    return hr;
}

// Check if Streamline (DLSS FG interposer) is loaded.
// When present, we MUST NOT wrap swapchains with CWrapDXGISwapChain because:
// - Streamline manages the real swapchain lifecycle internally
// - Our wrapper adds an extra COM ref layer that prevents Streamline from
//   destroying the old SC before creating the FG SC on the same HWND
// - This causes E_ACCESSDENIED when DLSS FG tries to activate
// The inline Present hooks (installed on the real DXGI function) provide the
// same interception without interfering with Streamline's lifecycle management.
inline bool IsStreamlineLoaded() {
    static bool detected = false;
    if (detected)
        return true;
    if (GetModuleHandleA("sl.interposer.dll") != nullptr) {
        detected = true;
        HookLogImportant("DX12: Streamline interposer detected — skipping swapchain wrapping for FG compat");
        return true;
    }
    return false;
}

// Detour for global CreateSwapChain hook
inline HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice,
                                                             DXGI_SWAP_CHAIN_DESC* pDesc,
                                                             IDXGISwapChain** ppSwapChain) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (dx12_hook_oCreateSwapChainGlobal)
            return dx12_hook_oCreateSwapChainGlobal(pThis, pDevice, pDesc, ppSwapChain);
        return E_FAIL;
    }

    HookLog("DetourCreateSwapChainGlobal: CALLED (factory=%p, device=%p, swapEffect=%d)", pThis, pDevice,
            pDesc ? (int)pDesc->SwapEffect : -1);

    const void* callerAddress = CE_RETURN_ADDRESS();
    char callerModulePath[MAX_PATH] = {};
    const bool rawCallerFromThirdPartyOverlay =
        callerAddress && TryGetModulePathFromCodeAddress(callerAddress, callerModulePath, sizeof(callerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath);
    const bool callerFromFFXFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(callerAddress);
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "DetourCreateSwapChainGlobal", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC modifiedDesc;
    DXGI_SWAP_CHAIN_DESC* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DetourCreateSwapChainGlobal", captureEvidence,
                                                               pDesc->BufferCount, pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant(
                    "DetourCreateSwapChainGlobal: Skipping BufferCount override %u < game's %u (flip model)", requested,
                    modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("DetourCreateSwapChainGlobal: Overriding BufferCount %u -> %u",
                                 modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    // Call original with (possibly) modified descriptor
    HRESULT hr = dx12_hook_oCreateSwapChainGlobal(pThis, pDevice, pDescToUse, ppSwapChain);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        if (callerFromThirdPartyOverlay) {
            MarkThirdPartyOverlaySwapchain(*ppSwapChain, callerModulePath);
            HookLogImportant(
                "DetourCreateSwapChainGlobal: Third-party overlay caller %s created swapchain %p — bypassing CE "
                "swapchain side-effects",
                callerModulePath[0] ? callerModulePath : "unknown", *ppSwapChain);
            CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSwapChain, "CreateSwapChain Global overlay bypass",
                                                  captureEvidence);
            return hr;
        }

        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainGlobal: Creating swapchain %ux%u", pDesc->BufferDesc.Width,
                    pDesc->BufferDesc.Height);
        }

        if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, *ppSwapChain,
                                                             "CreateSwapChain")) {
            return hr;
        }

        RefreshPresentHooksForRealSwapchain(*ppSwapChain, "CreateSwapChain");

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainGlobal: Streamline present, skipping wrap (sc=%p)", *ppSwapChain);
            if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
                CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSwapChain,
                                                      "CreateSwapChain Global Streamline fallback", captureEvidence);
            }
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        // Wrap the swapchain with CWrapDXGISwapChain
        HookLog("DetourCreateSwapChainGlobal: Wrapping swapchain %p", *ppSwapChain);
        auto* wrapper = new CWrapDXGISwapChain(*ppSwapChain, pDevice);
        *ppSwapChain = wrapper;
        HookLog("DetourCreateSwapChainGlobal: Swapchain wrapped successfully");

        // Don't capture queue here — global hooks fire for non-game swapchains
        // (e.g. Social Club internal).  The inline hook handles queue capture.
    }

    return hr;
}

// Detour for global CreateSwapChainForHwnd hook
inline HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd,
                                                                    const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc,
                                                                    IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
    // CRITICAL: Pass through during shutdown
    if (HookIsShuttingDown()) {
        if (dx12_hook_oCreateSwapChainForHwndGlobal)
            return dx12_hook_oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
        return E_FAIL;
    }

    HookLogImportant(
        "DetourCreateSwapChainForHwndGlobal: CALLED (factory=%p, device=%p, "
        "hwnd=%p)",
        pThis, pDevice, hWnd);

    const void* callerAddress = CE_RETURN_ADDRESS();
    char callerModulePath[MAX_PATH] = {};
    const bool rawCallerFromThirdPartyOverlay =
        callerAddress && TryGetModulePathFromCodeAddress(callerAddress, callerModulePath, sizeof(callerModulePath)) &&
        ce::overlay_compat::IsThirdPartyOverlayModulePath(callerModulePath);
    const bool callerFromFFXFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromFFXFrameGenerationModule(callerAddress);
    char ffxStackModulePath[MAX_PATH] = {};
    const bool ffxFrameGenerationInStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
    const bool callerFromStreamlineFGModule =
        callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
    const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
    const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
        "DetourCreateSwapChainForHwndGlobal", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule,
        ffxFrameGenerationInStack, callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
    const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
        callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
        callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
    const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
    if (pDesc && !applyDescriptorOverrides) {
        LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DetourCreateSwapChainForHwndGlobal", captureEvidence,
                                                               pDesc->BufferCount, pDesc->Flags, pDesc->SwapEffect);
    }
    if (pDesc && applyDescriptorOverrides) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                HookLogImportant(
                    "DetourCreateSwapChainForHwndGlobal: Skipping BufferCount override %u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                HookLogImportant("DetourCreateSwapChainForHwndGlobal: Overriding BufferCount %u -> %u",
                                 modifiedDesc.BufferCount, requested);
                modifiedDesc.BufferCount = requested;
            }
        }
        pDescToUse = &modifiedDesc;
    }

    PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "DetourCreateSwapChainForHwndGlobal");

    // Forward the original external caller through the DXGI vtable -> real DXGI
    // function chain so our inline/deep hooks don't misclassify CE's own detour
    // frame as the authoritative CreateSwapChainForHwnd caller.
    ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard inlineSideEffectGuard;
    ScopedForwardedCreateSwapchainForHwndCallerContext forwardedCallerContext(callerAddress, callerModulePath);
    HRESULT hr = dx12_hook_oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);

    if (ce::dx12_overlay_policy::ShouldSkipGlobalCreateSwapchainForHwndSideEffectsAfterInlineForward(
            inlineSideEffectGuard.InlineHandledForwardedCall())) {
        static std::atomic<int> s_inlineHandledForwardedGlobalLogCount{0};
        const int logCount = s_inlineHandledForwardedGlobalLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 128) == 0) {
            HookLogImportant(
                "DetourCreateSwapChainForHwndGlobal: inline CreateSwapChainForHwnd hook already handled forwarded "
                "swapchain side-effects (hr=0x%08X sc=%p hwnd=%p caller=%s count=%d) — skipping duplicate global "
                "processing",
                hr, (ppSC && *ppSC) ? *ppSC : nullptr, hWnd, callerModulePath[0] ? callerModulePath : "unknown",
                logCount + 1);
        }
        return hr;
    }

    if (SUCCEEDED(hr) && ppSC && *ppSC) {
        if (callerFromThirdPartyOverlay) {
            MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
            HookLogImportant(
                "DetourCreateSwapChainForHwndGlobal: Third-party overlay caller %s created swapchain %p for HWND=%p "
                "— bypassing CE swapchain side-effects",
                callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
            CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "CreateSwapChainForHwnd Global overlay bypass",
                                                  captureEvidence);
            return hr;
        }
        if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, *ppSC, "DetourCreateSwapChainForHwndGlobal",
                                                                  hr)) {
            return hr;
        }

        // Log swapchain details
        if (pDesc) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Creating swapchain %ux%u", pDesc->Width, pDesc->Height);
        }

        if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, *ppSC,
                                                             "CreateSwapChainForHwnd")) {
            return hr;
        }

        StartTransitionCooldown();

        RefreshPresentHooksForRealSwapchain(*ppSC, "CreateSwapChainForHwnd");

        // When Streamline is loaded, skip wrapping to avoid blocking FG swapchain
        // lifecycle management.  Inline Present hooks provide the same interception.
        if (IsStreamlineLoaded()) {
            HookLog("DetourCreateSwapChainForHwndGlobal: Streamline present, skipping wrap (sc=%p)", *ppSC);
            if (DXGIShared::ShouldCaptureQueueWhenSkippingWrapForStreamline(true)) {
                CaptureSwapchainQueueFromCreateDevice(
                    pDevice, *ppSC, "CreateSwapChainForHwnd Global Streamline fallback", captureEvidence);
            }
            return hr;
        }

        // NOTE: We don't install global Present vtable hooks for DX12.
        // The wrapper (CWrapDXGISwapChain) handles all Present interception.
        // This avoids conflicts between vtable hooks and wrapper interception
        // that caused stack overflow crashes.

        // CRITICAL: Check if this swapchain is already wrapped
        // This prevents double-wrapping which causes infinite Present recursion
        void* pExistingWrapper = nullptr;
        if (SUCCEEDED((*ppSC)->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
            ((IUnknown*)pExistingWrapper)->Release();
            HookLog(
                "DetourCreateSwapChainForHwndGlobal: Swapchain already wrapped, "
                "skipping double-wrap");
            return hr;
        }

        HookLog("DetourCreateSwapChainForHwndGlobal: Wrapping swapchain %p", *ppSC);
        auto* wrapper = new CWrapDXGISwapChain(*ppSC, pDevice);
        *ppSC = (IDXGISwapChain1*)wrapper;
        HookLog("DetourCreateSwapChainForHwndGlobal: Swapchain wrapped successfully");

        // Don't capture queue here — inline hook handles queue capture for all
        // CreateSwapChainForHwnd calls, including FG runtime swapchains.
    }

    return hr;
}

// This hooks the factory vtable directly in the DXGI module
inline void InstallGlobalVTableHooks() {
    HookLog("DX12: InstallGlobalVTableHooks called");

    // CRITICAL: Install global factory vtable hooks to catch swapchain creation
    // even for factories created before our IAT hooks were installed.
    // This ensures ALL swapchains get wrapped regardless of timing.

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
        return;
    }

    // Get CreateDXGIFactory1 export to create a temp factory
    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found");
        return;
    }

    // Create a temp factory to get its vtable
    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create temp factory for vtable extraction");
        return;
    }

    // Get the vtable - ALL IDXGIFactory instances share this vtable
    void** vtable = *(void***)pFactory;
    HookLog("DX12: Factory vtable at %p", vtable);

    // Save the real CreateSwapChainForHwnd address BEFORE vtable patching
    void* realCreateSCForHwndAddr = vtable[15];
    dx12_hook_s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;

    // Hook CreateSwapChain (vtable[10] for IDXGIFactory)
    // Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
    if (VTableHook::Create(reinterpret_cast<void*>(&vtable[10]), (LPVOID)DetourCreateSwapChainGlobal, (LPVOID*)&dx12_hook_oCreateSwapChainGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
    }

    if (VTableHook::Create(reinterpret_cast<void*>(&vtable[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal,
                           (LPVOID*)&dx12_hook_oCreateSwapChainForHwndGlobal)) {
        HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
    }

    pFactory->Release();

    // Also hook IDXGIFactory4 and IDXGIFactory6 vtables to catch games that
    // QueryInterface for higher factory versions (different vtable pointers).
    // CreateSwapChainForHwnd is at the same slot (15) in all factory versions
    // because IDXGIFactory4 inherits from IDXGIFactory3 → IDXGIFactory2.
    IDXGIFactory4* pFactory4 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory4)))) {
        void** vtable4 = *(void***)pFactory4;
        HookLog("DX12: IDXGIFactory4 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable4, vtable,
                (int)(vtable4 == vtable));
        if (vtable4 != vtable) {  // Different vtable pointer
            VTableHook::Create(reinterpret_cast<void*>(&vtable4[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(reinterpret_cast<void*>(&vtable4[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory4 vtable[10] and vtable[15]");
        }
        pFactory4->Release();
    } else {
        HookLog("DX12: IDXGIFactory4 not available");
    }

    IDXGIFactory6* pFactory6 = nullptr;
    if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory6)))) {
        void** vtable6 = *(void***)pFactory6;
        HookLog("DX12: IDXGIFactory6 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable6, vtable,
                (int)(vtable6 == vtable));
        if (vtable6 != vtable) {  // Different vtable pointer
            VTableHook::Create(reinterpret_cast<void*>(&vtable6[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
            VTableHook::Create(reinterpret_cast<void*>(&vtable6[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
            HookLog("DX12: Hooked IDXGIFactory6 vtable[10] and vtable[15]");
        }
        pFactory6->Release();
    } else {
        HookLog("DX12: IDXGIFactory6 not available");
    }

    // Install inline hook on CreateSwapChainForHwnd in dxgi.dll.
    // VTable hooks only patch a single vtable and miss calls through
    // Streamline's SL proxy factory (different COM vtable). Inline hooks
    // patch the actual function code and catch ALL callers.
    if (realCreateSCForHwndAddr && !dx12_hook_s_oCreateSCForHwndInline) {
        void* trampoline = nullptr;
        if (InlineHook::Install(realCreateSCForHwndAddr, (void*)DetourCreateSwapChainForHwndInline, &trampoline)) {
            dx12_hook_s_oCreateSCForHwndInline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed INLINE hook on CreateSwapChainForHwnd at %p", realCreateSCForHwndAddr);
        } else {
            HookLog("DX12: FAILED to install inline hook on CreateSwapChainForHwnd");
        }
    }

    // Install DEEP hook on CreateSwapChainForHwnd.
    // When Streamline hooks CreateSwapChainForHwnd at byte 0 and uses a saved
    // trampoline for internal calls (bypassing both our vtable and inline hooks),
    // the deep hook patches the function body past Streamline's JMP so ALL
    // callers are intercepted — including Streamline's linkSwapchainToCmdQueue.
    // The full wrapper pre-releases stale swapchains AND post-tracks new ones,
    // ensuring SL's shadow swapchains are tracked for subsequent releases.
    if (realCreateSCForHwndAddr) {
        void* trampoline = InlineHook::InstallDeepHook(realCreateSCForHwndAddr, (void*)DeepHookCreateSwapChainForHwnd);
        if (trampoline) {
            dx12_hook_s_deepHookTrampoline = (PFN_CreateSwapChainForHwnd)trampoline;
            HookLog("DX12: Installed DEEP hook on CreateSwapChainForHwnd at %p (trampoline=%p)",
                    realCreateSCForHwndAddr, trampoline);
        } else {
            HookLog("DX12: Deep hook not needed or failed for CreateSwapChainForHwnd");
        }
    }

    HookLog("DX12: Global factory vtable hooks installed");
}

// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)
inline void HookSwapchainVTableViaTempSwapchain(bool presentOnly) {
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    if (!hDXGI || !hD3D12)
        return;

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
    if (!pCreateFactory || !pD3D12CreateDevice)
        return;

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
        return;

    ID3D12Device* pDevice = nullptr;
    if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
        pFactory->Release();
        return;
    }

    // Hook CreateSampler on the device vtable
    // All D3D12 devices share the same vtable, so this hooks ALL devices
    DX12_HookDeviceVTable(pDevice);

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
        pDevice->Release();
        pFactory->Release();
        return;
    }

    // Create a minimal hidden window
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CE_Temp";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

    // Create temp swapchain
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = 2;
    scd.Height = 2;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // Mark that we're creating a temp swapchain for hook installation.
    // This prevents the CreateSwapChainForHwnd hooks from capturing the temp
    // queue as g_SwapchainQueue or tracking the temp swapchain.
    dx12_hook_g_CreatingTempSwapchain.store(true, std::memory_order_release);

    IDXGISwapChain1* pSwapChain = nullptr;
    HRESULT hr = E_FAIL;

    // CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
    // swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
    // wrapper If the original is not available, skip vtable hook installation
    if (dx12_hook_oCreateSwapChainForHwndGlobal) {
        // Call original directly - bypasses our wrapper
        hr = dx12_hook_oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            HookLog(
                "DX12: Created temp swapchain via original "
                "CreateSwapChainForHwnd (unwrapped)");
        }
    } else {
        HookLog(
            "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
            "Present vtable hooks");
    }

    dx12_hook_g_CreatingTempSwapchain.store(false, std::memory_order_release);

    if (SUCCEEDED(hr) && pSwapChain) {
        HookLog("DX12: Installing Present inline hooks via temp swapchain");
        if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
            HookLog("DX12: Present inline hooks installed successfully");
        } else {
            HookLog("DX12: Failed to install Present inline hooks");
        }
        pSwapChain->Release();
    } else {
        HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
    }

    // Hook ExecuteCommandLists on the temp queue's vtable.
    // All DX12 command queues share the same vtable, so this hooks ALL queues
    // (including the game's pre-existing queue). When ECL fires, it calls
    // DX12_SetCommandQueue which captures the game's actual queue pointer.
    DX12_HookQueueVTable(pQueue);

    // Cleanup
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(L"CE_Temp", wc.hInstance);
    pQueue->Release();
    pDevice->Release();
    pFactory->Release();
}

inline void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx,
                 D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride) {
    // CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
    // shutdown/reinit
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

    if (!dx12_hook_g_State.overlayInit || !cmdList)
        return;

    static std::atomic<int> s_drawOverlayLogCount{0};
    const bool logThisDraw = s_drawOverlayLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
    if (logThisDraw) {
        HookLogImportant(
            "DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)", cmdList,
            bufferIdx, isRealFrame ? 1 : 0, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0);
    }

    // CRITICAL FIX: Always set IPC client regardless of frame type.
    // RenderOverlay() guards on ipc being non-null, so if this was only set
    // on real frames, overlay would never render when isRealFrame is false.
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }

    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        g_OverlayAdapter.SetGraphicsAPI(api);
        // HDR state is set during overlay init (ProcessFrame) by querying the
        // display output's actual color space — not here, to avoid the false
        // positive of R10G10B10A2_UNORM being treated as HDR in SDR mode.
    }

    // Set Render Target for Custom Overlay
    // When rtvOverride is set (offscreen compositing path), use it instead of the backbuffer RTV.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    if (rtvOverride) {
        rtvHandle = *rtvOverride;
    } else {
        // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
        if (!dx12_hook_g_State.rtvDescHeap) {
            HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
            return;
        }
        rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
    }

    g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);
    g_OverlayAdapter.SetDX12UploadSlotFence(
        dx12_hook_g_State.fence, ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                           DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) || g_FGCompat.IsFGActive(),
                           dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue));

    // Render overlay content
    g_OverlayAdapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
    if (logThisDraw) {
        HookLogImportant("DX12: DrawOverlay end (bufferIdx=%u)", bufferIdx);
    }
}

// Ensure offscreen render target exists and matches backbuffer dimensions/format.
// Used for the copy-render-copy overlay compositing path that avoids
// OMSetRenderTargets(swapchain) + SetDescriptorHeaps GPU pipeline stalls.
inline bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
    if (dx12_hook_g_State.offscreenRT && dx12_hook_g_State.offscreenWidth == width && dx12_hook_g_State.offscreenHeight == height &&
        dx12_hook_g_State.offscreenFormat == format) {
        return true;
    }

    // Release old resources if dimensions/format changed
    if (dx12_hook_g_State.offscreenRT) {
        dx12_hook_g_State.offscreenRT->Release();
        dx12_hook_g_State.offscreenRT = nullptr;
    }
    if (dx12_hook_g_State.offscreenRtvHeap) {
        dx12_hook_g_State.offscreenRtvHeap->Release();
        dx12_hook_g_State.offscreenRtvHeap = nullptr;
    }

    // Create RTV descriptor heap for offscreen target
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRtvHeap));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RTV heap hr=0x%08X", hr);
        return false;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = width;
    resDesc.Height = height;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = format;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = format;

    hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                         &clearVal, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRT));
    if (FAILED(hr)) {
        HookLog("DX12: Failed to create offscreen RT %ux%u fmt=%d hr=0x%08X", width, height, format, hr);
        dx12_hook_g_State.offscreenRtvHeap->Release();
        dx12_hook_g_State.offscreenRtvHeap = nullptr;
        return false;
    }

    device->CreateRenderTargetView(dx12_hook_g_State.offscreenRT, nullptr,
                                   dx12_hook_g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());

    dx12_hook_g_State.offscreenRT->SetName(L"CE_OverlayOffscreenRT");

    dx12_hook_g_State.offscreenWidth = width;
    dx12_hook_g_State.offscreenHeight = height;
    dx12_hook_g_State.offscreenFormat = format;

    HookLogImportant("DX12: Created offscreen RT %ux%u fmt=%d for overlay compositing", width, height, format);
    return true;
}

inline bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue,
                                                          const char* context) {
    if (!swapChain || !swapchainQueue) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    const HRESULT descHr = swapChain->GetDesc(&desc);
    if (FAILED(descHr) || desc.BufferCount == 0 || desc.BufferCount > 8) {
        HookLogImportant(
            "DX12: PostSL handoff prewarm refused invalid swapchain description "
            "(source=%s sc=%p queue=%p hr=0x%08X buffers=%u)",
            context ? context : "unknown", swapChain, swapchainQueue, (unsigned)descHr, desc.BufferCount);
        return false;
    }

    ID3D12Device* queueDevice = nullptr;
    const HRESULT deviceHr = swapchainQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    IDXGISwapChain3* swapChain3 = nullptr;
    const HRESULT sc3Hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    if (FAILED(deviceHr) || !queueDevice || FAILED(sc3Hr) || !swapChain3) {
        HookLogImportant(
            "DX12: PostSL handoff prewarm missing exact queue/swapchain prerequisites "
            "(source=%s sc=%p queue=%p deviceHr=0x%08X sc3Hr=0x%08X)",
            context ? context : "unknown", swapChain, swapchainQueue, (unsigned)deviceHr, (unsigned)sc3Hr);
        if (swapChain3) {
            swapChain3->Release();
        }
        if (queueDevice) {
            queueDevice->Release();
        }
        return false;
    }

    const ULONGLONG startedMs = GetTickCount64();
    bool ready = false;
    bool overlayInit = false;
    bool syncInit = false;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    {
        std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
        const HRESULT deviceReason = queueDevice->GetDeviceRemovedReason();
        if (SUCCEEDED(deviceReason)) {
            // The adapter owns only device/format-scoped objects. Reuse it even when the Streamline proxy uses a
            // different queue, then create the new swapchain RTVs and allocator/fence set without recording or
            // submitting an overlay draw. This completes before slDLSSGSetOptions(ON), so the first generated
            // Present cannot race a backend shutdown/rebuild.
            dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            dx12_hook_g_State.cachedWidth = desc.BufferDesc.Width;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            dx12_hook_g_State.cachedHeight = desc.BufferDesc.Height;
            dx12_hook_g_State.format = desc.BufferDesc.Format;

            const bool backendReady =
                InitImGui(queueDevice, static_cast<int>(desc.BufferCount), desc.BufferDesc.Format, desc.OutputWindow);
            if (backendReady) {
                CreateRTVs(queueDevice, swapChain3, static_cast<int>(desc.BufferCount));
                if (dx12_hook_g_State.rtvDescHeap) {
                    InitOverlaySync(queueDevice, static_cast<int>(desc.BufferCount), swapchainQueue);
                }
            }
            ready = backendReady && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_g_State.rtvDescHeap && dx12_hook_g_State.cmdList &&
                    !dx12_hook_g_State.allocators.empty();
            if (!ready && !dx12_hook_g_State.rtvDescHeap) {
                // Make the normal first-PostSL bootstrap retry the complete swapchain-scoped setup. Preserve the
                // warm adapter if initialization itself succeeded; InitImGui will reuse it on that retry.
                dx12_hook_g_State.overlayInit = false;
                dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            }
            overlayInit = dx12_hook_g_State.overlayInit;
            syncInit = dx12_hook_g_State.syncInit;
            rtvHeap = dx12_hook_g_State.rtvDescHeap;
        } else {
            HookLogImportant(
                "DX12: PostSL handoff prewarm refused removed device "
                "(source=%s device=%p hr=0x%08X)",
                context ? context : "unknown", queueDevice, (unsigned)deviceReason);
        }
    }

    HookLogImportant(
        "DX12: PostSL handoff prewarm %s before DLSS enable "
        "(source=%s sc=%p queue=%p device=%p fmt=%d buffers=%u elapsed=%llums init=%d sync=%d rtv=%p)",
        ready ? "READY" : "INCOMPLETE", context ? context : "unknown", swapChain, swapchainQueue, queueDevice,
        static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
        static_cast<unsigned long long>(GetTickCount64() - startedMs), overlayInit ? 1 : 0, syncInit ? 1 : 0, rtvHeap);

    swapChain3->Release();
    queueDevice->Release();
    return ready;
}

// --- CPU Prerender Limit Support (DX12) ---
inline void ApplyPrerenderLimitDX12(float limit, bool frameGenerationPresentationActive) {
    if (limit < 0.0f)
        return;
    // FG runtimes can replace the observed ECL queue with their own internal
    // presentation queue. Keep the limiter on the retained application queue
    // so its fence stream cannot become part of a runtime Present dependency.
    bool usesOriginalGameQueue = false;
    ID3D12CommandQueue* currentQueueSnapshot = nullptr;
    DX12Context ctx = GetDX12PrerenderContext(frameGenerationPresentationActive, &usesOriginalGameQueue,
                                              &currentQueueSnapshot);
    if (!ctx.IsValid())
        return;

    std::lock_guard<std::mutex> lock(dx12_hook_g_PrerenderMutex);

    if (dx12_hook_g_PrerenderDevice != ctx.device || dx12_hook_g_PrerenderQueue != ctx.queue) {
        for (auto* fence : dx12_hook_g_PrerenderFences) {
            if (fence)
                fence->Release();
        }
        dx12_hook_g_PrerenderFences.clear();
        for (HANDLE event : dx12_hook_g_PrerenderEvents) {
            if (event)
                CloseHandle(event);
        }
        dx12_hook_g_PrerenderEvents.clear();
        dx12_hook_g_PrerenderFrameIndex = 0;
        if (dx12_hook_g_PrerenderDevice)
            dx12_hook_g_PrerenderDevice->Release();
        if (dx12_hook_g_PrerenderQueue)
            dx12_hook_g_PrerenderQueue->Release();
        dx12_hook_g_PrerenderDevice = ctx.device;
        dx12_hook_g_PrerenderQueue = ctx.queue;
        dx12_hook_g_PrerenderDevice->AddRef();
        dx12_hook_g_PrerenderQueue->AddRef();
        HookLogImportant(
            "DX12: Prerender fence stream rebound device=%p queue=%p role=%s currentQueue=%p",
            ctx.device, ctx.queue, usesOriginalGameQueue ? "original-game" : "current-fallback",
            currentQueueSnapshot);
    }

    // Initialize fence ring buffer if needed
    if (dx12_hook_g_PrerenderFences.empty()) {
        for (int i = 0; i < 16; i++) {
            ID3D12Fence* fence = nullptr;
            HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                dx12_hook_g_PrerenderFences.push_back(fence);
                dx12_hook_g_PrerenderEvents.push_back(event);
            } else if (event) {
                CloseHandle(event);
            }
        }
        HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)dx12_hook_g_PrerenderFences.size());
    }

    if (dx12_hook_g_PrerenderFences.empty())
        return;

    static std::atomic<int> s_prerenderWarnLogs{0};
    auto waitForFence = [&](ID3D12Fence* fenceToWait, HANDLE waitEvent, uint64_t waitValue) -> bool {
        if (!fenceToWait || !waitEvent)
            return false;
        if (fenceToWait->GetCompletedValue() >= waitValue)
            return true;

        HRESULT setHr = fenceToWait->SetEventOnCompletion(waitValue, waitEvent);
        if (FAILED(setHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
            }
            return false;
        }

        DWORD waitResult = WaitForSingleObject(waitEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
            return true;

        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender wait failed result=%lu error=%lu value=%llu", waitResult, GetLastError(),
                    waitValue);
        }
        return false;
    };

    size_t idx = dx12_hook_g_PrerenderFrameIndex % dx12_hook_g_PrerenderFences.size();
    ID3D12Fence* fence = dx12_hook_g_PrerenderFences[idx];
    HANDLE event = dx12_hook_g_PrerenderEvents[idx];

    if (limit == 0.0f) {
        // Strict Serial: Signal and immediately wait
        uint64_t value = dx12_hook_g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, value);
        if (SUCCEEDED(signalHr)) {
            waitForFence(fence, event, value);
        } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
        }
    } else {
        const int lookback = std::clamp(static_cast<int>(limit), 1, 6);

        // Signal current frame
        uint64_t signalValue = dx12_hook_g_PrerenderFrameIndex + 1;
        HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
        if (FAILED(signalHr)) {
            if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
                HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
            }
            dx12_hook_g_PrerenderFrameIndex++;
            return;
        }

        // Wait on N frames ago
        if (dx12_hook_g_PrerenderFrameIndex >= (uint64_t)lookback) {
            size_t waitIdx = (dx12_hook_g_PrerenderFrameIndex - lookback) % dx12_hook_g_PrerenderFences.size();
            ID3D12Fence* waitFence = dx12_hook_g_PrerenderFences[waitIdx];
            HANDLE waitEvent = dx12_hook_g_PrerenderEvents[waitIdx];
            uint64_t waitValue = (dx12_hook_g_PrerenderFrameIndex - lookback) + 1;

            if (waitFence->GetCompletedValue() < waitValue) {
                waitForFence(waitFence, waitEvent, waitValue);
            }
        }
    }

    dx12_hook_g_PrerenderFrameIndex++;
}

// ============================================================================
// Post-SL FG overlay renderer.
//
// Called from the RE-ENTRANT Present path (dxgi_shared.cpp) — i.e. AFTER
// Streamline's FG pipeline has finished generating/presenting its frames.
// By rendering here we avoid submitting extra ECLs before SL sees Present,
// which is what caused DXGI_ERROR_DEVICE_REMOVED with every previous approach.
//
// Flow:
//   1. Game calls Present → our DetourPresent → ProcessFrame (skips overlay
//      draw because SL FG is active) → calls oPresent (enters SL via E9 JMP)
//   2. SL processes FG → for each output frame SL calls Present via vtable
//   3. Re-entrant DetourPresent → g_PostSLOverlayRenderCallback → THIS function
//   4. We render overlay on the current backbuffer → bypass trampoline → real DXGI Present
//
// This matches the standard inject-overlay strategy: overlay is drawn after FG, before the real Present.
//
// KEY DESIGN DECISIONS (confirmed by diagnostics):
//
// 1. DIRECT QUEUE SUBMISSION: We submit ECL via g_RealD3D12ECL(g_RealQueueBehindSLWrapper)
//    instead of slQueue->ExecuteCommandLists().  SL's COM wrapper adds internal
//    metadata per ECL that accumulates and causes DEVICE_REMOVED after ~500-2000
//    frames.  Direct submission bypasses this — proven stable 16,798+ frames.
//
// 2. UAV BARRIERS (not state transitions): We use UAV barriers (global GPU flush)
//    instead of PRESENT→RT / RT→PRESENT state transitions.  State transition type
//    doesn't affect the cumulative crash (confirmed: all barrier types crash at
//    similar timing through SL's wrapper).  UAV barriers avoid resource state
//    tracking conflicts with SL's internal state management.
//
// 3. CACHED FG STATE: g_StreamlineFGRunning is cached ONCE at function entry
//    into cachedSLFGActive.  Reading it multiple times caused mid-function
//    transition races where barrier/queue selection became inconsistent →
//    instant DEVICE_REMOVED on the first inconsistent frame.
//
// 4. FG DEACTIVATION SUSPEND: When cachedSLFGActive transitions true→false,
//    PostSL suspends permanently (s_postSLFGSuspended=true) until FG reactivates.
//    This prevents using stale queue/state from the FG phase.  Pre-SL path
//    takes over for non-FG rendering.
//
// 5. FG "SUSPENSION" FALLBACK: When g_StreamlineFGRunning stays true but SL stops
//    generating re-entrant Present calls (game menu/pause), PostSL never fires.
//    ProcessFrame detects this via g_PostSLStallCounter and falls back to pre-SL.
//    When PostSL fires again (FG resumes), it resets the counter and takes over.
//
// COMPATIBILITY:
//   - GTA V Enhanced: DLSS FG with SL, menu pauses FG (stall fallback needed)
//   - Talos Principle Reawakened: DLSS FG + FSR FG, continuous rendering
//   - Both need the direct queue bypass to avoid cumulative SL damage
// ============================================================================
inline void PostSLOverlayRender(IDXGISwapChain* pSwapChain) {
    // --- PostSL per-frame statistics (declared early for lock-skip path) ---
    static std::atomic<int> s_postSLCalls{0};
    static std::atomic<int> s_postSLRenders{0};
    static std::atomic<int> s_postSLSkipLock{0};
    static std::atomic<int> s_postSLSkipFence{0};
    static std::atomic<int> s_postSLSkipOther{0};

    // Snapshot before this callback records anything. If the normal path has
    // already drawn since the last presented-frame accounting boundary, the
    // current present is covered and the same-queue startup handoff must not
    // draw a second overlay on top of it.
    const bool normalRouteDrawPendingAtEntry = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire) !=
                                               dx12_hook_g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);

    // THREAD SAFETY: During FG, SL may fire Present from multiple threads.
    // Our rendering resources (allocators, command list, descriptor heap) are NOT
    // thread-safe. Use a try-lock to ensure only one thread renders at a time.
    if (!dx12_hook_g_PostSLRenderMutex.try_lock()) {
        s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-render-lock");
        static int s_lockSkip = 0;
        if (s_lockSkip++ < 10)
            HookLogImportant("DX12: PostSL SKIP — another thread already rendering (tid=0x%04X)", GetCurrentThreadId());
        return;
    }
    // RAII unlock — ensures s_renderLock is released on ALL exit paths
    auto renderLockGuard = ce::make_scope_guard([]() { dx12_hook_g_PostSLRenderMutex.unlock(); });
    const uint32_t entryLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);

    // Cache FG state ONCE at function entry to avoid mid-function transition races.
    // g_StreamlineFGRunning can change between reads if FG transitions during PostSL.
    const bool cachedSLFGActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    constexpr ULONGLONG kDormantProcessFrameThresholdMs = 100;
    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG lastProcessFrameTickMs = dx12_hook_g_LastProcessFrameTickMs.load(std::memory_order_acquire);
    const bool processFrameRecentlySeen = lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs &&
                                          (nowMs - lastProcessFrameTickMs) < kDormantProcessFrameThresholdMs;
    const bool startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const int startupWrapperProgressCount =
        dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire);
    const bool useTopLevelHandoffWrapperProgress =
        ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
            dx12_hook_g_HadFSRFGPhase, startupTopLevelPresentConsumed, startupWrapperProgressCount > 0);
    const bool safePostFSRBootstrapPathForPostSL = HookHasSafePostFSRBootstrapPath();
    // Pure-DLSS engage proof: explicit slDLSSGSetOptions(ON) provenance for the
    // CURRENT comeback + retained startup activation swapchain + installed
    // callback (we are inside one — SL's present pipeline is live). Gates the
    // no-blank engage path for the synthetic-startup countdown and cold-start
    // warmup; GetState-only enables keep both protections.
    const bool explicitEnablePureDLSSColdStartProof = ce::dx12_overlay_policy::HasExplicitEnablePureDLSSColdStartProof(
        dx12_hook_g_HadFSRFGPhase, HookHasExplicitStreamlineSetOptionsActivation(),
        HasRetainedStreamlineStartupActivationSwapchain(),
        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr);

    // --- PostSL periodic stats logging ---
    int callNum = s_postSLCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((callNum % 500) == 0) {
        int renders = s_postSLRenders.load(std::memory_order_relaxed);
        int skipL = s_postSLSkipLock.load(std::memory_order_relaxed);
        int skipF = s_postSLSkipFence.load(std::memory_order_relaxed);
        int skipO = s_postSLSkipOther.load(std::memory_order_relaxed);
        HookLogImportant(
            "DX12: PostSL stats: calls=%d renders=%d skipLock=%d skipFence=%d skipOther=%d (render%%=%.0f%%)", callNum,
            renders, skipL, skipF, skipO, callNum > 0 ? (renders * 100.0 / callNum) : 0.0);
    }

    // Reactivation tracking: log the first N calls after reactivation to diagnose
    // silent early returns.  All early-return paths use HookLog (not in hook_debug.log),
    // so without this, PostSL failures after FG transitions are invisible.
    static int s_reactivationEpoch = 0;
    static int s_callsSinceReactivation = 0;
    static int s_postSLProbeFrames = 0;
    static bool s_wasActive = false;
    static uint32_t s_seenLifecycleEpoch = 0;
    static HANDLE s_dedicatedFenceEvent = nullptr;
    static ID3D12Fence* s_dedicatedSyncFence = nullptr;
    static UINT64 s_dedicatedSyncFenceValue = 0;

    // Streamline signal guard: a real FG shutdown must stop PostSL immediately,
    // but a transient signal drop during reactivation must not permanently strand
    // PostSL in a locally suspended state while synthetic re-entrant Presents are
    // still arriving.
    static bool s_wasSLFGActive = false;
    static bool s_postSLFGSuspended = false;
    const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    if (cachedSLFGActive) {
        s_wasSLFGActive = true;
        s_postSLFGSuspended = false;
    } else if (s_wasSLFGActive) {
        s_wasSLFGActive = false;
        s_postSLFGSuspended = ce::dx12_overlay_policy::ShouldLatchPostSLSuspensionOnStreamlineSignalDrop(
            cachedSLFGActive, postSLActive, postSLConfirmedRendering, startupActivationPending);
        HookLogImportant("DX12: PostSL FG signal dropped — %s (active=%d confirmed=%d startupPending=%d)",
                         s_postSLFGSuspended ? "suspending until clean reactivation"
                                             : "treating as transient and waiting for signal recovery",
                         postSLActive ? 1 : 0, postSLConfirmedRendering ? 1 : 0, startupActivationPending ? 1 : 0);
    }
    // Make-before-break keep-alive: a confirmed PostSL path renders across the
    // explicit OFF edge until the normal route recovers (gates below honor it).
    // It renders exactly what it rendered one present earlier on the same
    // proven queue/swapchain; PostSLOverlayRenderGated retires the latch on
    // normal-route recovery or Streamline unload.
    const bool keepAliveRenderAfterExplicitOff =
        ce::dx12_overlay_policy::ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(
            dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire), cachedSLFGActive, IsStreamlineLoaded());
    const bool exactExplicitOffKeepAliveSwapchain =
        keepAliveRenderAfterExplicitOff && pSwapChain != nullptr &&
        pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if ((!cachedSLFGActive || s_postSLFGSuspended) && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-sl-signal-inactive");
        static int s_suspendLog = 0;
        if (s_suspendLog < 5 || (s_suspendLog % 500 == 0)) {
            HookLog("DX12: PostSL SKIP — Streamline FG signal inactive (latched=%d frame=%d)",
                    s_postSLFGSuspended ? 1 : 0, s_suspendLog);
        }
        s_suspendLog++;
        return;
    }
    if (keepAliveRenderAfterExplicitOff) {
        static std::atomic<int> s_keepAliveRenderLogCount{0};
        const int logCount = s_keepAliveRenderLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL keep-alive render after explicit Streamline OFF #%d (confirmed=%d active=%d)",
                logCount + 1, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
                dx12_hook_g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0);
        }
    }

    // Same-queue pure-DLSS cold start proof (Talos startup: scQueue==origGame, no separate command/SL
    // wrapper queue). When DLSS FG runs on the game's own single queue there is no separate DLSS-G
    // proxy-init pipeline for CE's overlay ECL to corrupt, so the synthetic-startup countdown and the
    // cold-start warmup (the GTA separate-queue init protection) can be bypassed safely — the overlay
    // ECL is the same no-FG-route submit on the game's queue. Re-evaluated every callback so a title
    // that later creates a separate runtime queue (GTA) flips this false and the protections resume.
    bool sameQueuePureDLSSColdStartSafe = false;
    {
        ID3D12CommandQueue* sqScQueue = nullptr;
        ID3D12CommandQueue* sqOrigQueue = nullptr;
        ID3D12CommandQueue* sqCmdQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            sqScQueue = dx12_hook_g_SwapchainQueue;
            sqOrigQueue = dx12_hook_g_OriginalGameQueue;
            sqCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        ID3D12CommandQueue* sqSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
        auto* sqDev = g_Device.load(std::memory_order_acquire);
        const bool sqDeviceRemoved = sqDev && FAILED(sqDev->GetDeviceRemovedReason());
        sameQueuePureDLSSColdStartSafe = ce::dx12_overlay_policy::ShouldTreatSameQueuePureDLSSColdStartAsSafe(
            dx12_hook_g_HadFSRFGPhase, sqScQueue != nullptr && sqScQueue == sqOrigQueue,
            sqCmdQueue == nullptr || sqCmdQueue == sqOrigQueue, sqSLWrapperQueue != nullptr, sqDeviceRemoved);
    }

    bool syntheticStartupActivatedThisCall = false;
    bool immediateSameQueueStartupTakeover = false;
    {
        immediateSameQueueStartupTakeover =
            sameQueuePureDLSSColdStartSafe && processFrameRecentlySeen && startupActivationPending;
        if (ce::dx12_overlay_policy::ShouldSyntheticPostSLAdvanceDormantStartup(
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                cachedSLFGActive, dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire), processFrameRecentlySeen,
                useTopLevelHandoffWrapperProgress, sameQueuePureDLSSColdStartSafe)) {
            if (!dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.exchange(true, std::memory_order_acq_rel)) {
                if (immediateSameQueueStartupTakeover) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup immediate same-queue takeover — callback proves the "
                        "Streamline handoff before the ProcessFrame dormant timer (normalDrawPending=%d "
                        "cooldown=%d)",
                        normalRouteDrawPendingAtEntry ? 1 : 0,
                        dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
                } else {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup takeover — ProcessFrame dormant for %llums (cooldown=%d)",
                        lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs
                            ? (nowMs - lastProcessFrameTickMs)
                            : 0,
                        dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_relaxed));
                }
            }

            int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
            if (cooldownLeft > 0) {
                if (ce::dx12_overlay_policy::ShouldDelaySyntheticPostSLActivationBehindRepeatedCallbacks(
                        dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL)) {
                    dx12_hook_g_PostSLCooldownRemaining.fetch_sub(1, std::memory_order_acq_rel);
                    if (cooldownLeft > 1) {
                        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                        NoteDX12OverlayCoverageGate("postsl-startup-countdown");
                        return;
                    }
                } else if (safePostFSRBootstrapPathForPostSL) {
                    static int s_safePostFSRActivationLogCount = 0;
                    if (s_safePostFSRActivationLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing repeated-callback cooldown after safe post-FSR "
                            "bootstrap proof (cooldown=%d progress=%d)",
                            cooldownLeft, startupWrapperProgressCount);
                    }
                    s_safePostFSRActivationLogCount++;
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (useTopLevelHandoffWrapperProgress) {
                    static int s_wrapperProgressActivationLogCount = 0;
                    if (s_wrapperProgressActivationLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup using wrapper ECL progress after top-level handoff "
                            "(cooldown=%d progress=%d)",
                            cooldownLeft, startupWrapperProgressCount);
                    }
                    s_wrapperProgressActivationLogCount++;
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (explicitEnablePureDLSSColdStartProof) {
                    // Proof-gated no-blank engage: the current comeback was
                    // activated by an explicit slDLSSGSetOptions(ON) edge and
                    // CE retains the runtime-owned startup activation
                    // swapchain, so this callback is a real Streamline-routed
                    // present of the live proxy. Activate from callback #1
                    // instead of blanking through the 8-callback countdown.
                    // GetState-only enables (the historical GTA startup-churn
                    // family) never reach this branch.
                    static int s_explicitEnableCountdownBypassLogCount = 0;
                    if (s_explicitEnableCountdownBypassLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing pure-DLSS countdown after explicit "
                            "slDLSSGSetOptions(ON) proof (cooldown=%d retainedStartupSwapchain=1)",
                            cooldownLeft);
                    }
                    s_explicitEnableCountdownBypassLogCount++;
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else if (sameQueuePureDLSSColdStartSafe) {
                    // Same-queue pure-DLSS cold start (Talos): DLSS FG runs on the game's OWN single
                    // queue (scQueue==origGame, no separate command/SL-wrapper queue), so there is no
                    // separate DLSS-G proxy-init pipeline for CE's ECL to corrupt — activate from
                    // callback #1 instead of blanking through the countdown. The documented GTA hang
                    // family creates a SEPARATE runtime-owned queue during init (this proof is re-checked
                    // every callback and flips false the moment that happens, restoring the countdown).
                    static int s_sameQueueColdStartCountdownBypassLogCount = 0;
                    if (s_sameQueueColdStartCountdownBypassLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup bypassing pure-DLSS countdown — same-queue topology "
                            "(scQueue==origGame, no separate command/SL-wrapper queue): overlay ECL lands on the "
                            "game's own queue, not a separate DLSS-G init pipeline (cooldown=%d)",
                            cooldownLeft);
                    }
                    s_sameQueueColdStartCountdownBypassLogCount++;
                    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                } else {
                    // Pure DLSS cold start without explicit-enable proof: use a
                    // shorter stabilization period instead of bypassing
                    // entirely.  DLSS FG needs a few callbacks to initialize
                    // its internal pipeline (queue setup, mutex state, fence
                    // tracking) before our ECL can safely land on its queue.
                    // Without this, the very first PostSL render can corrupt
                    // DLSS FG state and cause a hang (observed in GTA V
                    // Enhanced with GetState-only activation evidence).
                    constexpr int kPureDLSSMinCooldown = 8;
                    int clamped = std::min(cooldownLeft, kPureDLSSMinCooldown);
                    int remaining = clamped > 0 ? clamped - 1 : 0;
                    dx12_hook_g_PostSLCooldownRemaining.store(remaining, std::memory_order_release);
                    static int s_pureDLSSCooldownLogCount = 0;
                    if (s_pureDLSSCooldownLogCount < 10) {
                        HookLogImportant(
                            "DX12: PostSL synthetic startup reduced cooldown for pure DLSS cold start "
                            "(original=%d clamped=%d remaining=%d)",
                            cooldownLeft, clamped, remaining);
                    }
                    s_pureDLSSCooldownLogCount++;
                    if (clamped > 1) {
                        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                        NoteDX12OverlayCoverageGate("postsl-startup-countdown");
                        return;
                    }
                }
            }

            auto* probeDev = g_Device.load(std::memory_order_acquire);
            const bool startupWindowActiveForProbe = DXGIShared::IsStreamlineStartupTransitionWindowActive();
            if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) && probeDev && IsStreamlineLoaded()) {
                if (!startupWindowActiveForProbe) {
                    ProbeRealD3D12ECL(probeDev);
                    HookLogImportant("DX12: PostSL synthetic startup activation probed realECL=%p",
                                     (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire));
                } else {
                    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    HookLogImportant(
                        "DX12: PostSL synthetic startup activation deferred ECL probe "
                        "(startup window active, will probe after window expires)");
                }
            }

            ID3D12CommandQueue* directQueue = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr directECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            ID3D12CommandQueue* slWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
            if (ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
                    dx12_hook_g_HadFSRFGPhase, directQueue != nullptr, directECL != nullptr, slWrapperQueue != nullptr,
                    safePostFSRBootstrapPathForPostSL)) {
                static int s_waitForSafePathLog = 0;
                if (s_waitForSafePathLog < 10 || (s_waitForSafePathLog % 100) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup waiting for safe bootstrap path after FSR phase "
                        "(realQ=%p realECL=%p slWrapper=%p safeBootstrap=%d)",
                        directQueue, (void*)directECL, slWrapperQueue, safePostFSRBootstrapPathForPostSL ? 1 : 0);
                }
                s_waitForSafePathLog++;
                s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
                NoteDX12OverlayCoverageGate("postsl-wait-safe-bootstrap");
                return;
            }

            const bool enterSyntheticStartupActivation =
                ce::dx12_overlay_policy::ShouldEnterSyntheticPostSLStartupActivation(
                    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                    postSLActiveButUnconfirmed, postSLConfirmedRendering);
            dx12_hook_g_PostSLOverlayActive.store(true, std::memory_order_release);
            dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
            if (enterSyntheticStartupActivation) {
                syntheticStartupActivatedThisCall = true;
                dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(true, std::memory_order_release);
                dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
                DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(false, std::memory_order_release);
                // Startup is still half-armed until the first real PostSL render confirms
                // that the path is actually safe. Activation alone is not enough.
                HookLogImportant("DX12: PostSL synthetic startup activation complete — enabling PostSL rendering");
                ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLActivationComplete,
                                            "DX12::PostSLSyntheticStartupActivation", directQueue, pSwapChain,
                                            g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(),
                                            HookHasExplicitStreamlineSetOptionsActivation());
            } else {
                static int s_repeatSyntheticStartupActivationLog = 0;
                if (s_repeatSyntheticStartupActivationLog < 10 || (s_repeatSyntheticStartupActivationLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL synthetic startup activation already half-armed — preserving warm-up progress "
                        "(pending=%d unconfirmed=%d confirmed=%d repeat=%d)",
                        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(
                            std::memory_order_relaxed)
                            ? 1
                            : 0,
                        postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                        s_repeatSyntheticStartupActivationLog + 1);
                }
                s_repeatSyntheticStartupActivationLog++;
            }
        }
    }

    if (syntheticStartupActivatedThisCall && immediateSameQueueStartupTakeover && normalRouteDrawPendingAtEntry) {
        // The normal route already covered this exact present. Leave PostSL
        // active for the next callback, but do not render twice during the
        // make-before-break boundary. PostSLOverlayRenderGated's scope guard
        // accounts the pending normal draw on return.
        NoteDX12OverlayCoverageGate("postsl-same-queue-make-before-break");
        HookLogImportant(
            "DX12: PostSL immediate same-queue takeover preserved the current normal-route draw — first PostSL "
            "draw moves to the next present (no blank, no double draw)");
        return;
    }

    uint32_t lifecycleEpoch = entryLifecycleEpoch;
    bool lifecycleChanged = lifecycleEpoch != s_seenLifecycleEpoch;
    if (lifecycleChanged) {
        s_wasActive = false;
        s_seenLifecycleEpoch = lifecycleEpoch;
    }

    bool active = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldTreatPostSLAsReactivated(active, s_wasActive, lifecycleChanged)) {
        s_reactivationEpoch++;
        s_callsSinceReactivation = 0;
        s_postSLProbeFrames = 0;  // Reset probe counter for new reactivation
        // Epoch-scoped: a genuine reactivation must re-prove the first ECL is safe before
        // the warmup can be confirmed-bypassed. Cleared here so a confirmed render from a
        // previous epoch can never bypass a real cold-start warmup.
        dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(false, std::memory_order_release);
        const bool previouslyConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        const int previousStableFrameCount = dx12_hook_g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
        const int previousStallCount = dx12_hook_g_PostSLStallCounter.exchange(0, std::memory_order_acq_rel);
        const bool previousRuntimeStateStabilizationLogged =
            dx12_hook_g_PostSLRuntimeStateStabilizationLogged.exchange(false, std::memory_order_acq_rel);
        const bool extendRuntimeStateStabilization =
            ce::dx12_overlay_policy::ShouldExtendConfirmedPostSLRuntimeStateStabilizationAfterReactivation(
                previousStableFrameCount);
        dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(extendRuntimeStateStabilization,
                                                                       std::memory_order_release);
        // Clean up dedicated queue from previous epochs (no longer used — virtual
        // call through SL's COM wrapper is now the primary submission path).
        ClearPostSLQueues("DX12: PostSL reactivation");
        if (s_dedicatedSyncFence) {
            s_dedicatedSyncFence->Release();
            s_dedicatedSyncFence = nullptr;
        }
        if (s_dedicatedFenceEvent) {
            CloseHandle(s_dedicatedFenceEvent);
            s_dedicatedFenceEvent = nullptr;
        }
        s_dedicatedSyncFenceValue = 0;
        if (ce::dx12_overlay_policy::ShouldResetPostSLStartupProgressOnReactivation(
                previouslyConfirmed, previousStableFrameCount, previousStallCount,
                previousRuntimeStateStabilizationLogged)) {
            HookLogImportant(
                "DX12: PostSL reactivation reset confirmed-startup progress "
                "(epoch=%d confirmed=%d stableFrames=%d stallCount=%d stabilizing=%d extendStaleOff=%d)",
                s_reactivationEpoch, previouslyConfirmed ? 1 : 0, previousStableFrameCount, previousStallCount,
                previousRuntimeStateStabilizationLogged ? 1 : 0, extendRuntimeStateStabilization ? 1 : 0);
        }
        if (extendRuntimeStateStabilization) {
            HookLogImportant(
                "DX12: PostSL reactivation extended runtime-state stabilization for churned startup "
                "(epoch=%d previousStableFrames=%d previousStallCount=%d proofThreshold=%d)",
                s_reactivationEpoch, previousStableFrameCount, previousStallCount,
                ce::dx12_overlay_policy::GetConfirmedPostSLWarmupProofFrameThreshold());
        }
        HookLogImportant("DX12: PostSL REACTIVATED (epoch=%d hadFSR=%d origGame=%p)", s_reactivationEpoch,
                         dx12_hook_g_HadFSRFGPhase ? 1 : 0, dx12_hook_g_OriginalGameQueue);
        // Arm the verbose overlay-handoff diagnostic so the next presents log per-present coverage
        // detail. prevRoute distinguishes off->DLSS (prevRoute=normal, native->fresh-proxy — the
        // reported slight-flash case) from FSR->DLSS (prevRoute=post-sl/ffx, warm proxy).
        {
            const uint32_t prevRoute = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
            dx12_hook_g_OverlayHandoffVerbosePrevRoute.store(prevRoute, std::memory_order_relaxed);
            dx12_hook_g_OverlayHandoffVerboseLogPresents.store(16, std::memory_order_relaxed);
            HookLogImportant(
                "[OVERLAY HANDOFF] PostSL reactivation armed verbose window (epoch=%d hadFSR=%d prevRoute=%s "
                "swapchain=%p) — logging the next 16 presents to pinpoint an off->DLSS fresh-proxy overlay flash",
                s_reactivationEpoch, dx12_hook_g_HadFSRFGPhase ? 1 : 0, DX12OverlayRenderRouteName(prevRoute), (void*)pSwapChain);
        }
        // Reset ECL diagnostic counter for fresh diagnostics after transition
        g_PostSLECLDiagCount.store(0, std::memory_order_relaxed);

        // Reset post-FSR probe state for fresh graduated probing
        dx12_hook_g_PostFSRProbeLevel.store(0, std::memory_order_release);
        dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
        dx12_hook_g_PostFSRDescFreeRecreated = false;
    }
    s_wasActive = active;
    s_callsSinceReactivation++;

    // Gate: only render when explicitly enabled (not during cooldown / resize).
    // The make-before-break keep-alive renders regardless: it is the same
    // confirmed path that rendered one present earlier.
    if (!active && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-inactive");
        static int s_gateSkip = 0;
        if (s_gateSkip++ < 5)
            HookLog("DX12: PostSL SKIP — g_PostSLOverlayActive=false");
        return;
    }

    // Secondary gate: don't render during FG transition cooldown.
    // g_PostSLOverlayActive may be stale if set before ProcessFrame disables it.
    int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
    if (cooldownLeft > 0 && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-fg-transition-cooldown");
        static int s_cooldownSkip = 0;
        if (s_cooldownSkip++ < 5)
            HookLog("DX12: PostSL SKIP — FG transition cooldown active (%d frames left)", cooldownLeft);
        return;
    }

    // Post-reactivation warm-up: after FG transition reactivation, skip rendering
    // for the first N frames to let DLSS FG's internal pipeline fully stabilize.
    // Observed: first ECL on origGame queue after FSR→DLSS switch causes
    // DEVICE_REMOVED, even with correct queue and no cross-queue sync.
    // Waiting ~30 frames lets SL's FG pipeline establish its internal state.
    //
    // Cold-start DLSS (epoch 1): a shorter warmup gives DLSS FG time to initialize
    // its internal pipeline (queue setup, mutex state, fence tracking) before our
    // first ECL submission.  Without this, the very first PostSL render can corrupt
    // DLSS FG's state and cause a hang/crash (observed in GTA V Enhanced).
    constexpr int kPostSLReactivationWarmup = 30;
    constexpr int kPostSLColdStartWarmup = 15;
    const int warmupThreshold = (s_reactivationEpoch > 1) ? kPostSLReactivationWarmup : kPostSLColdStartWarmup;
    ID3D12CommandQueue* warmupSwapchainQueue = nullptr;
    ID3D12CommandQueue* warmupLastWorkingQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        warmupSwapchainQueue = dx12_hook_g_SwapchainQueue;
        warmupLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
    }
    const bool confirmedPureStreamlineResumeWarmupProof =
        ce::dx12_overlay_policy::HasConfirmedPureStreamlinePostSLResumeProof(
            dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, startupTopLevelPresentConsumed, warmupLastWorkingQueue != nullptr,
            warmupSwapchainQueue != nullptr,
            warmupLastWorkingQueue != nullptr && warmupSwapchainQueue != nullptr &&
                warmupLastWorkingQueue == warmupSwapchainQueue);
    const bool postSLConfirmedRenderThisEpoch =
        dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.load(std::memory_order_acquire);
    const bool bypassReactivationWarmup = ce::dx12_overlay_policy::ShouldBypassPostSLReactivationWarmup(
        dx12_hook_g_HadFSRFGPhase, useTopLevelHandoffWrapperProgress, safePostFSRBootstrapPathForPostSL,
        confirmedPureStreamlineResumeWarmupProof, explicitEnablePureDLSSColdStartProof, postSLConfirmedRenderThisEpoch,
        sameQueuePureDLSSColdStartSafe);
    if (s_callsSinceReactivation <= warmupThreshold && !bypassReactivationWarmup && !keepAliveRenderAfterExplicitOff) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        NoteDX12OverlayCoverageGate("postsl-reactivation-warmup");
        if (s_callsSinceReactivation <= 5 || s_callsSinceReactivation == warmupThreshold) {
            // Log the proof inputs that resolved bypassReactivationWarmup=false so a
            // true->false flip (e.g. the explicit-enable proof dropping when the retained
            // startup swapchain is released after frame 1) is visible inline on the first
            // skipped frame, without cross-referencing the SUBMIT/release lines.
            HookLogImportant(
                "DX12: PostSL warm-up after reactivation epoch=%d frame=%d/%d (coldStart=%d hadFSR=%d "
                "safeBootstrap=%d confirmedResume=%d explicitEnableColdStart=%d confirmedThisEpoch=%d "
                "retainedSwapchain=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, s_reactivationEpoch <= 1 ? 1 : 0,
                dx12_hook_g_HadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPathForPostSL ? 1 : 0,
                confirmedPureStreamlineResumeWarmupProof ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0,
                postSLConfirmedRenderThisEpoch ? 1 : 0, HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0);
        }
        return;
    }
    if (s_callsSinceReactivation <= warmupThreshold && bypassReactivationWarmup) {
        static int s_bypassWarmupLogCount = 0;
        if (s_bypassWarmupLogCount < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing reactivation warm-up after safe startup proof "
                "(epoch=%d frame=%d/%d hadFSR=%d wrapperProgress=%d safeBootstrap=%d confirmedResume=%d "
                "explicitEnableColdStart=%d confirmedThisEpoch=%d sameQueueColdStart=%d scQ=%p lastWorkingQ=%p)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, dx12_hook_g_HadFSRFGPhase ? 1 : 0,
                useTopLevelHandoffWrapperProgress ? 1 : 0, safePostFSRBootstrapPathForPostSL ? 1 : 0,
                confirmedPureStreamlineResumeWarmupProof ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0,
                postSLConfirmedRenderThisEpoch ? 1 : 0, sameQueuePureDLSSColdStartSafe ? 1 : 0, warmupSwapchainQueue,
                warmupLastWorkingQueue);
        }
        s_bypassWarmupLogCount++;
    }

    // DEBUG: Log when warmup completes and we're about to proceed to actual rendering
    if (s_callsSinceReactivation == warmupThreshold + 1 ||
        (bypassReactivationWarmup && s_callsSinceReactivation == 1)) {
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        HookLogImportant(
            "DX12: PostSL WARMUP COMPLETE — proceeding to render submission "
            "(epoch=%d warmupFrames=%d confirmed=%d startupWindowActive=%d overlayInit=%d syncInit=%d "
            "swapchain=%p dev=%p bypassed=%d)",
            s_reactivationEpoch, warmupThreshold, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0,
            startupWindowActive ? 1 : 0, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0, (void*)pSwapChain,
            (void*)(dx12_hook_g_State.syncDevice ? dx12_hook_g_State.syncDevice : g_Device.load(std::memory_order_acquire)),
            bypassReactivationWarmup ? 1 : 0);
    }

    // Startup transition window rendering gate: while the startup transition
    // window is active, DLSS FG is still initializing its internal pipeline.
    // Submitting ECL on the SL-owned swapchain queue during this phase can
    // corrupt DLSS FG's internal state (mutex tracking, fence state), leading
    // to hangs/crashes.  Wait for the window to expire before submitting GPU work.
    // Once PostSL has confirmed stable rendering (from a previous cycle), this
    // gate no longer applies — the pipeline is proven stable.
    const bool startupTransitionWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLWarmupComplete = bypassReactivationWarmup || s_callsSinceReactivation > warmupThreshold;
    if (startupTransitionWindowActive && !dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) &&
        safePostFSRBootstrapPathForPostSL) {
        static int s_bypassStartupWindowGuardLog = 0;
        if (s_bypassStartupWindowGuardLog < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing startup transition window deferral after safe post-FSR bootstrap proof "
                "(epoch=%d call#=%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_bypassStartupWindowGuardLog++;
    }
    if (startupTransitionWindowActive && !dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) &&
        !safePostFSRBootstrapPathForPostSL && cachedSLFGActive && postSLWarmupComplete) {
        static int s_activeRuntimeStartupWindowGuardLog = 0;
        if (s_activeRuntimeStartupWindowGuardLog < 10) {
            HookLogImportant(
                "DX12: PostSL bypassing startup transition window deferral after active DLSS FG runtime proof "
                "(epoch=%d call#=%d warmup=%d/%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, warmupThreshold, warmupThreshold,
                useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_activeRuntimeStartupWindowGuardLog++;
    }
    if (ce::dx12_overlay_policy::ShouldDeferPostSLRenderingDuringStartupTransitionWindow(
            startupTransitionWindowActive, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
            useTopLevelHandoffWrapperProgress, safePostFSRBootstrapPathForPostSL, cachedSLFGActive,
            postSLWarmupComplete)) {
        s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
        static int s_startupWindowGuardLog = 0;
        if (s_startupWindowGuardLog < 10 || (s_startupWindowGuardLog % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL SKIP — startup transition window active, deferring ECL until DLSS FG stabilizes "
                "(epoch=%d call#=%d activeDLSSSignal=%d warmupComplete=%d safeBootstrap=%d wrapperProgress=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, cachedSLFGActive ? 1 : 0, postSLWarmupComplete ? 1 : 0,
                safePostFSRBootstrapPathForPostSL ? 1 : 0, useTopLevelHandoffWrapperProgress ? 1 : 0);
        }
        s_startupWindowGuardLog++;
        return;
    }

    // Use the sync device (the one that created allocators/cmdList/fence) for all
    // per-frame D3D12 operations.  g_Device may have been updated by the ECL hook
    // to a different device pointer (SL wraps devices), causing cross-device
    // CreateRenderTargetView or descriptor heap access → DEVICE_REMOVED.
    auto* dev = dx12_hook_g_State.syncDevice;
    if (!dev)
        dev = g_Device.load(std::memory_order_acquire);

    if (dev && ce::dx12_overlay_policy::ShouldBootstrapPostSLOverlayState(
                   cachedSLFGActive, active, dx12_hook_g_State.overlayInit, processFrameRecentlySeen, startupActivationPending,
                   postSLActiveButUnconfirmed, postSLConfirmedRendering)) {
        if (!pSwapChain) {
            static int s_nullBootstrapLog = 0;
            if (s_nullBootstrapLog < 5) {
                HookLogImportant("DX12: PostSL bootstrap skipped — pSwapChain is nullptr");
                ++s_nullBootstrapLog;
            }
        } else {
            DXGI_SWAP_CHAIN_DESC bootstrapDesc = {};
            const HRESULT descHr = pSwapChain->GetDesc(&bootstrapDesc);
            if (SUCCEEDED(descHr)) {
                IDXGISwapChain3* bootstrapSc3 = nullptr;
                const HRESULT sc3Hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&bootstrapSc3));
                if (SUCCEEDED(sc3Hr) && bootstrapSc3) {
                    ID3D12CommandQueue* bootstrapScQueue = nullptr;
                    ID3D12CommandQueue* bootstrapCmdQueue = nullptr;
                    ID3D12CommandQueue* bootstrapOrigQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        bootstrapScQueue = dx12_hook_g_SwapchainQueue;
                        bootstrapCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
                        bootstrapOrigQueue = dx12_hook_g_OriginalGameQueue;
                    }

                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    dx12_hook_g_State.cachedWidth = bootstrapDesc.BufferDesc.Width;
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    dx12_hook_g_State.cachedHeight = bootstrapDesc.BufferDesc.Height;
                    dx12_hook_g_State.format = bootstrapDesc.BufferDesc.Format;

                    HookLogImportant(
                        "DX12: PostSL bootstrap — rebuilding torn-down overlay state after dormant reactivation "
                        "(fmt=%d buffers=%u hwnd=%p scQueue=%p cmdQueue=%p origQueue=%p)",
                        (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount, bootstrapDesc.OutputWindow,
                        bootstrapScQueue, bootstrapCmdQueue, bootstrapOrigQueue);
                    // Attribution for the FSR->DLSS-comeback floor gap: when this reactivation
                    // present rebuilds the backend before the first confirmed PostSL draw lands
                    // (the draw covers the NEXT present), the present is uncovered. Label the
                    // coverage gate here so that documented 1-present floor reports
                    // `postsl-bootstrap-reactivation` instead of `unknown` (session
                    // 20260613_211048: the sole gate=unknown streak). Read only if uncovered.
                    NoteDX12OverlayCoverageGate("postsl-bootstrap-reactivation");

                    if (InitImGui(dev, (int)bootstrapDesc.BufferCount, bootstrapDesc.BufferDesc.Format,
                                  bootstrapDesc.OutputWindow)) {
                        int actualBufferCount = (int)bootstrapDesc.BufferCount;
                        if (actualBufferCount > 8) {
                            actualBufferCount = 8;
                        }
                        CreateRTVs(dev, bootstrapSc3, actualBufferCount);

                        ID3D12CommandQueue* bootstrapQueue = bootstrapScQueue;
                        if (!bootstrapQueue) {
                            bootstrapQueue = bootstrapCmdQueue;
                        }
                        if (!bootstrapQueue) {
                            bootstrapQueue = bootstrapOrigQueue;
                        }

                        if (bootstrapQueue && dx12_hook_g_State.rtvDescHeap) {
                            HookLogImportant(
                                "DX12: PostSL bootstrap — inline InitOverlaySync (queue=%p overlayInit=%d syncInit=%d)",
                                bootstrapQueue, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0);
                            InitOverlaySync(dev, (int)bootstrapDesc.BufferCount, bootstrapQueue);
                            dev = dx12_hook_g_State.syncDevice;
                            if (!dev) {
                                dev = g_Device.load(std::memory_order_acquire);
                            }
                        } else {
                            HookLogImportant(
                                "DX12: PostSL bootstrap — waiting for missing init prerequisites (queue=%p rtvHeap=%p)",
                                bootstrapQueue, dx12_hook_g_State.rtvDescHeap);
                        }
                    } else {
                        HookLogImportant("DX12: PostSL bootstrap — InitImGui failed (fmt=%d buffers=%u hwnd=%p)",
                                         (int)bootstrapDesc.BufferDesc.Format, bootstrapDesc.BufferCount,
                                         bootstrapDesc.OutputWindow);
                    }

                    bootstrapSc3->Release();
                } else {
                    HookLogImportant("DX12: PostSL bootstrap — swapchain3 query failed hr=0x%08X", (unsigned)sc3Hr);
                }
            } else {
                HookLogImportant("DX12: PostSL bootstrap — swapchain desc unavailable hr=0x%08X", (unsigned)descHr);
            }
        }
    }

    // After FG type transitions, syncInit is reset to force fresh sync resources.
    // PostSL re-initializes inline with the current queue (scQueue or g_CommandQueue).
    if (dev && dx12_hook_g_State.overlayInit && !dx12_hook_g_State.syncInit) {
        ID3D12CommandQueue* reinitQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            reinitQueue = dx12_hook_g_SwapchainQueue;
            if (!reinitQueue)
                reinitQueue = g_CommandQueue.load(std::memory_order_acquire);
        }
        if (reinitQueue) {
            HookLogImportant("DX12: PostSL triggering inline InitOverlaySync (queue=%p dev=%p)", reinitQueue, dev);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            InitOverlaySync(dev, dx12_hook_g_State.bufferCount, reinitQueue);
            dev = dx12_hook_g_State.syncDevice;
            if (!dev)
                dev = g_Device.load(std::memory_order_acquire);
        }
    }

    if (!dev || !dx12_hook_g_State.overlayInit || !dx12_hook_g_State.syncInit || !dx12_hook_g_State.cmdList || dx12_hook_g_State.allocators.empty()) {
        static int s_stateSkip = 0;
        const int stateSkip = s_stateSkip++;
        if (stateSkip < 5 || s_callsSinceReactivation <= 20) {
            HookLogImportant(
                "DX12: PostSL SKIP — state unavailable (epoch=%d call#=%d dev=%p syncDev=%p init=%d sync=%d "
                "list=%p alloc=%d)",
                s_reactivationEpoch, s_callsSinceReactivation, (void*)g_Device.load(), dev, dx12_hook_g_State.overlayInit ? 1 : 0,
                dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_State.cmdList, (int)dx12_hook_g_State.allocators.size());
        }
        return;
    }

    // Don't render if device is removed
    if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed))
        return;

    HRESULT devReason = dev->GetDeviceRemovedReason();
    if (FAILED(devReason)) {
        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
        HookLogImportant("DX12: PostSLOverlayRender — device removed (0x%08X), disabling", (unsigned)devReason);
        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        return;
    }

    // Don't render if swapchain is being resized
    if (DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
        static int s_scInvalid = 0;
        if (s_scInvalid++ < 5)
            HookLog("DX12: PostSL SKIP — swapchainInvalid=true");
        return;
    }

    // The first DLSS-G input frame may already carry CE's overlay through Streamline's
    // official UIColorAndAlpha tag. That route covers generated output before PostSL can
    // possibly run. It is not proof that the first proxy output reaching PostSL contains
    // the tag, though: cold OFF->DLSS activation can expose that output first. Once a
    // proven safe PostSL callback exists, take over its exact backbuffer immediately and
    // retire the bounded UI handoff. GetState-only activation keeps the conservative tag
    // consumption path because it lacks explicit current-activation provenance.
    const bool officialUiCoverageActive = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
    const bool requireExactPostSLStartupOutputDraw =
        ce::dx12_overlay_policy::ShouldRequireExactPostSLBackbufferDrawForStartup(
            dx12_hook_g_RequireExactPostSLStartupTransportDraw, dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL,
            explicitEnablePureDLSSColdStartProof, officialUiCoverageActive);
    const bool retireOfficialUiCoverageAfterExactDraw = requireExactPostSLStartupOutputDraw && officialUiCoverageActive;
    if (!requireExactPostSLStartupOutputDraw && ce::dx12_streamline_ui_overlay::ConsumePostSLCoverage()) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kStreamlineUI);
        return;
    }
    // After FSR→DLSS: PostSL rendering causes DEVICE_REMOVED. Use graduated
    // probes so we do not jump directly from an empty submit to a full
    // copy-render-copy overlay pass on the first real PostSL frame.

    // Scene transition cooldown: skip overlay during scene loads/transitions
    int cd = dx12_hook_g_SceneTransitionCooldown.load(std::memory_order_acquire);
    if (cd > 0) {
        dx12_hook_g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
        if (cd == 1) {
            ID3D12CommandQueue* resumeQueue = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                resumeQueue = dx12_hook_g_PostSLLastWorkingQueue;
                if (!resumeQueue)
                    resumeQueue = g_CommandQueue.load(std::memory_order_acquire);
                if (!resumeQueue)
                    resumeQueue = dx12_hook_g_SwapchainQueue;
            }
            HookLogImportant(
                "DX12: Post-SL scene transition cooldown complete — resuming overlay "
                "(queue=%p overlayInit=%d syncInit=%d bufCount=%d)",
                resumeQueue, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_State.bufferCount);
        }
        return;
    }

    // Get the submission queue for PostSL overlay.
    //
    // CRITICAL: Lock to the first queue that works and DON'T follow g_CommandQueue
    // changes.  During DLSS FG, SL creates internal FG worker queues and starts
    // calling ECL from them.  Our DetourECL hook updates g_CommandQueue to these
    // new SL queues, but they may be COM wrapper/aggregation objects incompatible
    // with realECL.  The game's original queue (captured at the start of FG) is
    // a real D3D12 queue that works with realECL.
    //
    // Queue selection:
    // 1. exact explicit-OFF keep-alive: g_PostSLLastWorkingQueue (the retained
    //    direct queue which successfully rendered this exact proxy)
    // 2. g_PostSLLockedQueue — the ordinary proven queue for this epoch
    // 3. g_OriginalGameQueue — the game's very first queue (SL synchronizes with it)
    // 4. g_CommandQueue — last resort, may be SL's internal queue during FG
    //
    // After FG transitions (FSR→DLSS), the NVIDIA driver's internal state for
    // existing queues (including origGame) can become corrupted.  On reactivation
    // PostSL queue selection strategy:
    //
    // PREFER scQueue (swapchain queue): SL transitions the backbuffer to PRESENT
    // on scQueue before calling Present.  By submitting our ECL on scQueue too,
    // D3D12 resource state tracking is correct (same queue = serialized execution).
    // PRESENT→RT and RT→PRESENT barriers work reliably because the before-state
    // matches.  A dedicated queue would break state tracking (different queue
    // doesn't know the resource's current state), causing DEVICE_REMOVED even
    // if the queue itself is healthy.
    //
    // For scQueue, use origECL (SL's original ECL captured from the vtable hook)
    // instead of realECL, because scQueue may be an SL COM wrapper object whose
    // memory layout differs from raw D3D12 CCommandQueue.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* scQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        scQueue = dx12_hook_g_SwapchainQueue;

        // Queue selection strategy for PostSL:
        //
        // DLSS FG (no prior FSR FG): use origGame.  It's the swapchain creation
        //   queue with valid NVIDIA driver state and authorized backbuffer access.
        //
        // DLSS FG (after FSR FG was active): prefer the runtime-owned swapchain
        //   queue or a captured direct queue behind SL's wrapper. Keeping PostSL
        //   locked to the wrapper itself can poison long-running FG state and later
        //   crash on teardown, so wrapper use is bootstrap-only at most.
        //
        // Outside SL FG: exact OFF keep-alive lastWorking > locked > scQueue >
        // origGame > preFG > cmdQueue.
        bool slFGNow = cachedSLFGActive;
        // GTA V's DLSS FG activation triggers a heuristic FSR ghost (brief swapchain
        // queue change) that clears within frames.  Setting hadFSR from heuristic forces
        // PostSL onto SL's internal queues which causes DEVICE_HUNG.
        if (ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), false)) {
            if (!dx12_hook_g_HadFSRFGPhase) {
                dx12_hook_g_HadFSRFGPhase = true;
                HookLogImportant("DX12: PostSL — FSR FG history confirmed, origGame driver state may be stale");
            }
        }

        ID3D12CommandQueue* directQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        ID3D12CommandQueue* latestSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* validatedCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ExecuteCommandListsPtr currentRealECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool validatedCommandQueueIsWrapper =
            validatedCommandQueue && validatedCommandQueue != dx12_hook_g_OriginalGameQueue && validatedCommandQueue != scQueue;
        ID3D12CommandQueue* wrapperBootstrapQueue = latestSLWrapperQueue;
        if (ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(
                dx12_hook_g_HadFSRFGPhase, slFGNow, directQueueBehindWrapper != nullptr, validatedCommandQueueIsWrapper,
                scQueue != nullptr && scQueue != dx12_hook_g_OriginalGameQueue,
                HookHasExplicitStreamlineSetOptionsActivation())) {
            wrapperBootstrapQueue = validatedCommandQueue;
        }
        bool hasDirectQueueBehindWrapper = directQueueBehindWrapper != nullptr;
        const bool hasRuntimeOwnedSwapchainQueue = scQueue != nullptr && scQueue != dx12_hook_g_OriginalGameQueue;
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
        bool preferRealQueueBehindWrapper = ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(
            dx12_hook_g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper);
        const bool preferValidatedDirectQueueForLock =
            ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(dx12_hook_g_HadFSRFGPhase, slFGNow,
                                                                                    hasDirectQueueBehindWrapper);
        bool allowWrapperBootstrapQueue = ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(
            dx12_hook_g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper, wrapperBootstrapQueue != nullptr,
            hasRuntimeOwnedSwapchainQueue, explicitSetOptionsActivation, safePostFSRBootstrapPath);
        const bool resumeOnValidatedLastWorkingQueue = ce::dx12_overlay_policy::
            ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
                dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire),
                dx12_hook_g_PostSLLastWorkingQueue != nullptr, scQueue != nullptr, explicitSetOptionsActivation,
                safePostFSRBootstrapPath);
        const bool lockedQueueIsSLWrapper =
            dx12_hook_g_PostSLLockedQueue && dx12_hook_g_PostSLLockedQueue != dx12_hook_g_OriginalGameQueue && dx12_hook_g_PostSLLockedQueue != scQueue;
        ExecuteCommandListsPtr scQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
        const bool hasSwapchainQueueSubmitPath = scQueue && (scQueueOrigECL != nullptr || currentRealECL != nullptr);
        const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
        const bool selectDirectQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
                dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper,
                hasDirectQueueBehindWrapper);
        const bool selectSwapchainQueueInsteadOfLockedWrapper =
            ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper, scQueue != nullptr,
                scQueue != dx12_hook_g_OriginalGameQueue, hasSwapchainQueueSubmitPath, hasWrapperDerivedDirectPath);

        if (preferValidatedDirectQueueForLock && directQueueBehindWrapper) {
            queue = directQueueBehindWrapper;
            static int s_directQueuePreferredLog = 0;
            if (s_directQueuePreferredLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — validated direct queue %p preferred over scQueue %p after FSR",
                    queue, scQueue);
            }
        } else if (selectDirectQueueInsteadOfLockedWrapper) {
            queue = directQueueBehindWrapper;
            static int s_promoteSelectionLog = 0;
            if (s_promoteSelectionLog++ < 5) {
                HookLog("DX12: PostSL queue candidate — direct real queue %p replacing locked wrapper %p", queue,
                        dx12_hook_g_PostSLLockedQueue);
            }
        } else if (selectSwapchainQueueInsteadOfLockedWrapper) {
            queue = scQueue;
            static int s_swapchainSelectionLog = 0;
            if (s_swapchainSelectionLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue candidate — swapchain queue %p replacing locked wrapper %p after FSR", queue,
                    dx12_hook_g_PostSLLockedQueue);
            }
        } else if (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                       keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                       dx12_hook_g_PostSLLastWorkingQueue != nullptr)) {
            queue = dx12_hook_g_PostSLLastWorkingQueue;
            static std::atomic<int> s_exactOffKeepAliveLastWorkingQueueLogCount{0};
            const int logCount = s_exactOffKeepAliveLastWorkingQueueLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: PostSL exact-proxy explicit-OFF keep-alive selecting last successful direct queue %p "
                    "ahead of locked queue %p (sc=%p log=%d)",
                    queue, dx12_hook_g_PostSLLockedQueue, pSwapChain, logCount + 1);
            }
        } else if (dx12_hook_g_PostSLLockedQueue) {
            queue = dx12_hook_g_PostSLLockedQueue;
        } else if (resumeOnValidatedLastWorkingQueue) {
            queue = dx12_hook_g_PostSLLastWorkingQueue;
            static int s_postFSRResumeQueueLog = 0;
            if (s_postFSRResumeQueueLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue — reusing validated lastWorking queue %p for resumed DLSS activation during "
                    "post-FSR inactive recovery (origGame=%p explicit=%d safeBootstrap=%d)",
                    queue, dx12_hook_g_OriginalGameQueue, explicitSetOptionsActivation ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0);
            }
        } else if (slFGNow) {
            if (preferRealQueueBehindWrapper) {
                queue = directQueueBehindWrapper;
                static int s_realQueueLog = 0;
                if (s_realQueueLog++ < 5) {
                    HookLog("DX12: PostSL queue — realQueueBehindWrapper %p (scQueue=%p hadFSR=%d)", queue, scQueue,
                            dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (allowWrapperBootstrapQueue && wrapperBootstrapQueue &&
                       wrapperBootstrapQueue != dx12_hook_g_OriginalGameQueue && wrapperBootstrapQueue != scQueue) {
                queue = wrapperBootstrapQueue;
                static int s_wrapperBootstrapLog = 0;
                if (s_wrapperBootstrapLog++ < 10) {
                    HookLogImportant(
                        "DX12: PostSL queue — wrapper bootstrap %p (validatedCmdQ=%p latestWrapper=%p scQueue=%p "
                        "hadFSR=%d)",
                        queue, validatedCommandQueue, latestSLWrapperQueue, scQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (scQueue && scQueue != dx12_hook_g_OriginalGameQueue) {
                if (dx12_hook_g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL queue — WARNING: falling back to scQueue %p in post-FSR DLSS path "
                        "(origGame=%p, hadFSR=%d, no wrapper/direct queue available)",
                        scQueue, dx12_hook_g_OriginalGameQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                }
                queue = scQueue;
                static int s_scQLog = 0;
                if (s_scQLog++ < 5)
                    HookLog("DX12: PostSL queue — scQueue %p (SL swapchain, origGame=%p, hadFSR=%d)", queue,
                            dx12_hook_g_OriginalGameQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            } else if (dx12_hook_g_OriginalGameQueue) {
                queue = dx12_hook_g_OriginalGameQueue;
                static int s_origLog = 0;
                if (s_origLog++ < 5)
                    HookLog("DX12: PostSL queue — origGame %p (slFG, hadFSR=%d)", queue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (scQueue) {
            queue = scQueue;
        } else if (dx12_hook_g_OriginalGameQueue) {
            queue = dx12_hook_g_OriginalGameQueue;
        } else if (dx12_hook_g_PreFGGameQueue) {
            queue = dx12_hook_g_PreFGGameQueue;
        } else {
            queue = g_CommandQueue.load(std::memory_order_acquire);
        }

        // AddRef the selected queue under the mutex to prevent it from being
        // freed by DX12_SetCommandQueue (which also uses this mutex) or SL's
        // internal cleanup while we use it.  Released by scope guard below.
        if (queue)
            queue->AddRef();
    }
    // Scope guard ensures Release on all exit paths
    auto queueReleaseGuard = ce::make_scope_guard([&]() {
        if (queue)
            queue->Release();
    });

    if (!queue) {
        static int s_noQueue = 0;
        if (s_noQueue++ < 5)
            HookLog("DX12: PostSL SKIP — no queue (cmdQueue=%p scQueue=%p)", (void*)g_CommandQueue.load(),
                    dx12_hook_g_SwapchainQueue);
        return;
    }

    // Lock to the selected queue for the current epoch, but allow a one-time
    // post-FSR migration from the wrapper bootstrap queue to the captured real
    // queue behind it once the ECL detour has observed that path.
    {
        ID3D12CommandQueue* oldLockedQueue = nullptr;
        bool lockedQueueWasUpdated = false;
        bool shouldKeepExistingLockedQueue = false;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            ID3D12CommandQueue* directQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
            ExecuteCommandListsPtr currentRealECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            ExecuteCommandListsPtr lockedScQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
            const bool lockedQueueIsSLWrapper =
                dx12_hook_g_PostSLLockedQueue && dx12_hook_g_PostSLLockedQueue != dx12_hook_g_OriginalGameQueue && dx12_hook_g_PostSLLockedQueue != scQueue;
            const bool hasSwapchainQueueSubmitPath =
                scQueue && (lockedScQueueOrigECL != nullptr || currentRealECL != nullptr);
            const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
            bool shouldReplaceLockedQueue =
                ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(
                    dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper, directQueueBehindWrapper != nullptr) &&
                queue == directQueueBehindWrapper;
            shouldReplaceLockedQueue =
                shouldReplaceLockedQueue ||
                (ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                     dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper,
                     scQueue != nullptr, scQueue != dx12_hook_g_OriginalGameQueue, hasSwapchainQueueSubmitPath,
                     hasWrapperDerivedDirectPath) &&
                 queue == scQueue);
            shouldReplaceLockedQueue =
                shouldReplaceLockedQueue ||
                (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                     keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                     dx12_hook_g_PostSLLastWorkingQueue != nullptr) &&
                 queue == dx12_hook_g_PostSLLastWorkingQueue);
            const bool selectedQueueMatchesLockedQueue = queue == dx12_hook_g_PostSLLockedQueue;

            if (ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(
                    dx12_hook_g_PostSLLockedQueue != nullptr, selectedQueueMatchesLockedQueue, shouldReplaceLockedQueue)) {
                oldLockedQueue = dx12_hook_g_PostSLLockedQueue;
                dx12_hook_g_PostSLLockedQueue = queue;
                queue->AddRef();  // prevent locked queue from being freed between PostSL calls
                lockedQueueWasUpdated = true;

                if (oldLockedQueue) {
                    if (queue == directQueueBehindWrapper) {
                        HookLogImportant(
                            "DX12: PostSL promoting locked queue %p -> real queue behind wrapper %p after post-FSR "
                            "bootstrap",
                            oldLockedQueue, directQueueBehindWrapper);
                    } else if (exactExplicitOffKeepAliveSwapchain && queue == dx12_hook_g_PostSLLastWorkingQueue) {
                        HookLogImportant(
                            "DX12: PostSL replacing stale locked queue %p -> retained exact-proxy queue %p for "
                            "explicit-OFF keep-alive",
                            oldLockedQueue, queue);
                    } else {
                        HookLogImportant(
                            "DX12: PostSL replacing locked queue %p -> swapchain queue %p after post-FSR direct path "
                            "recovery",
                            oldLockedQueue, queue);
                    }
                } else {
                    bool usingSLWrapper = (queue != dx12_hook_g_OriginalGameQueue && queue != scQueue);
                    bool slFGAtLock = cachedSLFGActive;
                    HookLogImportant(
                        "DX12: PostSL locked to queue %p (origGame=%p scQueue=%p cmdQueue=%p preFG=%p epoch=%d "
                        "slWrapper=%d slFG=%d hadFSR=%d)",
                        queue, dx12_hook_g_OriginalGameQueue, scQueue, (void*)g_CommandQueue.load(), dx12_hook_g_PreFGGameQueue,
                        s_reactivationEpoch, usingSLWrapper ? 1 : 0, slFGAtLock ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
                }
            } else if (!selectedQueueMatchesLockedQueue) {
                shouldKeepExistingLockedQueue = true;
                queue->Release();  // Release per-call AddRef on the rejected queue
                queue = dx12_hook_g_PostSLLockedQueue;
                if (queue) {
                    queue->AddRef();  // Per-call AddRef on the locked queue instead
                }
            }
        }


        if (oldLockedQueue) {
            oldLockedQueue->Release();
        }

        if (shouldKeepExistingLockedQueue && queue) {
            ID3D12CommandQueue* newCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
            HookLogImportant("DX12: PostSL REFUSING queue change: locked=%p, cmdQueue=%p (changed!), scQueue=%p", queue,
                             newCmdQueue, scQueue);
        }

        if (!queue) {
            static int s_missingLockedQueue = 0;
            if (s_missingLockedQueue++ < 5) {
                HookLogImportant("DX12: PostSL SKIP — locked queue disappeared during synchronized selection");
            }
            return;
        }
    }

    // CRITICAL: Verify device compatibility before using sync resources.
    // After swapchain recreation (e.g. FSR→DLSS switch), the submission queue may
    // belong to a different D3D12 device than the one used to create allocators,
    // command list, and fence in InitOverlaySync.  Cross-device ECL submission
    // causes DEVICE_REMOVED.  Detect this and force full re-initialization.
    if (dx12_hook_g_State.syncDevice) {
        // Belt-and-suspenders: verify queue vtable is intact before virtual call.
        // The AddRef above should keep the queue alive, but if something else
        // (SL internal cleanup) bypassed COM refcounting, the vtable may be gone.
        void* vtbl = *reinterpret_cast<void* volatile*>(queue);
        if (!vtbl) {
            HookLogImportant("DX12: PostSL SKIP — queue %p has null vtable (freed?), clearing lock", queue);
            ClearPostSLQueues("DX12: PostSL null vtable");
            return;
        }
        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
            if (queueDevice != dx12_hook_g_State.syncDevice) {
                HookLogImportant(
                    "DX12: PostSL DEVICE MISMATCH! queue=%p queueDev=%p != syncDev=%p — "
                    "forcing overlay re-init to prevent cross-device DEVICE_REMOVED",
                    queue, queueDevice, dx12_hook_g_State.syncDevice);
                queueDevice->Release();
                // Force full re-initialization on next ProcessFrame
                dx12_hook_g_State.overlayInit = false;
                dx12_hook_g_State.syncInit = false;
                dx12_hook_g_State.syncDevice = nullptr;
                ClearPostSLQueues("DX12: PostSL device mismatch");
                ClearPostSLPinnedSLWrapperQueue("DX12: PostSL device mismatch");
                SetPostSLLastWorkingQueue(nullptr);  // Cross-device — old queue invalid
                return;
            }
            queueDevice->Release();
        }
    }

    // Get current backbuffer from the re-entrant swapchain
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: QI for IDXGISwapChain3 failed (call#%d)",
                             s_callsSinceReactivation);
        return;
    }

    UINT bufIdx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    HRESULT getBufHr = sc3->GetBuffer(bufIdx, IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(getBufHr) || !bb) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: GetBuffer(%u) failed hr=0x%08X (call#%d)", bufIdx, getBufHr,
                             s_callsSinceReactivation);
        return;
    }

    // Validate buffer index against current overlay state.
    // After FG mode switches, SL may create a new swapchain with more buffers
    // (e.g., 3→4 for DLSS FG triple buffering).  g_State.bufferCount reflects
    // the count at init time and may be stale.  Dynamically expand to match.
    if (bufIdx >= (UINT)dx12_hook_g_State.bufferCount) {
        if (bufIdx < 8) {
            int newCount = (int)bufIdx + 1;
            HookLogImportant("DX12: PostSL expanding bufferCount %d -> %d (bufIdx=%u from swapchain)",
                             dx12_hook_g_State.bufferCount, newCount, bufIdx);
            dx12_hook_g_State.bufferCount = newCount;
        } else {
            if (s_callsSinceReactivation <= 20)
                HookLogImportant("DX12: PostSL EARLY-EXIT: bufIdx=%u too large (>8) (call#%d)", bufIdx,
                                 s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    // Pick an allocator from the pool. Preserve round-robin locality, but scan the whole pool for a completed
    // slot before considering a Present-thread wait. At high generated-frame rates the preferred slot can
    // still be busy while a later slot is free; waiting in that case creates avoidable interval variance.
    int allocPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
    if (allocPoolSize <= 0) {
        bb->Release();
        return;
    }
    const int preferredIdx = dx12_hook_g_State.allocIndex % allocPoolSize;
    const UINT64 completedFenceValue = dx12_hook_g_State.fence ? dx12_hook_g_State.fence->GetCompletedValue() : UINT64_MAX;
    int idx = ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(dx12_hook_g_State.fenceValues.data(), allocPoolSize,
                                                                       preferredIdx, completedFenceValue);
    if (idx < 0) {
        idx = preferredIdx;
    }
    dx12_hook_g_State.allocIndex = (idx + 1) % allocPoolSize;

    auto* list = dx12_hook_g_State.cmdList;
    auto* alloc = (idx < allocPoolSize) ? dx12_hook_g_State.allocators[idx] : nullptr;
    if (!list || !alloc) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list=%p alloc=%p (idx=%d poolSize=%d call#%d)", list, alloc, idx,
                             allocPoolSize, s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Fence check: ensure allocator's GPU work is complete before reset. This is now exceptional: the pool-wide
    // scan above reaches here with an in-flight allocator only when every allocator is still busy.
    // Uses event-based wait (SetEventOnCompletion + WaitForSingleObject) instead
    // of instant bail — at 100% GPU load, the allocator may be just microseconds
    // from completing, and a skip causes visible overlay flicker.  Event-based
    // wait has zero CPU overhead (thread sleeps until GPU signals) with a 1ms
    // timeout cap to avoid blocking the game.
    if (dx12_hook_g_State.fence && idx < (int)dx12_hook_g_State.fenceValues.size() && dx12_hook_g_State.fenceValues[idx] > 0) {
        UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
        if (completed < dx12_hook_g_State.fenceValues[idx]) {
            // Reusable event handle — created once, persists for the DLL lifetime
            static HANDLE s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool fenceReady = false;
            if (s_fenceEvent) {
                HRESULT evHr = dx12_hook_g_State.fence->SetEventOnCompletion(dx12_hook_g_State.fenceValues[idx], s_fenceEvent);
                if (SUCCEEDED(evHr)) {
                    DWORD waitResult = WaitForSingleObject(s_fenceEvent, 1);  // 1ms max
                    completed = dx12_hook_g_State.fence->GetCompletedValue();
                    fenceReady = (completed >= dx12_hook_g_State.fenceValues[idx]);

                    static int s_fenceWaitLog = 0;
                    if (fenceReady && s_fenceWaitLog++ < 10)
                        HookLogImportant(
                            "DX12: PostSL fence wait resolved via event (alloc[%d] completed=%llu needed=%llu "
                            "waitResult=%lu)",
                            idx, completed, dx12_hook_g_State.fenceValues[idx], waitResult);
                }
            }
            if (!fenceReady) {
                s_postSLSkipFence.fetch_add(1, std::memory_order_relaxed);
                if (s_callsSinceReactivation <= 20)
                    HookLogImportant(
                        "DX12: PostSL EARLY-EXIT: alloc[%d] in-flight after 1ms wait (completed=%llu needed=%llu "
                        "call#%d)",
                        idx, completed, dx12_hook_g_State.fenceValues[idx], s_callsSinceReactivation);
                bb->Release();
                return;
            }
        }
    }

    HRESULT allocResetHr = alloc->Reset();
    if (FAILED(allocResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: alloc->Reset failed hr=0x%08X (call#%d)", allocResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }
    HRESULT listResetHr = list->Reset(alloc, nullptr);
    if (FAILED(listResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list->Reset failed hr=0x%08X (call#%d)", listResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g. SL's
    // FG re-init failed), bail early instead of causing a cascade crash.
    {
        HRESULT preDevHr = dev->GetDeviceRemovedReason();
        if (FAILED(preDevHr)) {
            HookLogImportant(
                "DX12: PostSL EARLY-EXIT: device already removed BEFORE submit "
                "(hr=0x%08X epoch=%d call#%d)",
                preDevHr, s_reactivationEpoch, s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    bool rendered = false;

    if (s_callsSinceReactivation <= 1 || s_postSLRenders.load(std::memory_order_relaxed) == 0) {
        HookLogImportant(
            "DX12: PostSL first ECL submit approaching (epoch=%d call#=%d queue=%p slFG=%d "
            "runtimeMode=%d hadFSR=%d)",
            s_reactivationEpoch, s_callsSinceReactivation, queue, cachedSLFGActive ? 1 : 0,
            (int)g_FGCompat.GetRuntimeMode(), dx12_hook_g_HadFSRFGPhase ? 1 : 0);
    }

    const bool selectedQueueIsSwapchainQueue = (queue == scQueue);
    // Fast post-FSR DLSS probe: when the safe-bootstrap proof holds and the
    // overlay submits on the runtime-owned swapchain queue (not the documented
    // origGame first-ECL crash case), one scratch-barrier health frame replaces
    // the full ~4-frame graduated probe so the DLSS-engage overlay seam drops
    // to a single present. Unproven/off-swapchain-queue paths keep the full probe.
    const bool fastPostFSRDLSSProbe = ce::dx12_overlay_policy::ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(
        dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL, selectedQueueIsSwapchainQueue, cachedSLFGActive);
    const int postFSRProbeFramesPerLevel = fastPostFSRDLSSProbe ? 1 : dx12_hook_kPostFSRProbeFramesPerLevel;
    ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    ID3D12CommandQueue* realQ = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
    ExecuteCommandListsPtr selectedQueueOrigECL = GetOriginalExecuteCommandLists(queue);
    const bool selectedQueueOrigECLMatchesRealECL = selectedQueueOrigECL && selectedQueueOrigECL == realECL;
    bool isSLWrapperQ = ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(
        queue == dx12_hook_g_OriginalGameQueue, queue == dx12_hook_g_PostSLDedicatedQueue, selectedQueueIsSwapchainQueue,
        selectedQueueOrigECLMatchesRealECL);
    const bool useExplicitPostFSRSwapchainTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
            dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool usePostSLOffscreenComposite = ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(
        dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool useExplicitPostFSRBackbufferCopyTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
            usePostSLOffscreenComposite, useExplicitPostFSRSwapchainTransitions);
    const bool hasSelectedQueueSubmitPath = selectedQueueOrigECL != nullptr || realECL != nullptr;
    const bool hasWrapperDerivedDirectPath = realQ != nullptr && realECL != nullptr;
    const bool preferSelectedSwapchainQueueSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(
            dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, isSLWrapperQ, hasSelectedQueueSubmitPath,
            hasWrapperDerivedDirectPath);
    const bool preferSelectedQueueDirectSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(
            dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
            selectedQueueOrigECLMatchesRealECL, realQ != nullptr);

    // Zero-frame post-FSR DLSS reactivation: when the fast-bootstrap proof holds and we submit on the
    // SL-owned swapchain queue, the real overlay render is itself the device-health proof (the
    // pre-submit GetDeviceRemovedReason bail above + the post-submit device-removed check), so the
    // separate scratch-barrier probe present is redundant and only costs the documented 1-present
    // `postsl-bootstrap-reactivation` flicker on every DLSS engage. Skip straight to full-render level
    // and draw the overlay directly on this first reactivation present. The slower graduated probe is
    // retained for the unproven / off-swapchain-queue fragile paths (fastPostFSRDLSSProbe=false there).
    if (ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(
            fastPostFSRDLSSProbe, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire))) {
        HookLogImportant(
            "DX12: PostSL post-FSR fast bootstrap — rendering overlay directly on first reactivation present "
            "(skipping redundant scratch-barrier probe; render's own pre/post devRemoved check is the health "
            "proof) queue=%p scQueue=%p epoch=%d",
            queue, scQueue, s_reactivationEpoch);
        dx12_hook_g_PostFSRProbeLevel.store(3, std::memory_order_release);
        dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
    }

    // --- Post-FSR graduated probing ---
    // Level 0: Scratch resource barrier (confirms queue/device path works)
    // Level 1: Reserved for future backbuffer-specific probes
    // Level 2: Offscreen copy-only pass (touch swapchain only via copy ops)
    // Level 3+: Full offscreen composite/render is allowed
    bool isPostFSRProbe = dx12_hook_g_HadFSRFGPhase && dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3;

    // For post-FSR rendering, use SL's wrapper queue captured from ECL detour.
    // origGame's driver-internal state tracking for FSR-created swapchain backbuffers is
    // invalid (FSR created the swapchain on its own queue, origGame never saw the backbuffers).
    // SL's wrapper queue dispatches through SL's ECL interception which knows the correct state.
    ID3D12CommandQueue* slWrapperQueue = nullptr;
    ID3D12CommandQueue* liveSLWrapperQueue = nullptr;
    bool usingPinnedPostFSRWrapperQueue = false;
    if (dx12_hook_g_HadFSRFGPhase) {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        liveSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);

        ID3D12CommandQueue* pinnedSLWrapperQueue = dx12_hook_g_PostSLPinnedSLWrapperQueue;
        ID3D12CommandQueue* wrapperCandidate = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : liveSLWrapperQueue;
        if (!wrapperCandidate) {
            // Fallback: try g_CommandQueue if it's not origGame or scQueue.
            ID3D12CommandQueue* cmdQ = g_CommandQueue.load(std::memory_order_acquire);
            if (cmdQ && cmdQ != dx12_hook_g_OriginalGameQueue && cmdQ != dx12_hook_g_SwapchainQueue)
                wrapperCandidate = cmdQ;
        }
        if (wrapperCandidate == dx12_hook_g_OriginalGameQueue || wrapperCandidate == dx12_hook_g_SwapchainQueue)
            wrapperCandidate = nullptr;

        if (ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(
                dx12_hook_g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue,
                pinnedSLWrapperQueue != nullptr, wrapperCandidate != nullptr,
                preferSelectedSwapchainQueueSubmitAfterFSR)) {
            wrapperCandidate->AddRef();
            dx12_hook_g_PostSLPinnedSLWrapperQueue = wrapperCandidate;
            pinnedSLWrapperQueue = wrapperCandidate;
            usingPinnedPostFSRWrapperQueue = true;
            HookLogImportant("DX12: PostSL pinned post-FSR SL wrapper queue %p for epoch=%d (source=%s scQueue=%p)",
                             wrapperCandidate, s_reactivationEpoch,
                             liveSLWrapperQueue ? "captured" : "cmdQueue-fallback", scQueue);
        } else {
            usingPinnedPostFSRWrapperQueue = pinnedSLWrapperQueue != nullptr;
        }

        slWrapperQueue = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : wrapperCandidate;
        if (slWrapperQueue)
            slWrapperQueue->AddRef();
    }
    auto slWrapperQueueReleaseGuard = ce::make_scope_guard([&]() {
        if (slWrapperQueue)
            slWrapperQueue->Release();
    });

    if (isPostFSRProbe) {
        // Log comprehensive diagnostics on first probe frame
        if (dx12_hook_g_PostFSRProbeFrames.load(std::memory_order_acquire) == 0 &&
            dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
            D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
            HookLogImportant("DX12: PostSL post-FSR DIAG: pSwapChain=%p bb=%p bufIdx=%u bbW=%u bbH=%u", pSwapChain, bb,
                             bufIdx, (unsigned)bbDesc.Width, bbDesc.Height);
            HookLogImportant("DX12: PostSL post-FSR DIAG: queue=%p origGame=%p slWrapper=%p scQ=%p", queue,
                             dx12_hook_g_OriginalGameQueue, slWrapperQueue, dx12_hook_g_SwapchainQueue);
        }

        bool probeHandled = true;
        const bool preferRealQueueBehindWrapperAfterFSR =
            ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(dx12_hook_g_HadFSRFGPhase, cachedSLFGActive,
                                                                                   realQ != nullptr);
        const bool bootstrapRealQueueCaptureViaWrapperProbe =
            ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
                dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire),
                realQ != nullptr, slWrapperQueue != nullptr, hasSelectedQueueSubmitPath, isSLWrapperQ);
        if (preferRealQueueBehindWrapperAfterFSR && dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
            dx12_hook_g_PostFSRProbeLevel.store(3, std::memory_order_release);
            dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
            HookLogImportant(
                "DX12: PostSL post-FSR switching to direct real queue behind wrapper %p — skipping level 2 probe",
                realQ);
            bb->Release();
            return;
        }
        // CRITICAL: Always use the locked queue (stable across frames) for probe
        // submissions, NOT the transient slWrapperQueue (g_SLWrapperQueue) which
        // changes as different SL wrapper queues are seen by the ECL detour on
        // other threads.  Using a transient wrapper mid-probe causes DEVICE_REMOVED
        // when the new wrapper doesn't own the swapchain's resource state.
        ID3D12CommandQueue* probeQueue = bootstrapRealQueueCaptureViaWrapperProbe
                                             ? queue
                                             : ((dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1 &&
                                                 slWrapperQueue && !preferSelectedSwapchainQueueSubmitAfterFSR)
                                                    ? slWrapperQueue
                                                    : queue);

        if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
            // Probe 0: Scratch resource barrier on origGame — confirms queue works.
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC scratchDesc = {};
            scratchDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            scratchDesc.Width = 64;
            scratchDesc.Height = 64;
            scratchDesc.DepthOrArraySize = 1;
            scratchDesc.MipLevels = 1;
            scratchDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scratchDesc.SampleDesc.Count = 1;
            scratchDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            ID3D12Resource* scratch = nullptr;
            HRESULT scratchHr =
                dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&scratch));
            if (SUCCEEDED(scratchHr) && scratch) {
                D3D12_RESOURCE_BARRIER barriers[2] = {};
                barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[0].Transition.pResource = scratch;
                barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[1].Transition.pResource = scratch;
                barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(2, barriers);
                scratch->Release();
            }
        } else if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1) {
            // Probe 1: PRESENT→RT→PRESENT on backbuffer via SL's wrapper queue.
            // SL's ECL interception dispatches to its internal queue which has correct
            // resource state tracking for the swapchain backbuffers.
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = bb;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = bb;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(2, barriers);
        } else if (ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(
                       dx12_hook_g_HadFSRFGPhase, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire),
                       usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue)) {
            if (!EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
                HookLogImportant(
                    "DX12: PostSL post-FSR copy-only probe could not create offscreen RT (w=%d h=%d fmt=%d)",
                    dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format);
                bb->Release();
                return;
            }

            D3D12_RESOURCE_BARRIER toCopyDest = {};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = dx12_hook_g_State.offscreenRT;
            toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopyDest);

            D3D12_RESOURCE_BARRIER bbToCopySource = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopySource.Transition.pResource = bb;
                bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopySource);
            }

            D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
            bbSrc.pResource = bb;
            bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION offDst = {};
            offDst.pResource = dx12_hook_g_State.offscreenRT;
            offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = dx12_hook_g_State.offscreenRT;
            toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopySource);

            D3D12_RESOURCE_BARRIER bbToCopyDest = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopyDest.Transition.pResource = bb;
                bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopyDest);
            }

            D3D12_TEXTURE_COPY_LOCATION offSrc = {};
            offSrc.pResource = dx12_hook_g_State.offscreenRT;
            offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bbDst = {};
            bbDst.pResource = bb;
            bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

            if (useExplicitPostFSRBackbufferCopyTransitions) {
                D3D12_RESOURCE_BARRIER bbToPresent = {};
                bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToPresent.Transition.pResource = bb;
                bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToPresent);
            }
        } else if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
            probeHandled = false;
        }

        if (probeHandled) {
            list->Close();
            ID3D12CommandList* lists[] = {list};
            // Keep probe submission on the queue that actually owns the tested path.
            // Post-FSR copy probes have only been observed to survive when routed
            // through the SL wrapper path rather than forcing an immediate direct
            // queue handoff.
            {
                ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL post-FSR probe submit");
                if (bootstrapRealQueueCaptureViaWrapperProbe && isSLWrapperQ) {
                    dx12_hook_s_insidePostSLOverlayECL = true;
                    probeQueue->ExecuteCommandLists(1, lists);
                    dx12_hook_s_insidePostSLOverlayECL = false;
                } else if (isSLWrapperQ && !dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire)) {
                    dx12_hook_s_insidePostSLOverlayECL = true;
                    probeQueue->ExecuteCommandLists(1, lists);
                    dx12_hook_s_insidePostSLOverlayECL = false;
                    ID3D12CommandQueue* capturedReal = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
                    if (capturedReal) {
                        HookLogImportant(
                            "DX12: PostSL post-FSR probe captured real queue %p behind wrapper bootstrap %p",
                            capturedReal, probeQueue);
                    }
                } else {
                    probeQueue->ExecuteCommandLists(1, lists);
                }
            }

            if (dx12_hook_g_State.fence) {
                UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
                HRESULT sigHr = probeQueue->Signal(dx12_hook_g_State.fence, next);
                if (SUCCEEDED(sigHr)) {
                    dx12_hook_g_State.currentFenceValue = next;
                    if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                        dx12_hook_g_State.fenceValues[idx] = next;
                }
            }

            HRESULT probeHr = dev->GetDeviceRemovedReason();
            dx12_hook_g_PostFSRProbeFrames.fetch_add(1, std::memory_order_acq_rel);
            const char* probeNames[] = {"scratch-barrier", "SLwrapper-bb-barrier", "offscreen-copy-only"};
            const char* probeName = dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3
                                        ? probeNames[dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire)]
                                        : "unknown";
            HookLogImportant(
                "DX12: PostSL post-FSR PROBE level=%d (%s) frame=%d/%d queue=%p devRemoved=0x%08X %s (fast=%d)",
                dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire), probeName,
                dx12_hook_g_PostFSRProbeFrames.load(std::memory_order_acquire), postFSRProbeFramesPerLevel, probeQueue, probeHr,
                FAILED(probeHr) ? "FAILED" : "OK", fastPostFSRDLSSProbe ? 1 : 0);

            if (FAILED(probeHr)) {
                // DEVICE_REMOVED from BB barrier is FATAL — skip to barrier-free.
                // Scratch barrier failures are non-fatal (queue just isn't ready).
                int skipTo = (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1)
                                 ? 2
                                 : dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
                dx12_hook_g_PostFSRProbeLevel.store(static_cast<int>(skipTo), std::memory_order_release);
                dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
                HookLogImportant("DX12: PostSL post-FSR probe FAILED, advancing to level %d",
                                 dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire));
                bb->Release();
                return;
            }

            if (dx12_hook_g_PostFSRProbeFrames >= postFSRProbeFramesPerLevel) {
                // Skip level 1 (BB barrier): go directly from level 0 to level 2.
                // BB barriers cause FATAL DEVICE_REMOVED on queues that don't own the
                // swapchain's resource state. Level 2 only validates copy traffic on
                // the swapchain timeline before any real overlay rendering is attempted.
                int nextLevel = (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0)
                                    ? (selectedQueueIsSwapchainQueue ? 3 : 2)
                                    : dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
                dx12_hook_g_PostFSRProbeLevel.store(static_cast<int>(nextLevel), std::memory_order_release);
                dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
                HookLogImportant(
                    "DX12: PostSL post-FSR probe PASSED, advancing to level %d (selectedScQueue=%d skipped BB barrier "
                    "probe)",
                    dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire), selectedQueueIsSwapchainQueue ? 1 : 0);
            }
            bb->Release();
            return;
        }
    }

    // Post-FSR: the DescFree backend contains device-level objects (PSO, root sig)
    // that work on any queue. Format mismatch is handled below (~line 4325).
    // No need to force-destroy — just reuse the existing backend.

    // Lazy-init DescFree backend if needed (same logic as pre-SL path).
    // The backend normally survives FG transitions warm (device-scoped); this
    // also rebuilds it after a device change.
    EnsureDescFreeBackendForDeviceAndFormat(dev, dx12_hook_g_State.format, "PostSL lazy init");

    bool willRender = dx12_hook_g_DescFreeBackend && dx12_hook_g_D3D11On12Adapter.IsInitialized();

    // Validate backbuffer format matches DescFree PSO format.
    // After FG transitions the swapchain may be recreated with a different format.
    // PSO/RTV format mismatch causes DEVICE_REMOVED.
    if (willRender && dx12_hook_g_DescFreeBackend) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        DXGI_FORMAT bbFmt = bbDesc.Format;
        DXGI_FORMAT psoFmt = dx12_hook_g_State.format;
        static int s_fmtLogCount = 0;
        if (s_fmtLogCount < 5 || (bbFmt != psoFmt && s_fmtLogCount < 50)) {
            HookLogImportant("DX12: PostSL format check — backbuffer=%d psoFmt=%d %s", (int)bbFmt, (int)psoFmt,
                             bbFmt == psoFmt ? "MATCH" : "MISMATCH");
            s_fmtLogCount++;
        }
        if (bbFmt != psoFmt) {
            // Recreate DescFree backend with correct format
            HookLogImportant("DX12: PostSL format MISMATCH (bb=%d pso=%d) — recreating DescFree backend", (int)bbFmt,
                             (int)psoFmt);
            dx12_hook_g_State.format = bbFmt;
            willRender = EnsureDescFreeBackendForDeviceAndFormat(dev, bbFmt, "PostSL format mismatch");
        }
    }

    // PROBE: After FG transitions (epoch > 1), test queue health before full render.
    // Only do Probe 1 (empty ECL). Probe 2 (ClearRTV+barriers) is unsafe during PostSL
    // because the backbuffer state on origGame's timeline is unknown — SL manages
    // backbuffer state transitions internally, and cross-queue barrier assumptions fail.
    // The graduated post-FSR scratch-barrier probe above already validated the
    // runtime-owned swapchain queue under the fast-path proof, so the separate
    // empty-ECL probe is redundant there — skipping it removes the last probe
    // present from the DLSS-engage seam.
    // Pure-DLSS off->DLSS (hadFSR=0) does NOT take the fast post-FSR probe, so without this it spends
    // the first reactivation present on the empty-ECL probe and blanks the overlay for 1 present
    // (gate=postsl-transition-probe; session 20260615_014832). On the SL-owned swapchain queue the
    // real overlay render is itself the queue-health proof (pre-submit GetDeviceRemovedReason bail at
    // ~:11174 + post-submit check), so render directly instead. Off-swapchain-queue paths keep the probe.
    const bool transitionProbeDeviceHealthy = !FAILED(dev->GetDeviceRemovedReason());
    const bool renderDirectlyOnTransitionProbe =
        ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(selectedQueueIsSwapchainQueue,
                                                                                    transitionProbeDeviceHealthy);
    int probesNeeded = (fastPostFSRDLSSProbe || renderDirectlyOnTransitionProbe) ? 0 : 1;
    bool isPostTransitionProbe = (s_reactivationEpoch > 1 && s_postSLProbeFrames < probesNeeded);
    if (renderDirectlyOnTransitionProbe && !fastPostFSRDLSSProbe && s_reactivationEpoch > 1 &&
        s_postSLProbeFrames == 0) {
        static int s_skipTransitionProbeLog = 0;
        ++s_skipTransitionProbeLog;
        if (s_skipTransitionProbeLog <= 20 || (s_skipTransitionProbeLog % 120) == 0)
            HookLogImportant(
                "DX12: PostSL rendering overlay directly on first reactivation present — skipping redundant "
                "empty-ECL transition probe (swapchain queue, device healthy; render's pre/post devRemoved check "
                "is the proof) epoch=%d queue=%p scQueue=%p hadFSR=%d",
                s_reactivationEpoch, queue, scQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
    }
    if (isPostTransitionProbe) {
        // Probe frames present without an overlay draw — tag the coverage gate
        // so engage-seam streaks attribute to the probes instead of "unknown".
        NoteDX12OverlayCoverageGate("postsl-transition-probe");
        s_postSLProbeFrames++;
        ID3D12CommandList* probeList[] = {list};

        if (s_postSLProbeFrames == 1) {
            // Probe 1: empty ECL — tests basic queue health
            list->Close();
        } else {
            // Probe 2: ClearRTV with barriers — tests backbuffer access (non-SL only)
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = bb;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrier);

            D3D12_CPU_DESCRIPTOR_HANDLE probeRtv = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            probeRtv.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, probeRtv);

            float clearColor[4] = {0, 0, 0, 0};
            list->ClearRenderTargetView(probeRtv, clearColor, 0, nullptr);


            D3D12_RESOURCE_BARRIER barrierBack = {};
            barrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrierBack.Transition.pResource = bb;
            barrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            barrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &barrierBack);
            list->Close();
        }

        // Submit probe — use origECL for SL wrapper queue, realECL otherwise
        {
            ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL transition probe submit");
            if (isSLWrapperQ) {
                ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
                if (origECL) {
                    origECL(queue, 1, probeList);
                } else {
                    queue->ExecuteCommandLists(1, probeList);
                }
            } else {
                ExecuteCommandListsPtr eclFn = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
                if (eclFn) {
                    eclFn(queue, 1, probeList);
                } else {
                    queue->ExecuteCommandLists(1, probeList);
                }
            }
        }

        // Signal fence for allocator tracking
        if (dx12_hook_g_State.fence) {
            UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
            HRESULT sigHr = queue->Signal(dx12_hook_g_State.fence, next);
            if (SUCCEEDED(sigHr)) {
                dx12_hook_g_State.currentFenceValue = next;
                if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                    dx12_hook_g_State.fenceValues[idx] = next;
            }
        }

        HRESULT probeHr = dev->GetDeviceRemovedReason();
        HookLogImportant("DX12: PostSL PROBE #%d on queue=%p (scQ=%p epoch=%d slWrapper=%d) — %s devRemoved=0x%08X %s",
                         s_postSLProbeFrames, queue, scQueue, s_reactivationEpoch, isSLWrapperQ ? 1 : 0,
                         s_postSLProbeFrames == 1 ? "empty ECL" : "ClearRTV+barriers", probeHr,
                         FAILED(probeHr) ? "FAILED" : "OK");
        if (FAILED(probeHr)) {
            bb->Release();
            return;
        }
        bb->Release();
        return;
    }

    // Cross-queue sync fence — used for SL wrapper queue ↔ origGame synchronization
    // Created lazily when needed for PostSL ECL dispatch on SL's wrapper queue.
    static ID3D12Fence* s_xqSyncFence = nullptr;
    static uint64_t s_xqSyncVal = 0;
    bool didXQSync = false;

    if (willRender && !s_xqSyncFence) {
        // Create fence lazily (needed for SL queue → origGame post-submit sync)
        HRESULT fhr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_xqSyncFence));
        if (SUCCEEDED(fhr)) {
            HookLogImportant("DX12: PostSL created cross-queue sync fence for SL↔origGame sync");
        } else {
            HookLogImportant("DX12: PostSL FAILED to create cross-queue sync fence hr=0x%08X", fhr);
        }
    }

    // Use cached FG state for barrier/queue decisions (prevents mid-function race).
    // The post-FSR selected-scQueue path is special: probes and the stable non-FG
    // ProcessFrame path both indicate the backbuffer behaves like PRESENT on that
    // queue, so keep using explicit PRESENT<->RT transitions there.
    const auto postSLBarrierMode = ce::dx12_overlay_policy::DecidePostSLBackbufferBarrierMode(
        cachedSLFGActive, useExplicitPostFSRSwapchainTransitions);
    bool slFGBarrierFree = postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly;

    // During SL FG with direct submission (bypassing SL's wrapper), we can render
    // every frame since we no longer pollute SL's internal pipeline.
    // Keep real-frame detection for metrics updates but don't skip interpolated frames.

    // BB health diagnostic: log BB pointer, dimensions, ref count periodically
    if (willRender && bb) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        // AddRef/Release to get refcount without side effects
        bb->AddRef();
        ULONG refCount = bb->Release();
        static int s_bbHealthLog = 0;
        if (s_bbHealthLog < 10 || (s_bbHealthLog % 200 == 0)) {
            HookLogImportant("DX12: PostSL BB health #%d — bb=%p refCnt=%lu w=%u h=%u fmt=%u bufIdx=%d slFG=%d",
                             s_bbHealthLog, bb, refCount, (unsigned)bbDesc.Width, (unsigned)bbDesc.Height,
                             (unsigned)bbDesc.Format, bufIdx, cachedSLFGActive ? 1 : 0);
        }
        s_bbHealthLog++;
    }
    if (willRender && !usePostSLOffscreenComposite && slFGBarrierFree) {
        // UAV barrier: full GPU pipeline flush, no state tracking modification
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;  // NULL = global flush
        list->ResourceBarrier(1, &uavBarrier);
    } else if (willRender && !usePostSLOffscreenComposite) {
        D3D12_RESOURCE_BARRIER preBarrier = {};
        preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preBarrier.Transition.pResource = bb;
        preBarrier.Transition.StateBefore =
            postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget
                ? D3D12_RESOURCE_STATE_PRESENT
                : D3D12_RESOURCE_STATE_COMMON;
        preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &preBarrier);
    }
    if (willRender) {
        static bool s_loggedBarrierMode = false;
        if (!s_loggedBarrierMode) {
            s_loggedBarrierMode = true;
            const char* barrierModeName = "common->rt";
            if (postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly) {
                barrierModeName = "uav-only";
            } else if (postSLBarrierMode ==
                       ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget) {
                barrierModeName = "present->rt";
            }
            HookLogImportant(
                "DX12: PostSL barrier mode — mode=%s slFGBarrierFree=%d explicitPostFSR=%d offscreen=%d hadFSR=%d "
                "xqSync=%d",
                barrierModeName, slFGBarrierFree ? 1 : 0, useExplicitPostFSRSwapchainTransitions ? 1 : 0,
                usePostSLOffscreenComposite ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0, didXQSync ? 1 : 0);
        }
    }
    if (willRender) {
        if (ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(cachedSLFGActive, processFrameRecentlySeen)) {
            if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
                perf->Update(PerfLogger::GetQpcUs());
                const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
                ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, g_FGCompat.GetOutputFPS(),
                                                             g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                             "DX12::PostSLOverlayRender");
            }
        }

        // Update text/API labels on real frames, but always keep the overlay
        // bound to the shared metrics object so FPS/history remain visible when
        // the first frame after an FG-driven reinit is classified as interpolated.
        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
        dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
        dx12_hook_g_D3D11On12Adapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
        const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
        if (metricsBinding.bindMetrics) {
            dx12_hook_g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        }
        if (metricsBinding.refreshFrameMetadata) {
            static const bool s_isVKD3D = []() {
                return GetModuleHandleA("d3d12core.dll") &&
                       (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
            }();
            const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
            dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
        }

        if (usePostSLOffscreenComposite &&
            EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
            // Avoid binding the post-FSR DLSS backbuffer as an RTV on the first real
            // PostSL render. Instead composite through an offscreen RT and copy back.
            D3D12_RESOURCE_BARRIER toCopyDest = {};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = dx12_hook_g_State.offscreenRT;
            toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopyDest);

            D3D12_RESOURCE_BARRIER bbToCopySource = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopySource.Transition.pResource = bb;
                bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopySource);
            }

            D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
            bbSrc.pResource = bb;
            bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION offDst = {};
            offDst.pResource = dx12_hook_g_State.offscreenRT;
            offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

            D3D12_RESOURCE_BARRIER toRenderTarget = {};
            toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Transition.pResource = dx12_hook_g_State.offscreenRT;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toRenderTarget);

            dx12_hook_s_descFreeCmdList = list;
            dx12_hook_s_descFreeRtv = dx12_hook_g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
            // PostSL/FG overlay: synchronized by the FG completion fence each
            // frame, so disable the DescFree per-slot guard (g_State.fence does
            // not track this value here — a non-zero guard would stall reuse).
            dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
            dx12_hook_s_descFreeSlotGuardValue = 0;
            SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
            dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
            dx12_hook_s_descFreeCmdList = nullptr;

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = dx12_hook_g_State.offscreenRT;
            toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toCopySource);

            D3D12_RESOURCE_BARRIER bbToCopyDest = {};
            if (useExplicitPostFSRBackbufferCopyTransitions) {
                bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToCopyDest.Transition.pResource = bb;
                bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToCopyDest);
            }

            D3D12_TEXTURE_COPY_LOCATION offSrc = {};
            offSrc.pResource = dx12_hook_g_State.offscreenRT;
            offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offSrc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION bbDst = {};
            bbDst.pResource = bb;
            bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            bbDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

            if (useExplicitPostFSRBackbufferCopyTransitions) {
                D3D12_RESOURCE_BARRIER bbToPresent = {};
                bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bbToPresent.Transition.pResource = bb;
                bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                list->ResourceBarrier(1, &bbToPresent);
            }
        } else {
            // Recreate RTV for this buffer index (cheap CPU-side op)
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            rtvHandle.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, rtvHandle);

            dx12_hook_s_descFreeCmdList = list;
            dx12_hook_s_descFreeRtv = rtvHandle;
            // PostSL/FG overlay: synchronized by the FG completion fence (see above).
            dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
            dx12_hook_s_descFreeSlotGuardValue = 0;
            SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
            dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
            dx12_hook_s_descFreeCmdList = nullptr;
        }
        rendered = true;
    } else {
        // Log why rendering was skipped (HookLogImportant for visibility after reactivation)
        static int s_backendSkip = 0;
        s_backendSkip++;
        if (s_backendSkip <= 10 || (s_backendSkip % 100) == 0)
            HookLogImportant("DX12: PostSL SKIP render #%d — backend=%p adapterInit=%d", s_backendSkip,
                             (void*)dx12_hook_g_DescFreeBackend, dx12_hook_g_D3D11On12Adapter.IsInitialized() ? 1 : 0);
    }

    // Post-rendering barrier: UAV during SL FG, standard RT→PRESENT otherwise
    if (rendered && !usePostSLOffscreenComposite && slFGBarrierFree) {
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        list->ResourceBarrier(1, &uavBarrier);

        static int s_postBarrierLog = 0;
        if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
            HookLogImportant("DX12: PostSL UAV post-barrier #%d epoch=%d", s_postBarrierLog, s_reactivationEpoch);
        }
        s_postBarrierLog++;
    } else if (rendered && !usePostSLOffscreenComposite) {
        D3D12_RESOURCE_BARRIER postBarrier = {};
        postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarrier.Transition.pResource = bb;
        postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &postBarrier);

        static int s_postBarrierLog = 0;
        if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
            HookLogImportant("DX12: PostSL RT→PRESENT post-barrier #%d epoch=%d", s_postBarrierLog,
                             s_reactivationEpoch);
        }
        s_postBarrierLog++;
    }

    // If we can't render, bail — don't submit empty command lists.
    if (!willRender) {
        list->Close();
        bb->Release();
        return;
    }

    HRESULT closeHr = list->Close();
    if (FAILED(closeHr)) {
        static int s_closeFailCount = 0;
        if (s_closeFailCount++ < 10)
            HookLog("DX12: PostSLOverlayRender — list->Close failed hr=0x%08X", closeHr);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g., by
    // SL's internal FG queue transition), don't submit — it would fail anyway.
    HRESULT preDevReason = dev->GetDeviceRemovedReason();
    if (FAILED(preDevReason)) {
        HookLogImportant("DX12: PostSL PRE-submit device already removed (hr=0x%08X queue=%p) — skipping",
                         (unsigned)preDevReason, queue);
        bb->Release();
        return;
    }

    const uint32_t preSyncLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(entryLifecycleEpoch,
                                                                             preSyncLifecycleEpoch)) {
        static std::atomic<int> s_lifecycleAbortLogCount{0};
        const int logCount = s_lifecycleAbortLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 128) == 0) {
            HookLogImportant(
                "DX12: PostSL ABORT before queue synchronization — swapchain lifecycle changed during callback "
                "(entryEpoch=%u currentEpoch=%u queue=%p scQueue=%p count=%d)",
                entryLifecycleEpoch, preSyncLifecycleEpoch, queue, scQueue, logCount + 1);
        }
        bb->Release();
        return;
    }

    // CROSS-QUEUE GPU SYNC: When our overlay queue differs from the swapchain
    // queue (scQueue), the backbuffer was last used by SL's FG pipeline on
    // scQueue.  We MUST ensure SL's GPU work completes before our barriers
    // touch the backbuffer on a different queue.  Without this sync, the GPU
    // may execute our PRESENT→RT barrier in parallel with SL's FG work on the
    // same backbuffer, causing DEVICE_REMOVED.
    //
    // Pattern: Signal on scQueue (records SL's completion point) →
    //          Wait on our queue (stalls until SL finishes)
    //
    // This is the standard D3D12 cross-queue synchronization pattern.
    // During initial DLSS FG (scQueue=NULL), this is skipped — same-queue
    // guarantees GPU ordering naturally.
    //
    // EXCEPTION: During SL FG, scQueue may be SL's internal queue (captured
    // from CreateSwapChainForHwnd during FG init).  Signal/Wait on SL's queue
    // with our fence causes DEVICE_REMOVED.  Skip cross-queue sync entirely
    // during SL FG — SL manages its own synchronization.
    bool crossQueueSynced = didXQSync;  // SL→origGame sync from above
    if (scQueue && scQueue != queue && dx12_hook_g_State.crossQueueFence && !cachedSLFGActive) {
        UINT64 syncVal = ++dx12_hook_g_State.crossQueueFenceValue;
        // Signal on scQueue: "record SL's GPU progress"
        HRESULT sigHr = scQueue->Signal(dx12_hook_g_State.crossQueueFence, syncVal);
        if (SUCCEEDED(sigHr)) {
            // Wait on our queue: "don't execute until scQueue catches up"
            HRESULT waitHr = queue->Wait(dx12_hook_g_State.crossQueueFence, syncVal);
            if (SUCCEEDED(waitHr)) {
                crossQueueSynced = true;
            } else {
                HookLog("DX12: PostSL cross-queue pre-sync Wait failed hr=0x%08X", waitHr);
            }
        } else {
            static int s_preSyncFail = 0;
            if (s_preSyncFail++ < 5)
                HookLog(
                    "DX12: PostSL cross-queue pre-sync Signal failed hr=0x%08X "
                    "(scQueue=%p may reject external signals)",
                    sigHr);
        }
    }

    // Submit ECL via virtual call on origGame during SL FG.
    //
    // CRITICAL: Do NOT use realECL(g_OriginalGameQueue, ...) — g_OriginalGameQueue
    // may be SL's COM wrapper object.  Calling the raw D3D12 ECL with an SL wrapper
    // as `this` is type confusion (internal field offsets differ).
    //
    // Virtual call → SL's COM wrapper vtable → SL processes → SL calls real D3D12
    // queue internally.  This lets SL properly track our ECL in its FG pipeline.
    //
    // For non-SL-FG paths (origECL/realECL): no change, those work as before.
    ID3D12CommandList* lists[] = {list};
    bool usedRealECL = false;
    bool usedOrigECL = false;
    bool usedVirtualCall = false;
    ID3D12CommandQueue* submittedQueue = queue;

    // Pre-submit device health check — if the device is already removed
    // (e.g. after FG teardown), skip the ECL to avoid triggering ERR_GFX_STATE.
    {
        auto* preSubmitDev = g_Device.load(std::memory_order_acquire);
        HRESULT preSubmitHr = preSubmitDev ? preSubmitDev->GetDeviceRemovedReason() : E_FAIL;
        if (FAILED(preSubmitHr)) {
            HookLogImportant("DX12: PostSL SKIPPING ECL — device removed 0x%08X (queue=%p)", (unsigned)preSubmitHr,
                             queue);
            dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
            bb->Release();
            return;
        }
    }

    // Diagnostic: on first few submits after each transition, log ECL function pointer comparison
    // (reset to 0 on PostSL REACTIVATION for fresh diagnostics)
    bool slFGAtDispatch = cachedSLFGActive;
    if (g_PostSLECLDiagCount.load(std::memory_order_relaxed) < 10) {
        ExecuteCommandListsPtr origECLDiag = GetOriginalExecuteCommandLists(queue);
        HookLogImportant(
            "DX12: PostSL ECL diag — queue=%p scQueue=%p origECL=%p realECL=%p match=%d sameQueue=%d slWrapper=%d "
            "slFG=%d hadFSR=%d",
            queue, scQueue, (void*)origECLDiag, (void*)realECL, origECLDiag == realECL ? 1 : 0,
            queue == scQueue ? 1 : 0, isSLWrapperQ ? 1 : 0, slFGAtDispatch ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
        g_PostSLECLDiagCount.fetch_add(1, std::memory_order_relaxed);
    }

    {
        const uint32_t preSubmitLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
        if (ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(entryLifecycleEpoch,
                                                                                 preSubmitLifecycleEpoch)) {
            static std::atomic<int> s_lifecycleSubmitAbortLogCount{0};
            const int logCount = s_lifecycleSubmitAbortLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 128) == 0) {
                HookLogImportant(
                    "DX12: PostSL ABORT before ECL — swapchain lifecycle changed during callback "
                    "(entryEpoch=%u currentEpoch=%u queue=%p scQueue=%p count=%d)",
                    entryLifecycleEpoch, preSubmitLifecycleEpoch, queue, scQueue, logCount + 1);
            }
            bb->Release();
            return;
        }
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL overlay submit");
        if (slFGAtDispatch) {
            // When SL FG recreated the swapchain on a different queue (scQueue != origGame),
            // submit directly on scQueue.  SL's wrapper routes to origGame, causing
            // cross-queue backbuffer access → DEVICE_REMOVED.
            // PostSL fires after SL's FG pipeline completes, so scQueue is idle.
            bool scQueueDiffers = (scQueue && scQueue != dx12_hook_g_OriginalGameQueue);

            // DIRECT QUEUE SUBMISSION (bypasses SL's COM wrapper):
            //
            // SL's COM wrapper (g_SLWrapperQueue) adds internal metadata to each ECL.
            // This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
            // Confirmed by testing:
            //   - Full-rate through wrapper: crash at ~500 frames
            //   - 1/10 rate through wrapper: stable (damage drains between submits)
            //   - Direct to real queue: 16,798+ frames stable
            //   - Empty ECL through wrapper: stable (damage requires content)
            //
            // The fix: submit directly to the real D3D12 queue behind SL's wrapper
            // using g_RealD3D12ECL (raw D3D12 function from d3d12core.dll vtable).
            //
            // Bootstrap: First frame submits through SL's wrapper with
            // s_insidePostSLOverlayECL=true.  Our ECL detour sees the real queue
            // as pThis and captures it into g_RealQueueBehindSLWrapper.
            // Subsequent frames use the direct path.
            //
            // CAUTION FOR TALOS/OTHER GAMES: If the game uses FSR FG → DLSS FG
            // transitions, the real queue behind SL might change.  Monitor for
            // DEVICE_REMOVED after transitions and re-bootstrap if needed.
            ID3D12CommandQueue* slQueue = slWrapperQueue;

            const bool allowScQueueVirtualSubmit =
                ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(dx12_hook_g_HadFSRFGPhase, scQueueDiffers);
            const bool preferSelectedSwapchainQueueDirectSubmitForPureDLSS =
                ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(
                    dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
                    selectedQueueOrigECLMatchesRealECL);

            const bool useWrapperSubmitAfterFSR = ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(
                dx12_hook_g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue, slQueue != nullptr,
                preferSelectedSwapchainQueueSubmitAfterFSR);

            if (useWrapperSubmitAfterFSR) {
                // After an FSR phase, keep swapchain-touching PostSL work on the SL
                // wrapper path when that is the only path that has successfully
                // survived the post-FSR copy probes. We can still capture the real
                // queue behind the wrapper for diagnostics and later promotion.
                submittedQueue = slQueue;
                dx12_hook_s_insidePostSLOverlayECL = true;
                slQueue->ExecuteCommandLists(1, lists);
                dx12_hook_s_insidePostSLOverlayECL = false;
                usedVirtualCall = true;

                static int s_postFSRWrapperSubmitLog = 0;
                if (s_postFSRWrapperSubmitLog < 5 || (s_postFSRWrapperSubmitLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL post-FSR submit #%d via SL wrapper %p (liveWrapper=%p scQueue=%p realQ=%p "
                        "offscreen=%d pinned=%d)",
                        s_postFSRWrapperSubmitLog, slQueue, liveSLWrapperQueue, scQueue, realQ,
                        usePostSLOffscreenComposite ? 1 : 0, usingPinnedPostFSRWrapperQueue ? 1 : 0);
                }
                s_postFSRWrapperSubmitLog++;
            } else if (preferSelectedSwapchainQueueSubmitAfterFSR) {
                // After an FSR phase, if PostSL already resolved to the runtime's
                // swapchain queue and probe submits on that queue succeeded, keep
                // using that queue directly. Falling back to the SL wrapper here
                // reintroduces the cross-queue handoff we are trying to avoid.
                submittedQueue = queue;
                if (selectedQueueOrigECL) {
                    selectedQueueOrigECL(queue, 1, lists);
                    usedOrigECL = true;
                } else {
                    realECL(queue, 1, lists);
                    usedRealECL = true;
                }

                static int s_postFSRDirectScQueueLog = 0;
                if (s_postFSRDirectScQueueLog < 5 || (s_postFSRDirectScQueueLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL post-FSR submit #%d on selected scQueue %p (origECL=%d realECL=%d wrapper=%p)",
                        s_postFSRDirectScQueueLog, queue, selectedQueueOrigECL ? 1 : 0, realECL ? 1 : 0,
                        liveSLWrapperQueue);
                }
                s_postFSRDirectScQueueLog++;
            } else if (preferSelectedQueueDirectSubmitAfterFSR) {
                // After an FSR phase, the selected queue may already expose the real
                // D3D12 submit entrypoint directly. In that case, do not bounce to a
                // different late-captured "wrapper" queue for the first rendered
                // frame; stay on the queue that already passed our probes.
                submittedQueue = queue;
                selectedQueueOrigECL(queue, 1, lists);
                usedOrigECL = true;

                static int s_postFSRDirectSelectedQueueLog = 0;
                if (s_postFSRDirectSelectedQueueLog < 10 || (s_postFSRDirectSelectedQueueLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL post-FSR direct submit #%d on selected queue %p "
                        "(origECL matches realECL, scQueue=%p latestWrapper=%p)",
                        s_postFSRDirectSelectedQueueLog, queue, scQueue, liveSLWrapperQueue);
                }
                s_postFSRDirectSelectedQueueLog++;
            } else if (preferSelectedSwapchainQueueDirectSubmitForPureDLSS) {
                // Pure-DLSS startup/runtime path: when the live swapchain queue already
                // resolves to the real/original D3D12 ECL entrypoint, avoid bouncing
                // back through the queue's current virtual dispatch.
                submittedQueue = queue;
                selectedQueueOrigECL(queue, 1, lists);
                usedOrigECL = true;

                static int s_pureDLSSDirectScQueueLog = 0;
                if (s_pureDLSSDirectScQueueLog < 10 || (s_pureDLSSDirectScQueueLog % 200) == 0) {
                    HookLogImportant(
                        "DX12: PostSL pure-DLSS direct scQueue submit #%d on %p "
                        "(origECL matches realECL, latestWrapper=%p)",
                        s_pureDLSSDirectScQueueLog, queue, liveSLWrapperQueue);
                }
                s_pureDLSSDirectScQueueLog++;
            } else if (allowScQueueVirtualSubmit) {
                // Direct submission on scQueue — backbuffers belong to this queue.
                // Bypass SL's wrapper entirely (routes to origGame → wrong queue).
                submittedQueue = scQueue;
                dx12_hook_s_insidePostSLOverlayECL = true;
                scQueue->ExecuteCommandLists(1, lists);
                dx12_hook_s_insidePostSLOverlayECL = false;
                usedVirtualCall = true;

                static int s_scQSubmitLog = 0;
                if (s_scQSubmitLog < 5 || (s_scQSubmitLog % 500 == 0))
                    HookLogImportant("DX12: PostSL scQueue submit #%d on %p (origGame=%p, bypassing SL wrapper)",
                                     s_scQSubmitLog, scQueue, dx12_hook_g_OriginalGameQueue);
                s_scQSubmitLog++;
            } else if (realQ && realECL) {
                // Direct submission: bypass SL's wrapper entirely
                submittedQueue = realQ;
                dx12_hook_s_insidePostSLOverlayECL = true;
                realECL(realQ, 1, lists);
                dx12_hook_s_insidePostSLOverlayECL = false;
                usedRealECL = true;

                static int s_directLog = 0;
                if (s_directLog < 5 || (s_directLog % 500 == 0))
                    HookLogImportant("DX12: PostSL DIRECT submit #%d on real queue %p (bypass SL wrapper)", s_directLog,
                                     realQ);
                s_directLog++;
            } else {
                const bool allowWrapperBootstrap = ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(
                    dx12_hook_g_HadFSRFGPhase, realQ != nullptr, realECL != nullptr);
                if (!allowWrapperBootstrap) {
                    // For pure-DLSS startup (no FSR history), the real ECL might not
                    // be available yet if the deferred ECL probe hasn't fired (it's
                    // deferred until the Streamline startup window expires, and the
                    // window may still be active when PostSL first renders).  In this
                    // case, selectedQueueOrigECL is still valid (saved from the vtable
                    // hook on the swapchain queue).  Fall back to submitting through
                    // selectedQueueOrigECL on the queue itself rather than refusing
                    // and dropping every overlay frame.
                    if (!dx12_hook_g_HadFSRFGPhase && selectedQueueOrigECL && selectedQueueIsSwapchainQueue) {
                        submittedQueue = queue;
                        selectedQueueOrigECL(queue, 1, lists);
                        usedOrigECL = true;
                        static int s_pureDLSSBootstrapFallbackLog = 0;
                        if (s_pureDLSSBootstrapFallbackLog < 10) {
                            HookLogImportant(
                                "DX12: PostSL pure-DLSS bootstrap fallback via selectedQueueOrigECL on %p "
                                "(realECL not yet probed, scQueue=%p)",
                                queue, scQueue);
                        }
                        s_pureDLSSBootstrapFallbackLog++;
                    } else {
                        HookLogImportant(
                            "DX12: PostSL refusing SL wrapper bootstrap without direct path "
                            "(queue=%p scQueue=%p wrapper=%p)",
                            queue, scQueue, (void*)slQueue);
                        bb->Release();
                        return;
                    }
                }

                // Bootstrap: submit through SL's wrapper to capture real queue on first call
                if (!slQueue && dx12_hook_g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL refusing post-FSR bootstrap without SL wrapper queue (queue=%p scQueue=%p)",
                        queue, scQueue);
                    bb->Release();
                    return;
                }
                if (slQueue) {
                    submittedQueue = slQueue;
                    dx12_hook_s_insidePostSLOverlayECL = true;
                    slQueue->ExecuteCommandLists(1, lists);
                    dx12_hook_s_insidePostSLOverlayECL = false;
                    usedVirtualCall = true;
                    HookLogImportant(
                        "DX12: PostSL bootstrap via SL wrapper %p (will capture real queue for direct path)", slQueue);
                } else {
                    if (!ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
                            slFGAtDispatch, slQueue != nullptr, realQ != nullptr, realECL != nullptr,
                            selectedQueueIsSwapchainQueue, selectedQueueOrigECLMatchesRealECL,
                            queue == dx12_hook_g_OriginalGameQueue)) {
                        HookLogImportant(
                            "DX12: PostSL refusing no-wrapper virtual bootstrap during Streamline FG "
                            "(queue=%p scQueue=%p realQ=%p realECL=%p)",
                            queue, scQueue, realQ, (void*)realECL);
                        bb->Release();

                        return;
                    }
                    if (slFGAtDispatch && selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL &&
                        selectedQueueOrigECL) {
                        submittedQueue = queue;
                        selectedQueueOrigECL(queue, 1, lists);
                        usedOrigECL = true;
                        static int s_noWrapperDirectSelectedQueueLog = 0;
                        if (s_noWrapperDirectSelectedQueueLog < 10 || (s_noWrapperDirectSelectedQueueLog % 200) == 0) {
                            HookLogImportant(
                                "DX12: PostSL no-wrapper direct selected-queue submit #%d on %p "
                                "(scQueue=%p origECL matches realECL)",
                                s_noWrapperDirectSelectedQueueLog, queue, scQueue);
                        }
                        s_noWrapperDirectSelectedQueueLog++;
                    } else {
                        dx12_hook_s_insidePostSLOverlayECL = true;
                        queue->ExecuteCommandLists(1, lists);
                        dx12_hook_s_insidePostSLOverlayECL = false;
                        usedVirtualCall = true;
                        static int s_noSlQ = 0;
                        if (s_noSlQ++ < 3)
                            HookLogImportant("DX12: PostSL no SL wrapper queue, using origGame %p", queue);
                    }
                }
            }
        } else if (isSLWrapperQ) {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                queue->ExecuteCommandLists(1, lists);
                usedVirtualCall = true;
            }
        } else if (realECL) {
            realECL(queue, 1, lists);
            usedRealECL = true;
        } else {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                queue->ExecuteCommandLists(1, lists);
                usedVirtualCall = true;
            }
        }
    }

    if (rendered) {
        const bool retiredOfficialUiCoverage =
            retireOfficialUiCoverageAfterExactDraw &&
            ce::dx12_streamline_ui_overlay::RetirePostSLCoverageForExactBackbufferTakeover();
        if (retireOfficialUiCoverageAfterExactDraw) {
            static std::atomic<int> s_exactTransportOverridesOfficialUiLogCount{0};
            const int logCount =
                s_exactTransportOverridesOfficialUiLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: PostSL first proven startup output received an exact backbuffer draw; "
                    "retiredOfficialUiCoverage=%d so later proxy buffers cannot inherit stale coverage "
                    "(transportForced=%d postFSR=%d explicitPureDLSSColdStart=%d call#=%d log=%d)",
                    retiredOfficialUiCoverage ? 1 : 0, dx12_hook_g_RequireExactPostSLStartupTransportDraw ? 1 : 0,
                    dx12_hook_g_HadFSRFGPhase ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0, s_callsSinceReactivation,
                    logCount);
            }
        }
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kPostSL);
        SharedMemoryLayout* postSLShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        OverlayConfig postSLOverlayCfg = GetActiveDX12OverlayConfig(postSLShm);
        bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
        if (postSLShm && g_IPC && g_IPC->IsRecording() && isRealFrame && postSLOverlayCfg.showOverlay &&
            postSLOverlayCfg.captureIncludeOverlay) {
            PublishDX12CapturedFrame(pSwapChain, postSLShm, submittedQueue, true, bufIdx);
        }
        const uint64_t postSLScreenshotRequestId = GetPendingScreenshotRequestId(postSLShm);
        if (postSLScreenshotRequestId != 0 && postSLOverlayCfg.showOverlay &&
            postSLOverlayCfg.screenshotIncludeOverlay) {
            CaptureRequestedDX12Screenshot(sc3, postSLShm, postSLScreenshotRequestId, submittedQueue);
        }
    }

    // Fence signal for allocator tracking.
    // CRITICAL: Signal on the SAME queue we submitted the command list to.
    // During SL FG with direct submission, use the real D3D12 queue behind SL's wrapper.
    bool slFGSubmit = cachedSLFGActive;
    if (dx12_hook_g_State.fence) {
        UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
        ID3D12CommandQueue* submitQueue = submittedQueue ? submittedQueue : queue;
        HRESULT sigHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            dx12_hook_g_State.currentFenceValue = next;
            if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                dx12_hook_g_State.fenceValues[idx] = next;

            // Cross-queue GPU sync: only for non-SL-FG, non-same-queue scenarios
            bool crossQueueSafe = scQueue && scQueue != submitQueue && !slFGSubmit;
            if (crossQueueSafe) {
                HRESULT waitHr = scQueue->Wait(dx12_hook_g_State.fence, next);
                crossQueueSynced = true;
                if (FAILED(waitHr)) {
                    static int s_waitFail = 0;
                    if (s_waitFail++ < 5)
                        HookLog(
                            "DX12: PostSL cross-queue Wait failed hr=0x%08X "
                            "(scQueue=%p fence=%p val=%llu)",
                            waitHr, scQueue, dx12_hook_g_State.fence, (unsigned long long)next);
                }
            }
        }
    }

    // (Dedicated queue post-sync removed — no longer using dedicated queue.)

    // Periodic allocator fence health check — detect tracking issues before crash
    if (dx12_hook_g_State.fence) {
        UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
        static int s_fenceHealthLog = 0;
        s_fenceHealthLog++;
        UINT64 expected = dx12_hook_g_State.currentFenceValue;
        UINT64 gap = (expected > completed) ? (expected - completed) : 0;
        if (s_fenceHealthLog <= 10 || (s_fenceHealthLog % 200 == 0) || gap > 10) {
            HookLogImportant(
                "DX12: PostSL fence health #%d — completed=%llu current=%llu gap=%llu allocators=%d idx=%d",
                s_fenceHealthLog, completed, expected, gap, (int)dx12_hook_g_State.allocators.size(), idx);
        }
    }

    // Diagnostic logging — log queue info and device health after submit
    static std::atomic<int> s_postSLRenderCount{0};
    int renderNum = s_postSLRenderCount.fetch_add(1, std::memory_order_relaxed) + 1;
    s_postSLRenders.fetch_add(1, std::memory_order_relaxed);
    HRESULT postDevReason = dev->GetDeviceRemovedReason();

    if (SUCCEEDED(postDevReason) && rendered && pSwapChain && submittedQueue) {
        ++dx12_hook_s_PostSLSuccessfulSubmitSequence;
        if (!dx12_hook_g_HadSuccessfulPostSLPhase.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "DX12: Latched first device-healthy PostSL submit for future repeated pure-DLSS handoff prewarm "
                "(swapchain=%p queue=%p)",
                pSwapChain, submittedQueue);
        }
        if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            dx12_hook_g_PostSLWarmResumePreservationPending.exchange(false, std::memory_order_acq_rel)) {
            HookLogImportant(
                "DX12: PostSL warm-resume preservation completed on first successful active submit "
                "(sc=%p queue=%p)",
                pSwapChain, submittedQueue);
        }
        IDXGISwapChain* previousSuccessfulPostSLSwapchain =
            dx12_hook_g_LastSuccessfulPostSLSwapchain.exchange(pSwapChain, std::memory_order_acq_rel);
        if (previousSuccessfulPostSLSwapchain != pSwapChain) {
            HookLogImportant(
                "DX12: PostSL proved exact swapchain route %p on submitted queue %p "
                "(previousSwapchain=%p epoch=%d)",
                pSwapChain, submittedQueue, previousSuccessfulPostSLSwapchain, s_reactivationEpoch);
        }

        // The same COM identity may be rebound from the normal Present route
        // to a runtime proxy route. The newest successful submit is the useful
        // ownership proof; do not let its pre-FG identity classify it as normal
        // after Streamline is explicitly switched off. Publish this before the
        // confirmed-render release stores below so the OFF callback cannot see
        // confirmation without also seeing the exact swapchain proof.
        IDXGISwapChain* expectedNormalSwapchain = pSwapChain;
        if (dx12_hook_g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
                expectedNormalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
            HookLogImportant(
                "DX12: PostSL superseded remembered original-queue ownership for swapchain %p "
                "with a successful runtime-route submit",
                pSwapChain);
        }
    }

    // Mark PostSL as confirmed rendering — pre-SL draw can now be suppressed.
    // The first ECL just landed safely (devRemoved checked below): record it for this
    // reactivation epoch so the remaining reactivation warmup is confirmed-bypassed and a
    // live overlay is never re-blanked after the retained startup swapchain is released.
    dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(true, std::memory_order_release);
    if (!dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed)) {
        dx12_hook_g_PostSLConfirmedRendering.store(true, std::memory_order_release);
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
        dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
        ReleaseStreamlineStartupActivationSwapchain("DX12: PostSL confirmed rendering");
        // kStreamlineStartupTransitionGraceMs from the SL FG activation arm covers the
        // remaining startup churn window. Streamline can still call Present briefly after
        // PostSL confirms; during that family CE keeps using the bypass trampoline for
        // Streamline-stack Presents and keeps stale OFF churn suppressed. The window
        // expires naturally from the arm time; the old ShouldClear... check at ~line 9220
        // is removed because it cleared the window too aggressively on the same call
        // where confirmed became true, re-exposing the startup churn race.
        HookLogImportant("DX12: PostSL CONFIRMED rendering via re-entrant Present — suppressing pre-SL draw");
    }
    // Reset stall counter — PostSL is actively rendering, no need for pre-SL fallback
    dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
    // Track PostSL warmup — stable frame count since last FG transition.
    // Stall fallback is only enabled after this exceeds warmup threshold.
    const int stableFrameCount = dx12_hook_g_PostSLStableFrameCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (stableFrameCount == 1) {
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLFirstConfirmedRender,
                                    "DX12::PostSLOverlayRender", submittedQueue, pSwapChain,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(),
                                    HookHasExplicitStreamlineSetOptionsActivation());
    }
    const bool extendRuntimeStateStabilization =
        dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire);
    const int runtimeStateStabilizationLastFrame =
        ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(extendRuntimeStateStabilization);
    const bool runtimeStateStabilizing =
        ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
            true, stableFrameCount, extendRuntimeStateStabilization);
    const bool runtimeStateStabilizingPreviousFrame =
        stableFrameCount > 1 && ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
                                    true, stableFrameCount - 1, extendRuntimeStateStabilization);
    if (runtimeStateStabilizing && !runtimeStateStabilizingPreviousFrame) {
        dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: PostSL confirmed startup rendering entered runtime-state stabilization "
            "(stableFrames=%d first=%d last=%d extended=%d epoch=%d)",
            stableFrameCount, ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
            runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0, s_reactivationEpoch);
    } else if (!runtimeStateStabilizing && runtimeStateStabilizingPreviousFrame) {
        HookLogImportant(
            "DX12: PostSL confirmed startup rendering left runtime-state stabilization "
            "(stableFrames=%d last=%d extended=%d epoch=%d)",
            stableFrameCount, runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0,
            s_reactivationEpoch);
    }
    // Track last working queue — survives FG transitions so we can prefer
    // a proven-safe queue when PostSL re-activates after FSR→DLSS switch.
    if (SUCCEEDED(postDevReason) && submittedQueue != dx12_hook_g_PostSLLastWorkingQueue &&
        ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(isSLWrapperQ)) {
        HookLogImportant("DX12: PostSL updating lastWorkingQueue %p -> %p", dx12_hook_g_PostSLLastWorkingQueue, submittedQueue);
        SetPostSLLastWorkingQueue(submittedQueue);
    }
    if (renderNum <= 20 || (renderNum % 10) == 0 || renderNum >= 1800 || FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: Post-SL overlay SUBMIT #%d (bufIdx=%u queue=%p scQueue=%p slWrapper=%d rendered=%d "
            "virtualCall=%d realECL=%d origECL=%d xqSync=%d tid=0x%04X devRemoved=0x%08X epoch=%d)",
            renderNum, bufIdx, submittedQueue, scQueue, isSLWrapperQ ? 1 : 0, rendered ? 1 : 0, usedVirtualCall ? 1 : 0,
            usedRealECL ? 1 : 0, usedOrigECL ? 1 : 0, crossQueueSynced ? 1 : 0, GetCurrentThreadId(),
            (unsigned)postDevReason, s_reactivationEpoch);
    }
    // Early warning: if device just failed, log immediately
    if (FAILED(postDevReason)) {
        HookLogImportant(
            "DX12: DEVICE_REMOVED detected after PostSL ECL submit #%d "
            "(queue=%p scQueue=%p hr=0x%08X)",
            renderNum, submittedQueue, scQueue, (unsigned)postDevReason);
    }

    bb->Release();
}

inline void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain) {
    // [OVERLAY COVERAGE] every SL-routed callback invocation with a real
    // swapchain is one presented frame reaching the screen through Streamline's
    // pipeline (synthetic re-entrant, startup normal-route, retained startup
    // activation service). These presents bypass DX12_ProcessFrameExternal, so
    // they are accounted here on every exit path. Null-swapchain invocations
    // (ECL-hook direct triggers) are not presents and are excluded.
    const bool accountCoverage = ce::dx12_overlay_policy::ShouldAccountPostSLCallbackAsSeparatePresent(
        pSwapChain != nullptr, HookOverlayObserverOnlyEnabled(), dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent);
    const bool officialUiCoverage = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
    auto overlayCoverageGuard = ce::make_scope_guard([accountCoverage, officialUiCoverage]() {
        if (accountCoverage) {
            AccountPresentForOverlayCoverage(officialUiCoverage, "PostSL");
        }
    });

    if (!dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        NoteDX12OverlayCoverageGate("postsl-execution-disabled");
        return;
    }

    const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
    const bool observerPolicyOnlyMode = HookOverlayObserverPolicyOnlyEnabled();
    if (observerOnlyMode) {
        static std::atomic<int> s_observerOnlyPostSLSkipLogCount{0};
        const int logCount = s_observerOnlyPostSLSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant("DX12: PostSL callback SKIPPED - observer-only mode active (swapchain=%p)",
                             (void*)pSwapChain);
        }
        EnsurePostSLDisabledForObserverOnly(
            "DX12: observer-only PostSL callback",
            ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
                observerOnlyMode, observerPolicyOnlyMode));
        return;
    }

    if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
        NoteDX12OverlayCoverageGate("device-removed");
        static std::atomic<int> s_deviceRemovedSkipLogCount{0};
        const int logCount = s_deviceRemovedSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: PostSL callback SKIPPED — device already removed (ERR_GFX_STATE detected). "
                "Skipping callback to avoid crash during unstable FG transition.");
        }
        return;
    }

    const bool postSLKeepAliveArmed = dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    IDXGISwapChain* lastSuccessfulPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(
            postSLKeepAliveArmed, streamlineFGRunning, lastSuccessfulPostSLSwapchain != nullptr,
            pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain)) {
        NoteDX12OverlayCoverageGate("postsl-keepalive-swapchain-unproven");
        static std::atomic<int> s_unprovenPostSLKeepAliveSwapchainLogCount{0};
        const int logCount = s_unprovenPostSLKeepAliveSwapchainLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: PostSL explicit-OFF keep-alive rejected an unproven swapchain "
                "(current=%p lastSuccessful=%p lastWorkingQueue=%p lockedQueue=%p log=%d)",
                pSwapChain, lastSuccessfulPostSLSwapchain, dx12_hook_g_PostSLLastWorkingQueue, dx12_hook_g_PostSLLockedQueue, logCount + 1);
        }
        return;
    }

    // A normal command-list submit inside a Streamline wrapper is NOT proof
    // that presentation ownership left the proxy: the wrapper may execute that
    // work and then present its exact previously-confirmed PostSL swapchain.
    // Retire here only when the Streamline stack itself is gone. A genuine
    // normal swapchain return is retired separately from authoritative
    // swapchain/queue identity evidence before normal routing begins.
    if (dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const bool streamlineGone = !IsStreamlineLoaded();
        if (streamlineGone) {
            dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
            dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
            SetPostSLCallbackInstalled(false, "DX12: PostSL keep-alive retired after Streamline unload");
            return;
        }
    }

    const bool startupTransitionWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    const bool wrapperProgressObserved =
        dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
    const bool activeDLSSFGRuntimeSignalObserved = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool nullSwapChain = (pSwapChain == nullptr);

    // CRITICAL FIX: When ECL hook triggers callback with nullptr swapchain (due to direct
    // PostSL callback invocation bypassing ProcessFrame), we cannot safely enter the
    // normal PostSLOverlayRender path because:
    // 1. Bootstrap will fail with nullptr swapchain (pSwapChain->GetDesc() crash)
    // 2. Overlay state cannot be properly initialized
    // 3. This leads to "Present STALLED" because PostSL enters warmup but never renders
    //
    // Instead, we should NOT call PostSLOverlayRender with nullptr. The ECL hook has
    // already cleared the startup transition window, so the next normal ProcessFrame
    // call will properly enter PostSLOverlayRenderGated with a valid swapchain and
    // complete activation correctly.
    if (nullSwapChain) {
        static std::atomic<int> s_nullSwapChainSkipLogCount{0};
        const int logCount = s_nullSwapChainSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: PostSL callback SKIPPED — null swapchain passed from ECL hook direct trigger "
                "(startupPending=%d active=%d windowActive=%d confirmed=%d). "
                "Waiting for normal ProcessFrame path with valid swapchain to complete activation.",
                startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0);
        }
        // DO NOT call PostSLOverlayRender(nullptr) — it would crash or cause stall
        // The startup window has been cleared by the ECL hook, so the next
        // ProcessFrame call will properly complete activation with a valid swapchain
        return;
    }

    if (ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
            startupTransitionWindowActive, postSLConfirmedRendering, dx12_hook_g_HadFSRFGPhase, startupTopLevelPresentConsumed,
            wrapperProgressObserved, explicitSetOptionsActivation, activeDLSSFGRuntimeSignalObserved,
            startupActivationPending, postSLActive)) {
        NoteDX12OverlayCoverageGate("postsl-startup-window-deferral");
        static std::atomic<int> s_postSLStartupWindowCallbackDeferralLogCount{0};
        const int logCount = s_postSLStartupWindowCallbackDeferralLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: PostSL gated callback deferred until startup transition window expires "
                "(startupPending=%d active=%d progress=%d consumed=%d windowActive=%d confirmed=%d "
                "explicitSetOptions=%d activeDLSSSignal=%d)",
                startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, wrapperProgressObserved ? 1 : 0,
                startupTopLevelPresentConsumed ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0, explicitSetOptionsActivation ? 1 : 0,
                activeDLSSFGRuntimeSignalObserved ? 1 : 0);
        }
        return;
    }

    dx12_hook_g_PostSLCallbackInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard =
        ce::make_scope_guard([]() { dx12_hook_g_PostSLCallbackInFlight.fetch_sub(1, std::memory_order_acq_rel); });

    if (!dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
        return;
    }

    PostSLOverlayRender(pSwapChain);
}

// ============================================================
// Steam ECL deferred overlay submission
// ============================================================
// Submit the deferred overlay command list to the specified queue.  Called from
// DetourExecuteCommandLists after Steam's overlay ECL returns, or as fallback
// from DetourPresent after CallOriginalPresent returns.  Submits CE overlay to
// the same queue Steam used, so CE overlay renders after Steam's clear.
// The callerContext distinguishes the two paths for diagnostic logging.
inline bool SubmitSteamDeferredOverlay(ID3D12CommandQueue* submitQueue, const char* callerContext) {
    if (!dx12_hook_g_steamDeferredOverlay.pending || !dx12_hook_g_steamDeferredOverlay.cmdList) {
        return false;
    }

    ID3D12CommandList* list = dx12_hook_g_steamDeferredOverlay.cmdList;
    int allocIdx = dx12_hook_g_steamDeferredOverlay.allocIdx;

    HookLogImportant("DX12: [%s] Submitting Steam-deferred overlay ECL to queue %p (cmdList=%p, allocIdx=%d)",
                     callerContext ? callerContext : "unknown", submitQueue, list, allocIdx);

    ID3D12CommandList* lists[] = {list};

    // Prefer realECL (raw tracked D3D12 ECL from d3d12core.dll) to bypass all
    // hook layers including FG vtable hooks on this queue.
    ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("Steam-deferred overlay submit");
        if (realECL) {
            realECL(submitQueue, 1, lists);
            HookLog("DX12: [%s] used realECL=%p for ECL submit", callerContext ? callerContext : "unknown",
                    (void*)realECL);
        } else {
            // Use the per-queue original ECL (un-hooked) from the vtable hook.
            // This avoids re-entering DetourExecuteCommandLists via the vtable.
            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(submitQueue);
            if (original) {
                original(submitQueue, 1, lists);
                HookLog("DX12: [%s] used GetOriginalExecuteCommandLists=%p for ECL submit",
                        callerContext ? callerContext : "unknown", (void*)original);
            } else {
                HookLogImportant("DX12: [%s] WARNING — no original ECL available, using vtable call (will recurse)",
                                 callerContext ? callerContext : "unknown");
                submitQueue->ExecuteCommandLists(1, lists);
            }
        }
    }
    NoteDX12OverlayRendered(DX12OverlayRenderRoute::kNormal);

    // Signal fence immediately (not deferred) since we need to wait before Present.
    if (dx12_hook_g_State.fence) {
        UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
        HRESULT sigHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            dx12_hook_g_State.currentFenceValue = next;
            if (allocIdx >= 0 && allocIdx < static_cast<int>(dx12_hook_g_State.fenceValues.size())) {
                dx12_hook_g_State.fenceValues[allocIdx] = next;
            }
        } else {
            HookLog("DX12: Steam-deferred overlay fence Signal failed hr=0x%08X", (unsigned)sigHr);
        }
    }

    // Clear deferred state
    dx12_hook_g_steamDeferredOverlay.pending = false;
    dx12_hook_g_steamDeferredOverlay.cmdList = nullptr;
    dx12_hook_g_steamDeferredOverlay.allocIdx = -1;
    dx12_hook_g_steamDeferredOverlay.eclQueue = nullptr;

    static std::atomic<int> s_deferredSubmitLogCount{0};
    int logNum = s_deferredSubmitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 20 || (logNum % 200) == 0) {
        HookLogImportant("DX12: Steam-deferred overlay submitted #%d (queue=%p, fence=%llu)", logNum, submitQueue,
                         (unsigned long long)dx12_hook_g_State.currentFenceValue);
    }

    return true;
}

// Steam module path suffix check: returns true if the given module path contains
// "gameoverlayrenderer" (Steam overlay DLL for x64 or x86).
inline bool IsSteamOverlayModulePath(const char* modulePath) {
    if (!modulePath || !modulePath[0])
        return false;
    return strstr(modulePath, "gameoverlayrenderer") != nullptr;
}

inline bool IsD3D12ModuleAddress(void* address) {
    if (!address) {
        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameA(module, modulePath, MAX_PATH)) {
        return false;
    }

    return strstr(modulePath, "d3d12") != nullptr || strstr(modulePath, "D3D12") != nullptr;
}

inline bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut = nullptr, DWORD* foregroundPidOut = nullptr) {
    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    bool processHasForeground = false;
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
        processHasForeground = (foregroundPid == GetCurrentProcessId());
    }
    if (foregroundWindowOut) {
        *foregroundWindowOut = foregroundWindow;
    }
    if (foregroundPidOut) {
        *foregroundPidOut = foregroundPid;
    }
    return processHasForeground;
}

inline void ClearFocusLossPendingOverlayFence(const char* reason, UINT64 fenceValue, UINT64 completedValue) {
    UINT64 expected = dx12_hook_g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
    while (expected != 0 && expected <= fenceValue) {
        if (dx12_hook_g_FocusLossPendingOverlayFenceValue.compare_exchange_weak(expected, 0, std::memory_order_acq_rel)) {
            HookLogImportant("DX12: Focus-loss overlay fence hold cleared (%s fence=%llu completed=%llu)",
                             reason ? reason : "unknown", (unsigned long long)fenceValue,
                             (unsigned long long)completedValue);
            return;
        }
    }
}

inline bool ShouldHoldOverlayDrawForPendingFocusLossFence() {
    const UINT64 pendingFenceValue = dx12_hook_g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
    if (pendingFenceValue == 0) {
        return false;
    }

    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    if (processHasForeground) {
        ClearFocusLossPendingOverlayFence("process foreground restored", pendingFenceValue, 0);
        return false;
    }

    ID3D12Fence* fence = dx12_hook_g_State.fence;
    if (!fence) {
        ClearFocusLossPendingOverlayFence("overlay fence unavailable", pendingFenceValue, 0);
        return false;
    }

    const UINT64 completedValue = fence->GetCompletedValue();
    const bool pendingFenceComplete = completedValue >= pendingFenceValue;
    if (pendingFenceComplete) {
        ClearFocusLossPendingOverlayFence("pending fence completed", pendingFenceValue, completedValue);
        return false;
    }

    if (ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(processHasForeground, true,
                                                                                       pendingFenceComplete)) {
        static std::atomic<int> s_focusLossHoldLogCount{0};
        const int logCount = s_focusLossHoldLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Holding focus-loss overlay/capture backbuffer work until prior overlay fence completes "
                "(fence=%llu completed=%llu fg=%p/%lu log=%d); backend/resources preserved",
                (unsigned long long)pendingFenceValue, (unsigned long long)completedValue, foregroundWindow,
                foregroundPid, logCount + 1);
        }
        return true;
    }

    return false;
}

inline const char* DescribeFocusLossImmediateFenceSkip(bool isWrappedD3D12Present, bool isFullscreen,
                                                       bool processHasForeground, bool isIconic, bool hasZeroSize,
                                                       bool overlaySubmitSucceeded, bool deviceLost,
                                                       bool frameGenerationActive, bool runtimeOwnedPresentation,

                                                       bool usingDedicatedQueue, bool steamDeferredOverlaySubmit,
                                                       bool hasFence, bool hasFenceEvent, bool hasQueue,
                                                       UINT64 fenceValue) {
    if (!isWrappedD3D12Present)
        return "not-wrapped-present";
    if (isFullscreen)
        return "fullscreen";
    if (processHasForeground)
        return "foreground";
    if (isIconic)
        return "iconic";
    if (hasZeroSize)
        return "zero-sized";
    if (!overlaySubmitSucceeded)
        return "overlay-not-submitted";
    if (deviceLost)
        return "device-lost";
    if (frameGenerationActive)
        return "frame-generation-active";
    if (runtimeOwnedPresentation)
        return "runtime-owned-presentation";
    if (usingDedicatedQueue)
        return "dedicated-queue";
    if (steamDeferredOverlaySubmit)
        return "steam-deferred-submit";
    if (!hasFence)
        return "no-fence";
    if (!hasFenceEvent)
        return "no-fence-event";
    if (!hasQueue)
        return "no-queue";
    if (fenceValue == 0)
        return "zero-fence-value";
    return "policy";
}

inline void RequestImmediateFocusLossFenceDumpOnce(const char* reason, UINT64 fenceValue, UINT64 completedValue,
                                                   ID3D12CommandQueue* queue,
                                                   const DX12WrappedPresentFocusLossContext& presentContext,
                                                   HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow,
                                                   DWORD processId, DWORD waitResult, DWORD waitLastError) {
    const bool dumpAlreadyRequested = dx12_hook_g_FocusLossImmediateFenceDumpRequested.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false,
                                                                                                dumpAlreadyRequested)) {
        return;
    }

    bool expected = false;
    if (!dx12_hook_g_FocusLossImmediateFenceDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                                        std::memory_order_acquire)) {
        return;
    }

    HookLogImportant(
        "DX12: Requesting immediate freeze dump for focus-loss same-frame overlay fence wait "
        "(reason=%s present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
        "sync=%u flags=0x%08X wait=%s(0x%08lX) gle=%lu targetTid=%lu)",
        reason ? reason : "unknown", presentContext.presentName ? presentContext.presentName : "Present",
        presentContext.callCount, queue, (unsigned long long)fenceValue, (unsigned long long)completedValue,
        foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
        presentContext.presentFlags, DX12WaitResultName(waitResult), waitResult, waitLastError, GetCurrentThreadId());
    g_RenderWatchdog.RequestImmediateDump(reason ? reason : "D3D12 focus-loss overlay fence wait stalled",
                                          GetCurrentThreadId());
}

inline void RequestFocusLossDeviceRemovalDumpOnce(const char* reason, HRESULT deviceRemovedReason,
                                                  const DX12WrappedPresentFocusLossContext& presentContext,
                                                  HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow,
                                                  DWORD processId, ID3D12CommandQueue* queue) {
    const bool dumpAlreadyRequested = dx12_hook_g_FocusLossDeviceRemovalDumpRequested.load(std::memory_order_acquire);
    const bool recentFocusTransition =
        dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
        dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
    const bool foregroundBelongsToProcess = foregroundPid != 0 && foregroundPid == processId;
    if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
            true, recentFocusTransition || !foregroundBelongsToProcess, dumpAlreadyRequested)) {
        return;
    }

    bool expected = false;
    if (!dx12_hook_g_FocusLossDeviceRemovalDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                                       std::memory_order_acquire)) {
        return;
    }

    HookLogImportant(
        "DX12: Requesting immediate freeze dump for focus-loss device removal "
        "(reason=%s devRemoved=0x%08X present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X "
        "targetTid=%lu)",
        reason ? reason : "unknown", (unsigned)deviceRemovedReason,
        presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
        foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
        presentContext.presentFlags, GetCurrentThreadId());
    g_RenderWatchdog.RequestImmediateDump(reason ? reason : "D3D12 focus-loss device removal", GetCurrentThreadId());
}

inline bool WaitForFocusLossImmediateOverlayFenceBeforePresent(
    bool immediateFencePolicyAccepted, bool signalSucceeded, ID3D12Fence* fence, HANDLE fenceEvent,
    ID3D12CommandQueue* queue, UINT64 fenceValue, const DX12WrappedPresentFocusLossContext& presentContext,
    HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, bool usedRealECL,
    bool directD3D12Submit, bool usedDescFree, bool offscreenCompositeRequired) {
    const bool shouldWait = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossImmediateOverlayFence(
        immediateFencePolicyAccepted, signalSucceeded, fence != nullptr, fenceEvent != nullptr, queue != nullptr,
        fenceValue);
    if (!shouldWait) {
        return false;
    }

    UINT64 completedValue = fence->GetCompletedValue();
    if (completedValue >= fenceValue) {
        ClearFocusLossPendingOverlayFence("same-frame wait already complete", fenceValue, completedValue);
        static std::atomic<int> s_focusImmediateAlreadyLogCount{0};
        const int logCount = s_focusImmediateAlreadyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 40 || (logCount % 300) == 0) {
            HookLog(
                "DX12: Focus-loss same-frame overlay fence already complete before Present "
                "(present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
                "sync=%u flags=0x%08X realECL=%d directD3D12=%d descFree=%d offscreen=%d)",
                presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
                (unsigned long long)fenceValue, (unsigned long long)completedValue, foregroundWindow, foregroundPid,
                gameWindow, processId, presentContext.syncInterval, presentContext.presentFlags, usedRealECL ? 1 : 0,
                directD3D12Submit ? 1 : 0, usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
        }
        return true;
    }

    HRESULT setHr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
    if (FAILED(setHr)) {
        const DWORD setLastError = GetLastError();
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
        static std::atomic<int> s_focusImmediateSetEventFailLogCount{0};
        const int logCount = s_focusImmediateSetEventFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Focus-loss same-frame overlay fence wait could not arm event "
                "(hr=0x%08X present=%s#%d queue=%p fence=%llu completed=%llu event=%p fg=%p/%lu "
                "game=%p/%lu sync=%u flags=0x%08X); holding future unfocused backbuffer work",
                (unsigned)setHr, presentContext.presentName ? presentContext.presentName : "Present",
                presentContext.callCount, queue, (unsigned long long)fenceValue, (unsigned long long)completedValue,
                fenceEvent, foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
                presentContext.presentFlags);
        }
        RequestImmediateFocusLossFenceDumpOnce("D3D12 focus-loss overlay fence SetEventOnCompletion failed", fenceValue,
                                               completedValue, queue, presentContext, foregroundWindow, foregroundPid,
                                               gameWindow, processId, WAIT_FAILED, setLastError);
        return false;
    }

    constexpr DWORD kFocusLossImmediateFenceDumpTimeoutMs = 2000;
    DWORD waitResult = WaitForSingleObject(fenceEvent, kFocusLossImmediateFenceDumpTimeoutMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
    completedValue = fence->GetCompletedValue();
    bool completed = completedValue >= fenceValue || waitResult == WAIT_OBJECT_0;

    if (!completed) {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
        RequestImmediateFocusLossFenceDumpOnce("D3D12 focus-loss same-frame overlay fence wait stalled", fenceValue,
                                               completedValue, queue, presentContext, foregroundWindow, foregroundPid,
                                               gameWindow, processId, waitResult, waitLastError);
        if (waitResult == WAIT_TIMEOUT) {
            const DWORD finalWaitResult = WaitForSingleObject(fenceEvent, INFINITE);
            const DWORD finalLastError = (finalWaitResult == WAIT_FAILED) ? GetLastError() : 0;
            completedValue = fence->GetCompletedValue();
            completed = completedValue >= fenceValue || finalWaitResult == WAIT_OBJECT_0;
            waitResult = finalWaitResult;
            waitLastError = finalLastError;
        }
    }

    if (completed) {
        ClearFocusLossPendingOverlayFence("same-frame wait completed", fenceValue, completedValue);
    } else {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
    }

    static std::atomic<int> s_focusImmediateWaitLogCount{0};
    const int logCount = s_focusImmediateWaitLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 80 || !completed || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Focus-loss same-frame overlay fence wait result=%s(0x%08lX) "
            "(present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
            "sync=%u flags=0x%08X timeoutMs=%lu gle=%lu completed=%d pendingHold=%d realECL=%d "
            "directD3D12=%d descFree=%d offscreen=%d)",
            DX12WaitResultName(waitResult), waitResult,
            presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
            (unsigned long long)fenceValue, (unsigned long long)completedValue, foregroundWindow, foregroundPid,
            gameWindow, processId, presentContext.syncInterval, presentContext.presentFlags,
            kFocusLossImmediateFenceDumpTimeoutMs, waitLastError, completed ? 1 : 0, completed ? 0 : 1,
            usedRealECL ? 1 : 0, directD3D12Submit ? 1 : 0, usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
    }

    return completed;
}

// ===================== DX12 focus/mode-switch analysis (config-gated; off by default) =====================
// Enabled by [Overlay] dx12_focus_analysis=true. An in-process substitute for an external GPU-scheduler
// trace (GPUView/xperf) for the 32-bit Alt+Tab freeze investigation: each present it records a "flight
// recorder" sample (GPU residency budget/usage via IDXGIAdapter3, the present-to-present gap, and
// foreground state) and, on a stall (large present gap) or device removal, dumps the recent samples so we
// can see whether the iflip<->composited focus transition drives a VRAM budget drop / over-budget
// eviction (which would explain the app's own ExecuteCommandLists stalling in a kernel GPU-VA re-residency
// map). Deliberately low-overhead and non-perturbing: it does NOT arm DRED forced breadcrumbs or
// GPU-based validation (both were proven to CAUSE this very freeze).
inline std::atomic<bool> dx12_hook_g_Dx12FocusAnalysisActive{false};

namespace {
struct Dx12FocusAnalysisSample {
    uint64_t presentIdx;
    double gapMs;
    uint32_t localBudgetMB;
    uint32_t localUsageMB;
    uint32_t nonLocalUsageMB;
    int foreground;
};
}

inline constexpr uint32_t dx12_hook_kDx12FaRingSize = 256;

inline Dx12FocusAnalysisSample dx12_hook_g_Dx12FaRing[dx12_hook_kDx12FaRingSize] = {};

inline std::atomic<uint64_t> dx12_hook_g_Dx12FaCount{0};

inline IDXGIAdapter3* dx12_hook_g_Dx12FaAdapter = nullptr;

inline void EnsureDx12FaAdapter() {
    if (dx12_hook_g_Dx12FaAdapter) {
        return;
    }
    ID3D12Device* dev = g_Device.load(std::memory_order_acquire);
    if (!dev) {
        return;
    }
    const LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        IDXGIAdapter1* adapter1 = nullptr;
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter1))) && adapter1) {
            adapter1->QueryInterface(IID_PPV_ARGS(&dx12_hook_g_Dx12FaAdapter));
            adapter1->Release();
        }
        factory->Release();
    }
}

inline bool IsDX12FocusAnalysisModeActive(SharedMemoryLayout* shm) {
    return IsOverlayDx12FocusAnalysis(GetActiveDX12OverlayConfig(shm));
}

// Sample the process virtual address space. The uncapped-FPS crash on 32-bit (NV UMD AV in the APP's
// ExecuteCommandLists, deterministic ecx=0x7f2700d4, 64-bit immune) is hypothesized to be CPU VA /
// command-buffer-pool exhaustion rather than GPU residency (which is flat). Walk committed/free regions
// and report the largest contiguous free block — if it collapses toward 0 over the seconds before the
// crash, that confirms VA exhaustion as the root cause and points the fix at CE's per-frame
// command-buffer/VA churn on the shared queue. Done at most ~1/s + once at the stall (not per-present).
inline void Dx12SampleVaSpace(uint32_t* outCommitMB, uint32_t* outFreeMB, uint32_t* outLargestFreeMB) {
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    size_t totalCommit = 0, totalFree = 0, largestFree = 0;
    int guard = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    while (addr <= maxAddr && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) {
            totalFree += mbi.RegionSize;
            if (mbi.RegionSize > largestFree) {
                largestFree = mbi.RegionSize;
            }
        } else if (mbi.State == MEM_COMMIT) {
            totalCommit += mbi.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr || ++guard > 300000) {
            break;
        }
        addr = next;
    }
    if (outCommitMB)
        *outCommitMB = static_cast<uint32_t>(totalCommit >> 20);
    if (outFreeMB)
        *outFreeMB = static_cast<uint32_t>(totalFree >> 20);
    if (outLargestFreeMB)
        *outLargestFreeMB = static_cast<uint32_t>(largestFree >> 20);
}

inline void DX12_DumpFocusAnalysisRing(const char* reason) {
    static std::atomic<int> s_dumpCount{0};
    if (s_dumpCount.fetch_add(1, std::memory_order_relaxed) >= 30) {
        return;  // bound log volume across a repro
    }
    HookLogImportant("DX12 ANALYSIS: ===== flight recorder dump (%s) =====", reason ? reason : "?");
    {
        uint32_t commitMB = 0, freeMB = 0, largestFreeMB = 0;
        Dx12SampleVaSpace(&commitMB, &freeMB, &largestFreeMB);
        HookLogImportant("DX12 ANALYSIS:  vaspace-at-stall committedMB=%u freeMB=%u largestFreeBlockMB=%u", commitMB,
                         freeMB, largestFreeMB);
    }
    const uint64_t total = dx12_hook_g_Dx12FaCount.load(std::memory_order_relaxed);
    const uint32_t n = static_cast<uint32_t>((total < dx12_hook_kDx12FaRingSize) ? total : dx12_hook_kDx12FaRingSize);
    for (uint64_t i = total - n; i < total; ++i) {
        const Dx12FocusAnalysisSample& s = dx12_hook_g_Dx12FaRing[i % dx12_hook_kDx12FaRingSize];
        HookLogImportant(
            "DX12 ANALYSIS:  present#%llu gap=%.1fms localBudget=%uMB localUsage=%uMB%s nonLocalUsage=%uMB fg=%d",
            (unsigned long long)s.presentIdx, s.gapMs, s.localBudgetMB, s.localUsageMB,
            (s.localBudgetMB && s.localUsageMB > s.localBudgetMB) ? " OVER-BUDGET" : "", s.nonLocalUsageMB,
            s.foreground);
    }
    HookLogImportant("DX12 ANALYSIS: ===== end flight recorder dump =====");
}

inline void DX12_UpdateFocusAnalysis(SharedMemoryLayout* shm) {
    const bool active = IsDX12FocusAnalysisModeActive(shm);
    dx12_hook_g_Dx12FocusAnalysisActive.store(active, std::memory_order_relaxed);
    if (!active) {
        return;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    static LARGE_INTEGER s_last = {};
    const double gapMs =
        s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
    s_last = now;

    uint32_t localBudgetMB = 0, localUsageMB = 0, nonLocalUsageMB = 0;
    EnsureDx12FaAdapter();
    if (dx12_hook_g_Dx12FaAdapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO li = {}, ni = {};
        if (SUCCEEDED(dx12_hook_g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
            localBudgetMB = static_cast<uint32_t>(li.Budget >> 20);
            localUsageMB = static_cast<uint32_t>(li.CurrentUsage >> 20);
        }
        if (SUCCEEDED(dx12_hook_g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni))) {
            nonLocalUsageMB = static_cast<uint32_t>(ni.CurrentUsage >> 20);
        }
    }

    int foreground = 0;
    if (HWND fg = GetForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        foreground = (pid == GetCurrentProcessId()) ? 1 : 0;
    }

    const uint64_t idx = dx12_hook_g_Dx12FaCount.fetch_add(1, std::memory_order_relaxed);
    Dx12FocusAnalysisSample& slot = dx12_hook_g_Dx12FaRing[idx % dx12_hook_kDx12FaRingSize];
    slot.presentIdx = idx;
    slot.gapMs = gapMs;
    slot.localBudgetMB = localBudgetMB;
    slot.localUsageMB = localUsageMB;
    slot.nonLocalUsageMB = nonLocalUsageMB;
    slot.foreground = foreground;

    // Periodic residency snapshot (~1/s) so steady-state budget/usage is visible even without a stall.
    static std::atomic<ULONGLONG> s_lastResLogMs{0};
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG prevMs = s_lastResLogMs.load(std::memory_order_relaxed);
    if (nowMs - prevMs >= 1000 && s_lastResLogMs.compare_exchange_strong(prevMs, nowMs, std::memory_order_relaxed)) {
        HookLogImportant(
            "DX12 ANALYSIS: residency localBudget=%uMB localUsage=%uMB%s nonLocalUsage=%uMB fg=%d (present#%llu)",
            localBudgetMB, localUsageMB, (localBudgetMB && localUsageMB > localBudgetMB) ? " OVER-BUDGET" : "",
            nonLocalUsageMB, foreground, (unsigned long long)idx);
        // CPU virtual-address-space probe (32-bit VA-exhaustion hypothesis for the uncapped crash).
        // Watch largestFreeBlockMB over the seconds before the crash: a steady collapse toward 0 = VA
        // exhaustion (the fix target); flat = driver-internal corruption (escalate).
        uint32_t commitMB = 0, freeMB = 0, largestFreeMB = 0;
        Dx12SampleVaSpace(&commitMB, &freeMB, &largestFreeMB);
        HookLogImportant("DX12 ANALYSIS: vaspace committedMB=%u freeMB=%u largestFreeBlockMB=%u (present#%llu)",
                         commitMB, freeMB, largestFreeMB, (unsigned long long)idx);
    }

    // Stall: dump the recorder so the residency/gap trajectory INTO the freeze is captured.
    if (gapMs > 250.0) {
        char reason[96] = {};
        _snprintf_s(reason, sizeof(reason), _TRUNCATE, "present gap %.0fms fg=%d", gapMs, foreground);
        DX12_DumpFocusAnalysisRing(reason);
    }
}

// Delay overlay rendering for first frames after ImGui init
// This prevents GPU crashes when frame generation tech (DLSS FG/FSR FG) is
// initializing
inline std::atomic<bool> dx12_hook_s_initDelayComplete{false};

inline void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                                      bool frameGenerationPresentationActive,
                                      ce::dx12_process_frame_diagnostics::StageTimings* diagnostics) {
    const int64_t diagnosticStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    if (diagnostics) {
        *diagnostics = {};
    }
    auto diagnosticGuard = ce::make_scope_guard([&]() {
        if (diagnostics) {
            diagnostics->totalUs = PerfLogger::GetQpcUs() - diagnosticStartUs;
        }
    });

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // Heartbeat for freeze watchdog — skip when device is removed so the
    // watchdog can detect the stuck state and create a diagnostic dump.
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }

    const bool protectedOfficialFFXStartupOverlayOnly = ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();
    if (protectedOfficialFFXStartupOverlayOnly) {
        static std::atomic<int> s_protectedOfficialFFXProcessFrameSkipLogCount{0};
        const uint32_t progressCount =
            dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress("ProcessFrame")) {
            HookLogImportant(
                "DX12: Protected official FFX startup progress fallback completed on ProcessFrame; resuming CE "
                "overlay/capture side effects (sc=%p processFrameSkips=%u)",
                pSwapChain, progressCount);
        } else if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
            NoteDX12OverlayCoverageGate("protected-ffx-startup-quiesce");
            const int logCount = s_protectedOfficialFFXProcessFrameSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Protected official FFX startup pending - keeping ProcessFrame tracking-only while "
                    "suppressing nested real-swapchain overlay/capture/FFX retry/probe side effects until enabled "
                    "ffxConfigure; proxy-backbuffer prework remains the overlay transport "
                    "(sc=%p count=%d progress=%u eclProgress=%u)",
                    pSwapChain, logCount + 1, progressCount,
                    dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.load(std::memory_order_acquire));
            }
        }
    }

    // Retry FFX hook initialization periodically for late-loading FSR FG modules.
    // UE5 games often load amd_fidelityfx_framegeneration_dx12.dll after initial
    // hook setup completes, so we must retry until the module is found.
    static int s_ffxRetryCounter = 0;
    static bool s_ffxRetryLogged = false;
    if (!protectedOfficialFFXStartupOverlayOnly && !FFXHook::IsInitialized()) {
        // Retry FFX hook every 60 frames (UE5 games may load FFX modules late)
        if (++s_ffxRetryCounter % 60 == 0) {
            FFXHook::Init();
            if (FFXHook::IsInitialized()) {
                HookLog("DX12: FFX Hook installed on render-frame retry #%d", s_ffxRetryCounter / 60);
            } else if (!s_ffxRetryLogged && s_ffxRetryCounter >= 600) {
                s_ffxRetryLogged = true;
                HookLog("DX12: FFX Hook not found after %d render-frame retries (FG may use native integration)",
                        s_ffxRetryCounter / 60);
            }
        }
    }

    // CRITICAL FIX: Reset delay flag when ImGui is not initialized
    // This ensures we wait again after each init
    if (!dx12_hook_g_State.overlayInit) {
        dx12_hook_s_initDelayComplete = false;
        dx12_hook_s_framesSinceInit = 0;
    }

    // Minimal delay after ImGui init before rendering overlay (for stability)
    if (dx12_hook_g_State.overlayInit && !dx12_hook_s_initDelayComplete.load()) {
        int frames = ++dx12_hook_s_framesSinceInit;
        if (frames < 1) {
            // Skip - proceed immediately
            return;
        } else {
            dx12_hook_s_initDelayComplete = true;
            HookLog(
                "DX12: ProcessFrameExternal - Overlay rendering enabled (frame "
                "%d after init)",
                frames);
        }
    }

    // CRITICAL FIX: Dynamically detect Vulkan WSI swapchains
    // When NVIDIA's Vulkan WSI-to-DXGI mapping is active, the swapchain is
    // presented through DXGI but the device is not a real D3D12 device we can
    // render to. Check this dynamically because games can switch between Vulkan
    // WSI (focused) and DXGI (unfocused) modes.
    static bool s_checkedForVulkan = false;
    static bool s_vulkanLayerActive = false;
    if (!s_checkedForVulkan) {
        HMODULE hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
        if (!hVulkanLayer) {
            hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
        }
        s_vulkanLayerActive = (hVulkanLayer != nullptr);
        if (s_vulkanLayerActive) {
            HookLog(
                "DX12: Vulkan layer detected, will skip DXGI overlay for Vulkan "
                "WSI swapchains");
        }
        s_checkedForVulkan = true;
    }

    // If Vulkan layer is active, check if this is a Vulkan WSI swapchain
    // by attempting to get the D3D12 device - Vulkan WSI swapchains will fail
    // or return a device we can't use for rendering
    if (s_vulkanLayerActive && pSwapChain) {
        ID3D12Device* pDevice = nullptr;
        HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice));
        if (FAILED(hr) || !pDevice) {
            // This is likely a Vulkan WSI swapchain - skip DX12 overlay
            // The Vulkan layer will handle overlay rendering
            return;
        }
        // Check if we can actually use this device (Vulkan WSI devices may fail
        // here)
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
        hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));
        pDevice->Release();
        if (FAILED(hr)) {
            // Vulkan WSI device that doesn't support full D3D12 features
            return;
        }
    }

    if (!pSwapChain) {
        HookLog("DX12: ProcessFrameExternal - null swapchain");
        return;
    }
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        HookLog("DX12: ProcessFrameExternal - failed to get SwapChain3");
        return;
    }
    int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
    ++dx12_hook_g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    const char* fsrHeuristicBlockedReason = nullptr;
    bool canUseFSRHeuristics = false;
    if (protectedOfficialFFXStartupOverlayOnly) {
        fsrHeuristicBlockedReason = "protected official FFX startup";
    } else {
        canUseFSRHeuristics = CanUseFSRFGHeuristics(&fsrHeuristicBlockedReason);
    }
    if (!canUseFSRHeuristics) {
        // Do not immediately clear a live heuristic/native-FSR latch just
        // because heuristics are temporarily unsafe. Talos can keep the FSR
        // runtime-owned swapchain active while transient startup/menu state
        // makes one frame look ambiguous. A hard clear here collapses runtime
        // classification to STREAMLINE_NO_FG and tears down the still-live FSR
        // overlay path.
        if (!ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(
                canUseFSRHeuristics, dx12_hook_g_FGRuntimeOwnsSwapchain, dx12_hook_g_HadFSRFGPhase,
                DXGIShared::IsStreamlineStartupHandoffPending())) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
        }
    } else if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(
                   dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                   dx12_hook_g_FGRuntimeOwnsSwapchain)) {
        if (g_FGCompat.IsHeuristicFSRFGActive()) {
            g_FGCompat.SetHeuristicFSRFGActive(false);
        }
    }
    // Interpolated (FG) frame detection: the game submits zero command lists
    // between consecutive Present calls for frames generated by the FG engine.
    bool isInterpolatedFrame = (count == 0);
    if (PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
        activeDebugSample && isInterpolatedFrame) {
        activeDebugSample->flags |= kPresentSampleFlagInterpolatedFrame;
    }

    UINT currentBackBufferIdx = sc3->GetCurrentBackBufferIndex();

    bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);

    // [OVERLAY COVERAGE] account this top-level processed present on every exit
    // path (function-scope guard so all skip returns below are included).
    // SL-owned transport presents (SL FG running with the PostSL callback
    // installed) and zero-ECL interpolated frames inherit coverage from the
    // previous covered present — their visible overlay is composed by the FG
    // runtime / drawn per re-entrant present, not by this ProcessFrame call.
    //
    // Inheritance is ONLY valid while the overlay backend is bound to the CURRENT
    // swapchain (g_State.overlayInit): the runtime can only carry forward a real
    // overlay-composed frame if one exists on the live chain. During a swapchain
    // change / suspend where overlayInit was invalidated and reinit is deferred,
    // the new swapchain presents fresh frames WITHOUT CE's overlay, so inheriting
    // would falsely mask a real blank (session 20260613_145008: a ~800ms DLSS-FG
    // suspend blank counted as covered because zero-ECL proxy presents inherited
    // while overlayInit was false). Gate inheritance on overlayInit so that window
    // counts as uncovered and the blank is measured.
    const bool overlayBackendBoundToCurrentSwapchain = dx12_hook_g_State.overlayInit;
    const bool coverageInheritsFGComposedOverlay =
        ce::dx12_streamline_ui_overlay::HasActiveCoverage() ||
        (overlayBackendBoundToCurrentSwapchain &&
         (isInterpolatedFrame || (streamlineFGRunning && DXGIShared::g_PostSLOverlayRenderCallback.load(
                                                             std::memory_order_acquire) != nullptr)));
    auto overlayCoverageGuard = ce::make_scope_guard([coverageInheritsFGComposedOverlay]() {
        AccountPresentForOverlayCoverage(coverageInheritsFGComposedOverlay, "ProcessFrameExternal");
    });

    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
        const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
        lock.lock();
        if (diagnostics) {
            diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
        }
        currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
    }
    {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool runtimeOwnedStreamlineNoFG =
            ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
                dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, 1, std::numeric_limits<uint32_t>::max());
        if (runtimeOwnedStreamlineNoFG) {
            const uint32_t presentCount =
                dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (presentCount == 1 || presentCount == dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents + 1 ||
                (presentCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Runtime-owned Streamline no-FG swapchain present progress #%u "
                    "(settlePresents=%u scQueue=%p origGame=%p cmdQ=%p)",
                    presentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, currentSwapchainQueue, dx12_hook_g_OriginalGameQueue,
                    g_CommandQueue.load(std::memory_order_acquire));
            }
            if (ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
                    dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, presentCount,
                    dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents)) {
                NoteDX12OverlayCoverageGate("fresh-streamline-no-fg-settle");
                static std::atomic<int> s_freshStreamlineNoFGProcessFrameSkipLogCount{0};
                const int logCount =
                    s_freshStreamlineNoFGProcessFrameSkipLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 120) == 0) {
                    HookLogImportant(
                        "DX12: Skipping overlay/capture processing during fresh runtime-owned Streamline "
                        "no-FG handoff "
                        "(presentCount=%u settlePresents=%u sc=%p scQueue=%p origGame=%p cmdQ=%p runtime=%s)",
                        presentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, pSwapChain, currentSwapchainQueue,
                        dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                        ce::fg_runtime::GetRuntimeModeName(runtimeMode));
                }
                sc3->Release();
                return;
            }
        } else {
            const uint32_t previous = dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.exchange(0, std::memory_order_acq_rel);
            if (previous != 0) {
                HookLogImportant(
                    "DX12: Runtime-owned Streamline no-FG settle counter reset after %u present(s) "
                    "(runtime=%s slFG=%d runtimeOwns=%d)",
                    previous, ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGRunning ? 1 : 0,
                    dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
            }
        }
    }
    const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
        currentSwapchainQueue != nullptr);
    const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool postSLLastWorkingQueueStillActiveDuringRecentTeardown =
        dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    const bool suppressHeuristicFSRActivationDuringPostFSRNonFGRecovery =
        ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
            postFSRNonFGRecovery, recentStreamlineTeardown, postSLLastWorkingQueueStillActiveDuringRecentTeardown);

    // ECL-count-based FG activation: detect frame generation via the pattern
    // of alternating real (ECL>0) and interpolated (ECL=0) frames.  This works
    // for UE5 native FSR FG and other implementations that don't use hookable
    // DLLs (e.g., statically linked into engine plugins).
    {
        static int s_eclRealFrames = 0;
        static int s_eclInterpFrames = 0;
        static bool s_eclFGDetected = false;
        // FG transitions, SL on/off handlers, and game-swapchain recovery edges
        // request a full reset: interpolated/real counts accumulated during a
        // finished FG phase are stale evidence and must not re-latch phantom
        // FSR_FG on the fresh post-transition swapchain (which arms 60-frame
        // draw cooldowns that visibly blank a healthy overlay).
        if (dx12_hook_g_ResetECLPatternHeuristic.exchange(false, std::memory_order_acq_rel)) {
            if (s_eclRealFrames > 0 || s_eclInterpFrames > 0 || s_eclFGDetected) {
                static std::atomic<int> s_eclPatternTransitionResetLogCount{0};
                const int logCount = s_eclPatternTransitionResetLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 20 || (logCount % 100) == 0) {
                    HookLogImportant(
                        "DX12: Resetting ECL-pattern FG heuristic evidence at FG transition/recovery edge "
                        "(real=%d interp=%d detected=%d log=%d)",
                        s_eclRealFrames, s_eclInterpFrames, s_eclFGDetected ? 1 : 0, logCount + 1);
                }
            }
            s_eclFGDetected = false;
            s_eclRealFrames = 0;
            s_eclInterpFrames = 0;
        }
        if (s_eclFGDetected && !g_FGCompat.IsHeuristicFSRFGActive()) {
            s_eclFGDetected = false;
            s_eclRealFrames = 0;
            s_eclInterpFrames = 0;
        }

        const bool shouldResetBlockedPatternEvidence =
            ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(
                canUseFSRHeuristics, s_eclFGDetected, s_eclRealFrames > 0, s_eclInterpFrames > 0);

        if (!canUseFSRHeuristics) {
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                if (suppressHeuristicFSRActivationDuringPostFSRNonFGRecovery) {
                    g_FGCompat.SetHeuristicFSRFGActive(false);
                }
            }
            if (shouldResetBlockedPatternEvidence) {
                static std::atomic<int> s_blockedECLPatternResetLogCount{0};
                int logCount = s_blockedECLPatternResetLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: Resetting ECL-pattern FG heuristic while FSR heuristics are blocked "
                        "(%s, real=%d interp=%d detected=%d)",
                        fsrHeuristicBlockedReason ? fsrHeuristicBlockedReason : "unsafe window", s_eclRealFrames,
                        s_eclInterpFrames, s_eclFGDetected ? 1 : 0);
                }
                s_eclFGDetected = false;
                s_eclRealFrames = 0;
                s_eclInterpFrames = 0;
            }
        } else {
            if (isInterpolatedFrame)
                ++s_eclInterpFrames;
            else
                ++s_eclRealFrames;
            if (!s_eclFGDetected && s_eclInterpFrames >= 10 && s_eclRealFrames >= 5) {
                if (UpdateHeuristicFSRFGState(true, "ecl-pattern")) {
                    s_eclFGDetected = true;
                    HookLogImportant("DX12: FG detected via ECL count pattern (real=%d, interp=%d)", s_eclRealFrames,
                                     s_eclInterpFrames);
                }
            }
        }
    }

    // With the dedicated overlay queue, overlay commands execute on a separate
    // GPU queue with CPU-side fence synchronization, so it is safe to render
    // on both real and interpolated FG frames.  Without an overlay queue, we
    // must skip interpolated frames to avoid submitting work on the game queue
    // during Streamline's Present pipeline.
    // EXCEPTION: For heuristic FSR FG in single-queue mode, the overlay submits
    // to the swapchain queue (which FSR FG owns), so rendering on interpolated
    // frames is safe and required to avoid flickering (otherwise overlay only
    // appears on real frames = half the output).
    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    bool hasDedicatedQueue = ShouldUseDedicatedOverlayQueue() && dx12_hook_g_State.overlayQueue != nullptr &&
                             dx12_hook_g_State.crossQueueFence != nullptr && dx12_hook_g_State.crossQueueFenceEvent != nullptr;
    bool heuristicFSRFG = g_FGCompat.IsHeuristicFSRFGActive();
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    int authoritativeFSRRealFrameOnlyStreak = 0;
    if (ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(
            streamlineFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, authoritativeFSRActive, isInterpolatedFrame,
            recentStreamlineTeardown)) {
        authoritativeFSRRealFrameOnlyStreak =
            dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        ResetAuthoritativeFSRRealFrameOnlyStreak();
    }

    if (ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(
            authoritativeFSRRealFrameOnlyStreak, g_FGCompat.HasDirectFFXApiConfirmation())) {
        if (authoritativeFSRRealFrameOnlyStreak == 120 || (authoritativeFSRRealFrameOnlyStreak % 600) == 0) {
            HookLogImportant(
                "DX12: Clearing stale authoritative FSR FG after %d consecutive real frames on runtime-owned "
                "swapchain without direct FFX API confirmation (origGame=%p scQueue=%p slFG=%d recentSLTeardown=%d)",
                authoritativeFSRRealFrameOnlyStreak, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, streamlineFGRunning ? 1 : 0,
                recentStreamlineTeardown ? 1 : 0);
        }
        g_FGCompat.SetFSRFGActive(false);
        g_FGCompat.SetFSRFGMultiplier(0);
        SetNativeFSRStartupConfigureArmingPending(false, "stale authoritative FSR real-frame cleanup");
        ClearOfficialFFXRuntimeOwnedPresentPathAssumption("stale authoritative FSR real-frame cleanup");
        ResetAuthoritativeFSRRealFrameOnlyStreak();
    }

    ID3D12CommandQueue* currentCommandQueueForStaleRuntimeOwnedCheck = nullptr;
    {
        std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
        const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
        lock.lock();
        if (diagnostics) {
            diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
        }
        currentCommandQueueForStaleRuntimeOwnedCheck = g_CommandQueue.load(std::memory_order_acquire);
    }
    const bool staleRuntimeOwnedStreamlineNoFGRun =
        ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
            streamlineFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, g_FGCompat.GetRuntimeMode(), dx12_hook_g_OriginalGameQueue != nullptr,
            dx12_hook_g_OriginalGameQueue != nullptr && currentCommandQueueForStaleRuntimeOwnedCheck == dx12_hook_g_OriginalGameQueue,
            isInterpolatedFrame);
    int staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak = 0;
    if (staleRuntimeOwnedStreamlineNoFGRun) {
        staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak =
            dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
    } else {
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
    }

    if (ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(
            staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak)) {
        if (staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak == 120 ||
            (staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak % 600) == 0) {
            HookLogImportant(
                "DX12: Clearing stale runtime-owned Streamline no-FG swapchain after %d consecutive real frames on "
                "origGame while runtime remains STREAMLINE_NO_FG (origGame=%p scQueue=%p slFG=%d)",
                staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak, dx12_hook_g_OriginalGameQueue, currentSwapchainQueue,
                streamlineFGRunning ? 1 : 0);
        }
        {
            std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
            const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
            lock.lock();
            if (diagnostics) {
                diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
            }
            if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
                dx12_hook_g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
                ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
                dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
            }
            if (dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
                ID3D12CommandQueue* staleRuntimeOwnedSwapchainQueue = dx12_hook_g_SwapchainQueue;
                dx12_hook_g_SwapchainQueue = nullptr;
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_SwapchainQueueCaptureTime = 0;
                HookLogImportant(
                    "DX12: Releasing stale runtime-owned Streamline no-FG swapchain queue %p after long origGame-only "
                    "real-frame run (origGame=%p)",
                    staleRuntimeOwnedSwapchainQueue, dx12_hook_g_OriginalGameQueue);
                staleRuntimeOwnedSwapchainQueue->Release();
            }
            if (!dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue) {
                dx12_hook_g_SwapchainQueue = dx12_hook_g_OriginalGameQueue;
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
                dx12_hook_g_SwapchainQueue->AddRef();
                dx12_hook_g_SwapchainQueueCaptureTime = GetTickCount64();
                HookLogImportant(
                    "DX12: Restored g_SwapchainQueue to original game queue %p after stale runtime-owned cleanup",
                    dx12_hook_g_OriginalGameQueue);
            }
        }
        dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(true, std::memory_order_release);
        if (ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(
                HasRetainedStreamlineStartupActivationSwapchain(), true)) {
            ReleaseStreamlineStartupActivationSwapchain("DX12: stale runtime-owned Streamline no-FG cleanup");
        }
        DXGIShared::ResetStreamlineStartupTransitionState();
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
    }

    if (ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
            isInterpolatedFrame, hasDedicatedQueue, heuristicFSRFG, dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning,
            recentStreamlineTeardown, postFSRNonFGRecovery, g_FGCompat.GetRuntimeMode(),
            currentSwapchainQueue != nullptr &&
                dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.load(std::memory_order_acquire) == currentSwapchainQueue,
            dx12_hook_g_FGTransitionCooldown > 0)) {
        NoteDX12OverlayCoverageGate("zero-ecl-skip");
        sc3->Release();
        return;
    }
    if (!isInterpolatedFrame &&
        ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(dx12_hook_g_FGRuntimeOwnsSwapchain,
                                                                              streamlineFGRunning) &&
        ShouldSuppressLikelyDuplicateTopLevelPresent(sc3, currentBackBufferIdx)) {
        NoteDX12OverlayCoverageGate("duplicate-top-level-present");
        sc3->Release();
        return;
    }
    bool processCapture = !isInterpolatedFrame && !protectedOfficialFFXStartupOverlayOnly;
    if (processCapture && ShouldSkipCaptureForTargetCadence()) {
        processCapture = false;
    }

    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotWantsOverlay =
        screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
    const bool screenshotUsePostSL =
        screenshotWantsOverlay && ShouldUseConfirmedPostSLForOverlayIncludedWork(screenshotOverlayCfg);
    if (screenshotRequested && !screenshotWantsOverlay) {
        const int64_t screenshotStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
        if (diagnostics) {
            diagnostics->screenshotUs += PerfLogger::GetQpcUs() - screenshotStartUs;
        }
    }

    // For interpolated frames, only render overlay (no capture processing) since
    // the backbuffer content is from the FG engine, not a real game frame.
    const int64_t innerStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    if (diagnostics) {
        diagnostics->innerCalled = true;
    }
    ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive, diagnostics);
    if (diagnostics) {
        diagnostics->innerUs = PerfLogger::GetQpcUs() - innerStartUs;
    }

    if (screenshotWantsOverlay && !screenshotUsePostSL) {
        const int64_t screenshotStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
        if (diagnostics) {
            diagnostics->screenshotUs += PerfLogger::GetQpcUs() - screenshotStartUs;
        }
    }

    sc3->Release();
}

inline const char* DX12WaitResultName(DWORD waitResult) {
    switch (waitResult) {
        case WAIT_OBJECT_0:
            return "signaled";

        case WAIT_TIMEOUT:
            return "timeout";
        case WAIT_ABANDONED:
            return "abandoned";
        case WAIT_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

inline std::atomic<int> dx12_hook_g_ECLCallCount{0};
