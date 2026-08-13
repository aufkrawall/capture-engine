#pragma once


















namespace {
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
#include "dx12_hook_types.h"
extern ExecuteCommandListsPtr oExecuteCommandLists;
extern CreateCommittedResourcePtr oCreateCommittedResource;
extern CreateCommandQueuePtr oTraceCreateCommandQueue;
extern CreateDescriptorHeapPtr oTraceCreateDescriptorHeap;
extern CommandQueueSignalPtr oTraceCommandQueueSignal;
extern std::map<void**, SignalPtr> dx12_hook_g_CommandQueueSignalOriginalByVTable;
#if defined(__clang__) || defined(__GNUC__)
#define CE_RETURN_ADDRESS() __builtin_extract_return_addr(__builtin_return_address(0))
#elif defined(_MSC_VER)
#include <intrin.h>
#define CE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_RETURN_ADDRESS() nullptr
#endif
extern std::atomic<int> g_PostSLECLDiagCount;
extern std::atomic<ID3D12Device*> g_Device;
extern std::atomic<ID3D12CommandQueue*> g_CommandQueue;
extern std::recursive_mutex g_CommandQueueMutex;
extern ID3D12Resource* g_DummyBackBuffer;
extern DX12Hook* g_dx12HookInstance;
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED ((HRESULT)0x087A0001L)
#endif

// Function pointers for global factory vtable hooks


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

void CleanupOverlay();;

void CleanupRTVs();

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);

void DX12_HookQueueVTable(ID3D12CommandQueue* queue);

void DX12_HookDeviceVTable(ID3D12Device* device);



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



HRESULT STDMETHODCALLTYPE DetourCreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riidResource, void** ppvResource);

HRESULT STDMETHODCALLTYPE DetourTraceCreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* pDesc, REFIID riid, void** ppQueue);

HRESULT STDMETHODCALLTYPE DetourTraceCreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* pDesc, REFIID riid, void** ppHeap);

HRESULT STDMETHODCALLTYPE DetourTraceCommandQueueSignal(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value);

void DX12_ServiceDeferredECLProbe();

DWORD WINAPI UnloadThread(LPVOID lpParam);

bool IsActualFrameGenerationActive();;

bool IsStreamlineLoaded();;

bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue, bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain = false, IDXGISwapChain* associatedSwapchain = nullptr, bool authoritativeNormalSwapchainReturn = false);;

bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked( bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source);;

void PostSLOverlayRender(IDXGISwapChain* pSwapChain);;
const char* DX12OverlayRenderRouteName(uint32_t route);
void NoteDX12OverlayCoverageGate(const char* gate);
DX12OverlayCoverageSnapshot GetOverlayCoverageSnapshot();

const char* DX12OverlayRenderRouteName(uint32_t route);;
void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source);
void LogOverlayCoverageSummary(const char* edge);
void NoteDX12OverlayRendered(DX12OverlayRenderRoute route);
void RequestFGDetectionHeuristicReset(ID3D12CommandQueue* authoritativeBaseline = nullptr);
void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue);
void ShutdownDescFreeBackend(const char* reason, bool shutdownMode = false);
bool EnsureDescFreeBackendForDeviceAndFormat(ID3D12Device* dev, DXGI_FORMAT format, const char* context);
void EnsureOverlayBreadcrumbBuffer(ID3D12Device* device);
void BeginOverlayGpuBreadcrumbFrame(ID3D12Device* device);
void WriteOverlayGpuBreadcrumb(ID3D12GraphicsCommandList* list, OverlayGpuBreadcrumbOp op);
DX12Context GetDX12PrerenderContext(bool preferOriginalGameQueue, bool* usesOriginalGameQueue, ID3D12CommandQueue** currentQueueSnapshot);
void DX12_PublishNativeLimiterDevice(ID3D12Device* device, ID3D12CommandQueue* queue, const char* source);
void ResetAuthoritativeFSRRealFrameOnlyStreak();
void ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
void ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
bool HasResolvedOfficialFFXStartupPath();
void ResetProtectedOfficialFFXStartupProgressCounters();
void ArmProtectedOfficialFFXStartupProgressTracking(const char* reason);
void ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason);
void StoreDeferredOfficialFFXTakeoverSideEffects(ID3D12CommandQueue* queue, const char* modulePath, const char* reason);
ID3D12CommandQueue* ConsumeDeferredOfficialFFXTakeoverSideEffects(char* modulePathOut, size_t modulePathOutCount);
ID3D12CommandQueue* ReferenceDeferredOfficialFFXTakeoverQueue();
void ClearDeferredOfficialFFXTakeoverSideEffects(const char* reason);
void ClearProtectedOfficialFFXStartupSwapchainPending(const char* reason);
void SetNativeFSRStartupConfigureArmingPending(bool pending, const char* reason);
void RememberOriginalQueueSwapchainIdentity(IDXGISwapChain* swapchain, const char* reason);
void UpdateLastKnownSwapchainHDRStateCache(DXGI_FORMAT format, bool isActualHDR, int swapChainColorSpace, bool presentationContractSupported);
bool IsReadableSwapchainPointer(const void* ptr);
bool IsExecutableCodePointer(const void* ptr);
void* ResolveLoadedOrLoadableExport(const char* moduleName, const char* functionName);
bool IsCrtPurecallFunctionPointer(const void* ptr);
bool IsUsableStartupActivationSwapchainPointer(IDXGISwapChain* swapchain);
void SafeReleaseStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source);
void ReleaseStreamlineStartupActivationSwapchain(const char* source);
bool HasRetainedStreamlineStartupActivationSwapchain();
bool HasUsableRetainedStreamlineStartupActivationSwapchainCandidate();
bool HasStartupActivationSwapchainCandidateForECLProbe();

bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue);;

bool HookHasSafePostFSRBootstrapPathImpl();;

void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain);;

void ClearPostSLQueues(const char* reason);;

void ResetFFXPresentCallbackOverlayBackend(const char* reason);;
void SetPostSLCallbackInstalled(bool installed, const char* reason);
void WaitForInFlightPostSLCallbacks(const char* reason);
void WaitForOverlayGpuIdle(const char* reason);

void CleanupDeferredPostSLQueuesIfSafe(const char* reason);;

void RealignInactiveCommandQueueToSwapchainQueue(const char* reason);;

void WaitForOverlayGpuIdle(const char* reason);;

void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper, bool deferQueueReleaseUntilCallbacksDrain = false);;
void ClearPostSLPinnedSLWrapperQueue(const char* reason);
void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut);
void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue, ID3D12CommandQueue* dedicatedQueue);
void ClearPostSLQueues(const char* reason);
void CleanupDeferredPostSLQueuesIfSafe(const char* reason);
void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue);
void InvalidateAllOverlayCachedFrames();
void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper, bool deferQueueReleaseUntilCallbacksDrain);
void RealignInactiveCommandQueueToSwapchainQueue(const char* reason);
void MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled();
bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount, HMODULE* moduleHandleOut = nullptr);
bool Dx12TraceEnabled();
bool Dx12TraceIsInfraModule(const char* base);
void Dx12TraceLog(const char* api, const char* details);
bool IsCurrentECLCallerFromThirdPartyOverlay(char* modulePathOut = nullptr, size_t modulePathOutCount = 0);
CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence( const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath, const char* ffxModulePath);
CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext();
bool ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay( const char* context, bool rawCallerFromThirdPartyOverlay, bool callerFromFFXFGModule, bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack, const char* callerModulePath);
bool ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath( const CreateSwapchainQueueCaptureEvidence& captureEvidence);
void StageProtectedOfficialFFXStartupQueueFromCreateDevice( IUnknown* createDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context);
bool ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();
bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff( IUnknown* pDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, ID3D12CommandQueue** queueOut);
bool ShouldApplySwapchainDescriptorOverridesForCreate( const CreateSwapchainQueueCaptureEvidence& captureEvidence);
void PrepareForAuthoritativeFFXSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context);
void LogSkippedSwapchainDescriptorOverridesForRuntimeCreate( const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence, UINT bufferCount, UINT flags, DXGI_SWAP_EFFECT swapEffect);
bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(HWND hWnd, IDXGISwapChain* swapchain, const char* context, HRESULT hr);
void QuiesceStreamlinePostSLForProtectedOfficialFFXStartup( IDXGISwapChain* swapchain, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context);
bool HandleProtectedOfficialFFXStartupSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, IUnknown* createDevice, IDXGISwapChain* swapchain, const char* context);
void ApplyAuthoritativeFFXTakeoverSideEffects(ID3D12CommandQueue* capturedQueue, const char* callerModulePath, const char* reason);
bool MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress(const char* source);
void ClearStaleStreamlineOwnershipForFSRTakeover(const CreateSwapchainQueueCaptureEvidence& captureEvidence, bool runtimeOwnsSwapchain, bool runtimeOwnershipJustActivated, ID3D12CommandQueue* capturedQueue);

bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue, const char* context);;
OverlayConfig GetActiveDX12OverlayConfig(SharedMemoryLayout* shm);
bool IsDX12ObserverOnlyModeActive(SharedMemoryLayout* shm);
bool IsDX12ObserverPolicyOnlyModeActive(SharedMemoryLayout* shm);
bool IsDX12ObserverStartupPresentOnlyModeActive(SharedMemoryLayout* shm);
void EnsurePostSLDisabledForObserverOnly(const char* reason, bool preserveStartupTransitionWindow = false);
bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg);
void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm, uint64_t requestId, ID3D12CommandQueue* queueOverride = nullptr);
void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx, UINT currentBackBufferIdx);

const char* DX12WaitResultName(DWORD waitResult);;
bool CanUseFSRFGHeuristics(const char** blockedReason = nullptr);
bool IsFFXPresentCallbackStalled();
ProgressResolvedOfficialFFXOverlayFallbackProof EvaluateProgressResolvedOfficialFFXOverlayFallbackProof();
void ResetFFXPresentCallbackFirstStallDetection();
ULONGLONG GetFFXPresentCallbackStallDurationMs();
void UpdateFFXPresentCallbackFirstStallDetection(bool ffxPresentCallbackStalled);
bool ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(bool ffxPresentCallbackStalled);
void LogSuppressedFFXPresentCallbackStallNormalOverlayFallback();
bool ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(const char** reason = nullptr);
void SyncSecondaryDx12OverlayColorState(DXGI_FORMAT format);
bool ResolveSwapchainOutputHDRState(IDXGISwapChain* swapchain, DXGI_FORMAT format, const char* logPrefix, int* outColorSpace = nullptr, bool* outSupported = nullptr);
void ResetFFXPresentCallbackOverlayBackend(const char* reason);
void ForceClearNativeFSRInternalNoCallbackComposition(const char* reason);
bool UpdateHeuristicFSRFGState(bool active, const char* source);
void CleanupOverlay();

void DrawOverlay(ID3D12GraphicsCommandList* list, bool isRealFrame, UINT bufferIdx, D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride = nullptr);;

void ResetStartupOverlayBackendActivationStage();;
bool IsStartupOverlayCompatibilityActive();
bool ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
void UpdateStartupOverlayCompatibilityState();
const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage);
void ResetStartupOverlayBackendActivationStage();
bool IsActualFrameGenerationActive();
bool IsFSRFrameGenerationActive();
bool IsDLSSFrameGenerationActive();
bool IsNvidiaSmoothMotionActiveRuntime();

ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);;

bool IsStreamlineLoaded();;
bool ShouldUseDedicatedOverlayQueue(const char** disabledByOverlayModule = nullptr);
bool WaitForGameQueueBeforeDedicatedOverlaySubmission(ID3D12CommandQueue* gameQueue, const char* phase);
void ProbeRealD3D12ECL(ID3D12Device* device);
bool TryPublishRealD3D12ECLCandidate(ExecuteCommandListsPtr candidate, const char* source);
bool TryPublishRealD3D12SignalCandidate(SignalPtr candidate, const char* source);
bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex,
                              const char* phase, bool requireGameQueueDrain, bool listTouchesBackbuffer);
void NoteStartupBlockingRenderModuleActivityFromECL(ID3D12CommandQueue* queue, const void* callerAddress);
bool ShouldSuppressOverlayForStartupCompat( HWND gameWindow, const char** overlayModule = nullptr, ULONGLONG* remainingMs = nullptr, ce::overlay_compat::AuxiliaryProcessWindowInfo* activeWindow = nullptr);
bool ShouldDeferOverlayInitForStartupCompat(HWND gameWindow, ULONGLONG* remainingMs = nullptr);
bool ShouldDelayOverlayInitAfterStartupResumeCompat(bool allowOverlayRender, HWND gameWindow, bool runtimeOwnedSwapchainActive, ULONGLONG* remainingMs = nullptr);
bool ApplyOverlayStartupCompatMode(HWND gameWindow);
void DisableDedicatedOverlayQueueForOverlayCompat();
void EnsureDedicatedOverlayQueueForFGCompat();
ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue);
bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue);
bool HookHasSafePostFSRBootstrapPathImpl();
bool ShouldReserveInactiveFGOverlaySpaceNow();
ID3D12CommandQueue* GetFrameClassificationQueue();
bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx);
bool ShouldSkipCaptureForTargetCadence();
__attribute__((noinline)) void DX12_SetCommandQueueInternal(ID3D12CommandQueue* pQueue, bool callerFromThirdPartyOverlay, const char* callerModulePath);
bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue, bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain, IDXGISwapChain* associatedSwapchain, bool authoritativeNormalSwapchainReturn);
bool IsDX12Swapchain(IDXGISwapChain* pSwapChain);
bool InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(const char* context, ID3D12CommandQueue* newSwapchainQueue, ID3D12CommandQueue* previousSwapchainQueue, ID3D12CommandQueue* originalGameQueue);
void PublishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason);
int FinishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason);
int RetirePostSLRouteForNormalSwapchainReturn(const char* reason);
bool HandlePostSLRouteForNormalSwapchainReturn(const char* context, ID3D12CommandQueue* returnedQueue, IDXGISwapChain* returnedSwapchain, ID3D12CommandQueue* originalGameQueue, const CreateSwapchainQueueCaptureEvidence& captureEvidence);
void CaptureSwapchainQueueFromCreateDevice(IUnknown* pDevice, IDXGISwapChain* pSwapChain, const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence);

