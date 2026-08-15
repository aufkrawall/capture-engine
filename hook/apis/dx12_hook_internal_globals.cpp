#include "dx12_hook_internal.h"

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex dx12_hook_g_ExecuteCommandListsHookStateMutex;

std::map<void**, ExecuteCommandListsPtr> dx12_hook_g_ExecuteCommandListsOriginalByVTable;

std::map<void**, SignalPtr> dx12_hook_g_CommandQueueSignalOriginalByVTable;

std::atomic<uint64_t> dx12_hook_g_ExecuteCommandListsCaptureGeneration{0};

std::atomic<void**> dx12_hook_g_LastExecuteCommandListsVTable{nullptr};

std::atomic<ExecuteCommandListsPtr> dx12_hook_g_LastExecuteCommandListsOriginal{nullptr};

std::atomic<ExecuteCommandListsPtr> dx12_hook_g_RealD3D12ECL{nullptr};

std::atomic<SignalPtr> dx12_hook_g_RealD3D12Signal{nullptr};

std::atomic<ID3D12Fence*> dx12_hook_g_OverlayCompletionFence{nullptr};

std::atomic<bool> dx12_hook_g_ProbeRealD3D12ECLDeferred{false};

PFN_CreateSwapChain dx12_hook_oCreateSwapChain = nullptr;

PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwnd = nullptr;

ID3D12GraphicsCommandList* dx12_hook_s_descFreeCmdList = nullptr;

D3D12_CPU_DESCRIPTOR_HANDLE dx12_hook_s_descFreeRtv = {};

ID3D12Fence* dx12_hook_s_descFreeSlotFence = nullptr;

UINT64 dx12_hook_s_descFreeSlotGuardValue = 0;

std::atomic<bool> dx12_hook_g_PostSLOverlayActive{false};

std::atomic<int> dx12_hook_g_PostSLCooldownRemaining{0};

std::atomic<bool> dx12_hook_g_PostSLExplicitOffKeepAlive{false};

std::atomic<bool> dx12_hook_g_PostSLWarmResumePreservationPending{false};

std::atomic<IDXGISwapChain*> dx12_hook_g_LastSuccessfulPostSLSwapchain{nullptr};

thread_local uint64_t dx12_hook_s_PostSLSuccessfulSubmitSequence = 0;

std::atomic<ULONGLONG> dx12_hook_g_LastProcessFrameTickMs{0};

std::atomic<ULONGLONG> dx12_hook_g_LastFFXPresentCallbackTickMs{0};

std::atomic<bool> dx12_hook_g_FFXPresentCallbackBridgeExpected{false};

std::atomic<bool> dx12_hook_g_NativeFSRInternalNoCallbackComposition{false};

std::atomic<ULONGLONG> dx12_hook_g_LastDX12OverlayRenderTickMs{0};

std::atomic<uint32_t> dx12_hook_g_LastDX12OverlayRenderRoute{static_cast<uint32_t>(DX12OverlayRenderRoute::kNone)};

std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupTakeoverLogged{false};

std::atomic<int> dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount{0};

std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested{false};

std::atomic<uint32_t> dx12_hook_g_PostSLLifecycleEpoch{0};

std::mutex dx12_hook_g_PostSLRenderMutex;

std::atomic<uint32_t> dx12_hook_g_StreamlineEnableCallsInFlight{0};

std::atomic<bool> dx12_hook_g_PostSLConfirmedRendering{false};

std::atomic<bool> dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch{false};

std::atomic<bool> dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed{false};

std::atomic<bool> dx12_hook_g_PostSLRuntimeStateStabilizationLogged{false};

std::atomic<bool> dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch{false};

std::atomic<uint64_t> dx12_hook_g_OverlayCoverageDrawCount{0};

std::atomic<uint64_t> dx12_hook_g_OverlayCoverageLastSeenDrawCount{0};

std::atomic<const char*> dx12_hook_g_OverlayCoverageLastGate{nullptr};

std::atomic<const char*> dx12_hook_g_OverlayCoverageStreakGate{nullptr};

std::atomic<uint64_t> dx12_hook_g_OverlayCoverageStreakStartTickMs{0};

std::atomic<bool> dx12_hook_g_OverlayCoverageStreakStartConfirmed{false};

ce::dx12_overlay_policy::OverlayPresentCoverageTracker dx12_hook_g_OverlayCoverageTracker;

std::atomic_flag dx12_hook_g_OverlayCoverageLock = ATOMIC_FLAG_INIT;

thread_local bool dx12_hook_g_RequireExactPostSLStartupTransportDraw = false;

thread_local bool dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent = false;

std::atomic<int> dx12_hook_g_OverlayHandoffVerboseLogPresents{0};

std::atomic<uint32_t> dx12_hook_g_OverlayHandoffVerbosePrevRoute{0};

std::atomic<int> dx12_hook_g_PostSLStallCounter{0};

std::atomic<int> dx12_hook_g_PostSLStableFrameCount{0};

std::atomic<bool> dx12_hook_g_ResetQueueChangeHeuristic{false};

std::atomic<bool> dx12_hook_g_ResetECLPatternHeuristic{false};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline{nullptr};

std::atomic<int> dx12_hook_g_SLOffHeuristicGrace{0};

std::atomic<int> dx12_hook_g_SLOffSwapchainReinitGrace{0};

std::atomic<bool> dx12_hook_g_ResetReinitSubmitCounter{false};

std::atomic<uint32_t> dx12_hook_g_OuterSLTransitionEpoch{0};

std::atomic<bool> dx12_hook_g_OuterTrackedSLFGRunning{false};

ID3D12CommandQueue* dx12_hook_g_PostSLLockedQueue = nullptr;

std::atomic<bool> dx12_hook_g_HadFSRFGPhase{false};

std::atomic<bool> dx12_hook_g_HadSuccessfulPostSLPhase{false};

std::atomic<bool> dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG{false};

bool dx12_hook_g_NeedGPUDrainBeforeRender = false;

ID3D12Fence* dx12_hook_g_DrainFence = nullptr;

HANDLE dx12_hook_g_DrainEvent = nullptr;

UINT64 dx12_hook_g_DrainFenceValue = 0;

std::atomic<int> dx12_hook_g_PostFSRProbeLevel{0};  // 0=scratch, 1=reserved, 2=offscreen-copy-only, 3=full allowed

std::atomic<int> dx12_hook_g_PostFSRProbeFrames{0};

bool dx12_hook_g_PostFSRDescFreeRecreated = false;

ID3D12CommandQueue* dx12_hook_g_PostSLDedicatedQueue = nullptr;

ID3D12CommandQueue* dx12_hook_g_PostSLLastWorkingQueue = nullptr;

std::atomic<int> dx12_hook_g_SceneTransitionCooldown{0};

ID3D12CommandQueue* dx12_hook_g_PreFGGameQueue = nullptr;

ID3D12CommandQueue* dx12_hook_g_OriginalGameQueue = nullptr;

std::atomic<int> dx12_hook_g_FGTransitionCooldown{0};

int dx12_hook_g_FramesSinceFGActive = 9999;

DX12DescFreeBackend* dx12_hook_g_DescFreeBackend = nullptr;

DX12OverlayState dx12_hook_g_State;

bool dx12_hook_g_deferOverlaySubmitToSteamECL = false;

SteamDeferredOverlaySubmitState dx12_hook_g_steamDeferredOverlay;

SharedCaptureD3D12 dx12_hook_g_SharedCaptureD3D12;

OverlayAdapter dx12_hook_g_D3D11On12Adapter;

OverlayAdapter dx12_hook_g_SLFGAdapter;

OverlayAdapter dx12_hook_g_FFXPresentOverlayAdapter;

ID3D12Device* dx12_hook_g_FFXPresentOverlayDevice = nullptr;