// Forward declarations
void InstallGlobalVTableHooks();;

void HookSwapchainVTableViaTempSwapchain(bool presentOnly = false);;
void EnsurePresentInlineHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source);
void RefreshPresentHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source);
void StartTransitionCooldown();
bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked( bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source);
void MarkThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath = nullptr);
void MarkThirdPartyOverlaySwapchain(IDXGISwapChain1* pSwapChain, const char* creatorModulePath = nullptr);
void ForgetSwapchainFromTracking(IDXGISwapChain* pSwapChain);
void TrackSwapchainHwnd(IDXGISwapChain* pSwapChain, HWND hWnd);

// Forward declaration — defined below near DetourCreateSwapChainGlobal
bool IsStreamlineLoaded();;
HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndInline(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC);
bool IsStreamlineLoaded();
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainGlobal(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwndGlobal(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC);
void InstallGlobalVTableHooks();
void HookSwapchainVTableViaTempSwapchain(bool presentOnly);
void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx, D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride);
bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format);
bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue, const char* context);
void ApplyPrerenderLimitDX12(float limit, bool frameGenerationPresentationActive);
void PostSLOverlayRender(IDXGISwapChain* pSwapChain);
void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain);
bool SubmitSteamDeferredOverlay(ID3D12CommandQueue* submitQueue, const char* callerContext);
bool IsSteamOverlayModulePath(const char* modulePath);
bool IsD3D12ModuleAddress(void* address);
bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut = nullptr, DWORD* foregroundPidOut = nullptr);
void ClearFocusLossPendingOverlayFence(const char* reason, UINT64 fenceValue, UINT64 completedValue);
bool ShouldHoldOverlayDrawForPendingFocusLossFence();
const char* DescribeFocusLossImmediateFenceSkip(bool isWrappedD3D12Present, bool isFullscreen, bool processHasForeground, bool isIconic, bool hasZeroSize, bool overlaySubmitSucceeded, bool deviceLost, bool frameGenerationActive, bool runtimeOwnedPresentation,  bool usingDedicatedQueue, bool steamDeferredOverlaySubmit, bool hasFence, bool hasFenceEvent, bool hasQueue, UINT64 fenceValue);
void RequestImmediateFocusLossFenceDumpOnce(const char* reason, UINT64 fenceValue, UINT64 completedValue, ID3D12CommandQueue* queue, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, DWORD waitResult, DWORD waitLastError);
void RequestFocusLossDeviceRemovalDumpOnce(const char* reason, HRESULT deviceRemovedReason, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, ID3D12CommandQueue* queue);
bool WaitForFocusLossImmediateOverlayFenceBeforePresent( bool immediateFencePolicyAccepted, bool signalSucceeded, ID3D12Fence* fence, HANDLE fenceEvent, ID3D12CommandQueue* queue, UINT64 fenceValue, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, bool usedRealECL, bool directD3D12Submit, bool usedDescFree, bool offscreenCompositeRequired);
namespace {
}
void EnsureDx12FaAdapter();
bool IsDX12FocusAnalysisModeActive(SharedMemoryLayout* shm);
void Dx12SampleVaSpace(uint32_t* outCommitMB, uint32_t* outFreeMB, uint32_t* outLargestFreeMB);
void DX12_DumpFocusAnalysisRing(const char* reason);
void DX12_UpdateFocusAnalysis(SharedMemoryLayout* shm);
void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent, bool frameGenerationPresentationActive, ce::dx12_process_frame_diagnostics::StageTimings* diagnostics);
const char* DX12WaitResultName(DWORD waitResult);