DXGI_FORMAT dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;

std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferWidth{0};

std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferHeight{0};

std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferFormat{0};  // DXGI_FORMAT

ID3D12Device* dx12_hook_g_DescFreeBackendDevice = nullptr;

DXGI_FORMAT dx12_hook_g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;

ID3D12Resource* dx12_hook_g_OverlayBcBuffer = nullptr;

volatile uint32_t* dx12_hook_g_OverlayBcMapped = nullptr;

D3D12_GPU_VIRTUAL_ADDRESS dx12_hook_g_OverlayBcGpuVA = 0;

std::atomic<uint32_t> dx12_hook_g_OverlayBcSeq{0};

std::atomic<int> dx12_hook_g_CommandListsExecutedThisFrame{0};

std::atomic<uint64_t> dx12_hook_g_FGDebugFrameCount{0};

std::atomic<int> dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak{0};

std::atomic<int> dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak{0};

std::atomic<bool> dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun{false};

std::atomic<bool> dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown{false};

std::atomic<bool> dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain{false};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue{nullptr};

std::atomic<IDXGISwapChain*> dx12_hook_g_ExactGameSwapchainRecoverySwapchain{nullptr};

std::atomic<IDXGISwapChain*> dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain{nullptr};

std::atomic<IDXGISwapChain*> dx12_hook_g_PrewarmedPostSLHandoffSwapchain{nullptr};

std::atomic<bool> dx12_hook_g_NativeFSRStartupConfigureArmingPending{false};

std::atomic<bool> dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending{false};

std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips{0};

std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs{0};

std::atomic<ULONGLONG> dx12_hook_g_ProtectedOfficialFFXStartupBeginMs{0};

std::atomic<bool> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress{false};

std::atomic<ULONGLONG> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs{0};

std::atomic<bool> dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending{false};

std::mutex dx12_hook_g_DeferredOfficialFFXTakeoverMutex;

ID3D12CommandQueue* dx12_hook_g_DeferredOfficialFFXTakeoverQueue = nullptr;

char dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[MAX_PATH] = {};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_PrimaryGameQueue{nullptr};

std::atomic<bool> dx12_hook_g_KnownDLSSFGModuleSeen{false};

IDXGISwapChain* dx12_hook_g_LastSwapChain = nullptr;

std::atomic<IDXGISwapChain*> dx12_hook_g_LastSwapchainQueueCaptureSwapchain{nullptr};

std::atomic<IDXGISwapChain*> dx12_hook_g_LastProvenOriginalQueueSwapchain{nullptr};

IDXGISwapChain* dx12_hook_g_PendingSwapChainCleanup = nullptr;

std::atomic<bool> dx12_hook_g_LastKnownSwapchainHDRStateValid{false};

std::atomic<bool> dx12_hook_g_LastKnownSwapchainIsHDR{false};

std::atomic<int> dx12_hook_g_LastKnownSwapchainColorSpace{-1};

std::mutex dx12_hook_g_StreamlineStartupActivationSwapchainMutex;

IDXGISwapChain* dx12_hook_g_StreamlineStartupActivationSwapchain = nullptr;

std::atomic<DWORD> dx12_hook_g_GamePresentThreadId{0};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_SLWrapperQueue{nullptr};

ID3D12CommandQueue* dx12_hook_g_PostSLPinnedSLWrapperQueue = nullptr;

std::atomic<ID3D12CommandQueue*> dx12_hook_g_RealQueueBehindSLWrapper{nullptr};

std::atomic<bool> dx12_hook_g_PostSLCallbackExecutionEnabled{false};

std::atomic<uint32_t> dx12_hook_g_PostSLCallbackInFlight{0};

std::atomic<bool> dx12_hook_g_PostSLDeferredQueueCleanupPending{false};

std::atomic<bool> dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged{false};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_DeferredCommandQueueRelease{nullptr};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_DeferredPostSLLockedQueueRelease{nullptr};

std::atomic<ULONGLONG> dx12_hook_g_PostSLRecentTeardownActivityUntilMs{0};

ID3D12CommandQueue* dx12_hook_g_SwapchainQueue = nullptr;

ULONGLONG dx12_hook_g_SwapchainQueueCaptureTime = 0;  // GetTickCount64() when scQueue was last set

bool dx12_hook_g_FGRuntimeOwnsSwapchain = false;

ULONGLONG dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;

std::atomic<bool> dx12_hook_g_CreatingTempSwapchain{false};
std::atomic<bool> dx12_hook_g_EarlyPresentHookInstallDeferred{false};
std::atomic<bool> dx12_hook_g_PostponedPresentHookInstallLogged{false};

thread_local unsigned dx12_hook_s_InternalDXGISwapchainProbeDepth = 0;

void DX12_BeginInternalDXGISwapchainProbe() {
    ++dx12_hook_s_InternalDXGISwapchainProbeDepth;
}

void DX12_EndInternalDXGISwapchainProbe() {
    if (dx12_hook_s_InternalDXGISwapchainProbeDepth != 0) {
        --dx12_hook_s_InternalDXGISwapchainProbeDepth;
    }
}

bool DX12_IsInternalDXGISwapchainProbe() {
    return dx12_hook_s_InternalDXGISwapchainProbeDepth != 0;
}

thread_local ForwardedCreateSwapchainForHwndCallerContext dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext;

thread_local int dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth = 0;

thread_local bool dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = false;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex dx12_hook_g_OverlayMutex;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex dx12_hook_g_DX12CaptureMutex;

std::atomic<bool> dx12_hook_g_InSwapchainResizeCleanup{false};

std::atomic<bool> dx12_hook_g_PreserveOverlayAdapterAcrossResize{false};

std::atomic<ID3D12Device*> dx12_hook_g_OverlayAdapterBackendDevice{nullptr};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_OverlayAdapterBackendQueue{nullptr};

std::atomic<int> dx12_hook_g_OverlayAdapterBackendFormat{static_cast<int>(DXGI_FORMAT_UNKNOWN)};

std::atomic<int> dx12_hook_s_framesSinceInit{0};

std::atomic<int> dx12_hook_s_framesBeforeInit{0};

std::vector<ID3D12Fence*> dx12_hook_g_PrerenderFences;

std::vector<HANDLE> dx12_hook_g_PrerenderEvents;

uint64_t dx12_hook_g_PrerenderFrameIndex = 0;

std::mutex dx12_hook_g_PrerenderMutex;

ID3D12Device* dx12_hook_g_PrerenderDevice = nullptr;

ID3D12CommandQueue* dx12_hook_g_PrerenderQueue = nullptr;

std::atomic<bool> dx12_hook_g_PiggybackOverlayActive{false};

ID3D12DescriptorHeap* dx12_hook_g_FFXPresentRtvHeap = nullptr;

std::mutex dx12_hook_g_FFXPresentCallbackBridgeMutex;

std::unordered_map<void*, FFXPresentCallbackBridgeState> dx12_hook_g_FFXPresentCallbackBridges;

std::atomic<UINT64> dx12_hook_g_deferredSignalValue{0};

std::atomic<int> dx12_hook_g_deferredSignalAllocIdx{-1};

std::atomic<ID3D12CommandQueue*> dx12_hook_g_deferredSignalQueue{nullptr};

std::atomic<UINT64> dx12_hook_g_FocusLossPendingOverlayFenceValue{0};

std::atomic<bool> dx12_hook_g_FocusLossImmediateFenceDumpRequested{false};

std::atomic<bool> dx12_hook_g_FocusLossDeviceRemovalDumpRequested{false};

std::atomic<int> dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining{0};

std::atomic<int> dx12_hook_g_FocusLossRecentTransitionPresentWindow{0};

std::atomic<bool> dx12_hook_g_SwapchainPresentOccluded{false};

std::atomic<bool> dx12_hook_g_HaveD3D12PresentResultSignal{false};

std::atomic<int> dx12_hook_g_FocusTransitionHoldFrames{0};

thread_local DX12WrappedPresentFocusLossContext dx12_hook_s_WrappedPresentFocusLossContext = {};

thread_local bool dx12_hook_s_insideECL = false;

thread_local bool dx12_hook_s_insidePostSLOverlayECL = false;

thread_local int dx12_hook_s_insideCEOverlayECLDepth = 0;

thread_local const char* dx12_hook_s_insideCEOverlayECLReason = nullptr;

std::atomic<ULONGLONG> dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs{0};

std::atomic<ULONGLONG> dx12_hook_g_OverlaySuppressedSinceMs{0};

std::atomic<uint32_t> dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount{0};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex dx12_hook_g_FFXUiCompositeMutex;

ID3D12CommandQueue* dx12_hook_g_FFXUiCompositeQueue = nullptr;

ID3D12Fence* dx12_hook_g_FFXUiCompositeFence =
    nullptr;  // signaled on g_FFXUiCompositeQueue (CE's own queue, not the game queue)

UINT64 dx12_hook_g_FFXUiCompositeFenceVal = 0;

int dx12_hook_g_FFXUiCompositeFrame = 0;

StartupOverlayActivationStage dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;

StartupOverlayFirstDrawProbeStage dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;

ULONGLONG dx12_hook_s_startupOverlayActivationStageMs = 0;

ULONGLONG dx12_hook_s_startupOverlaySyncInitMs = 0;

ULONGLONG dx12_hook_s_startupOverlayResourcePrimeMs = 0;

ULONGLONG dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;

std::atomic<ULONGLONG> dx12_hook_s_lastStartupBlockingRenderModuleActivityMs{0};

std::atomic<bool> dx12_hook_s_startupOverlayCompatSettled{false};

std::atomic<bool> dx12_hook_s_startupOverlayObservedAnyFG{false};

std::atomic<bool> dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff{false};

std::atomic<bool> dx12_hook_g_DeviceRemoved{false};

PFN_CreateSwapChain dx12_hook_oCreateSwapChainGlobal = nullptr;

PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwndGlobal = nullptr;

// The factory vtable dx12_hook_oCreateSwapChainForHwndGlobal was saved from.
// That slot function may only be invoked with objects carrying this exact
// vtable (see hook/common/dx12_factory_slot_policy.h); a factory proxy from a
// hooked CreateDXGIFactory1 is a different class and type-punning it as
// CDXGIFactory corrupts the dxgi adapter table (sessions 20260813_004853 /
// 20260813_004923).
void** dx12_hook_s_savedCreateSwapChainForHwndVtable = nullptr;

PFN_CreateSwapChainForHwnd dx12_hook_s_oCreateSCForHwndInline = nullptr;

void* dx12_hook_s_realCreateSCForHwndAddr = nullptr;

PFN_CreateSwapChainForHwnd dx12_hook_s_deepHookTrampoline = nullptr;

std::atomic<int64_t> dx12_hook_g_OverlayCooldownUntilQpc{0};

std::mutex dx12_hook_s_hwndSwapchainMutex;

std::map<HWND, std::vector<IDXGISwapChain*>> dx12_hook_s_hwndSwapchainMap;

std::atomic<bool> dx12_hook_g_Dx12FocusAnalysisActive{false};

Dx12FocusAnalysisSample dx12_hook_g_Dx12FaRing[dx12_hook_kDx12FaRingSize] = {};

std::atomic<uint64_t> dx12_hook_g_Dx12FaCount{0};

IDXGIAdapter3* dx12_hook_g_Dx12FaAdapter = nullptr;

std::atomic<bool> dx12_hook_s_initDelayComplete{false};

std::atomic<int> dx12_hook_g_ECLCallCount{0};
