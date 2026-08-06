#include "dx12_hook_internal.h"


void ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown() {
dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(false, std::memory_order_release);
}


bool HasResolvedOfficialFFXStartupPath() {
return g_FGCompat.HasDirectFFXApiConfirmation() ||
       dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
}


void ResetProtectedOfficialFFXStartupProgressCounters() {
dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(0, std::memory_order_release);
}


void ArmProtectedOfficialFFXStartupProgressTracking(const char* reason) {
dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
dx12_hook_g_ProtectedOfficialFFXStartupBeginMs.store(GetTickCount64(), std::memory_order_release);
HookLogImportant("DX12: Protected official FFX startup progress tracking armed (%s)",
                 reason && reason[0] ? reason : "unknown");
}


void ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.store(0, std::memory_order_release);
if (dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant("DX12: Cleared progress-resolved official FFX runtime-owned Present path assumption (%s)",
                     reason && reason[0] ? reason : "unknown");
}
}


void StoreDeferredOfficialFFXTakeoverSideEffects(ID3D12CommandQueue* queue, const char* modulePath, const char* reason) {
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


ID3D12CommandQueue* ConsumeDeferredOfficialFFXTakeoverSideEffects(char* modulePathOut, size_t modulePathOutCount) {
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


ID3D12CommandQueue* ReferenceDeferredOfficialFFXTakeoverQueue() {
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


void ClearDeferredOfficialFFXTakeoverSideEffects(const char* reason) {
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


void ClearProtectedOfficialFFXStartupSwapchainPending(const char* reason) {
if (dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel)) {
    HookLogImportant("DX12: Cleared protected official FFX startup swapchain pass-through (%s)",
                     reason && reason[0] ? reason : "unknown");
}
ResetProtectedOfficialFFXStartupProgressCounters();
}


void SetNativeFSRStartupConfigureArmingPending(bool pending, const char* reason) {
const bool previous = dx12_hook_g_NativeFSRStartupConfigureArmingPending.exchange(pending, std::memory_order_acq_rel);
if (previous != pending) {
    HookLogImportant("DX12: Native FSR startup configure arming %s (%s)", pending ? "pending" : "cleared",
                     reason && reason[0] ? reason : "unknown");
}
}


bool ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
return ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(
    captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.officialAMDFFXRuntimeCreator,
    HasResolvedOfficialFFXStartupPath());
}


void StageProtectedOfficialFFXStartupQueueFromCreateDevice(IUnknown* createDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
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


bool ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup() {
return ce::dx12_overlay_policy::ShouldQuiesceCESideEffectsDuringProtectedOfficialFFXStartup(
    dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
    HasResolvedOfficialFFXStartupPath());
}


bool ShouldDeferPresentHookRefreshForPostFSRStreamlineRuntimeHandoff(IUnknown* pDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, ID3D12CommandQueue** queueOut) {
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


bool ShouldApplySwapchainDescriptorOverridesForCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
return ce::dx12_overlay_policy::ShouldApplySwapchainDescriptorOverridesForCreate(
    captureEvidence.callerFromThirdPartyOverlay,
    captureEvidence.authoritativeFFXRuntimeCreator || captureEvidence.authoritativeStreamlineRuntimeCreator);
}


void PrepareForAuthoritativeFFXSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
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


void LogSkippedSwapchainDescriptorOverridesForRuntimeCreate(const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence, UINT bufferCount, UINT flags, DXGI_SWAP_EFFECT swapEffect) {
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


bool ShouldBypassInvisibleWindowCreateSwapchainSideEffects(HWND hWnd, IDXGISwapChain* swapchain, const char* context, HRESULT hr) {
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


void QuiesceStreamlinePostSLForProtectedOfficialFFXStartup(IDXGISwapChain* swapchain, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
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


bool HandleProtectedOfficialFFXStartupSwapchainCreate(const CreateSwapchainQueueCaptureEvidence& captureEvidence, IUnknown* createDevice, IDXGISwapChain* swapchain, const char* context) {
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


void ApplyAuthoritativeFFXTakeoverSideEffects(ID3D12CommandQueue* capturedQueue, const char* callerModulePath, const char* reason) {
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


bool MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress(const char* source) {
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


void ClearStaleStreamlineOwnershipForFSRTakeover(const CreateSwapchainQueueCaptureEvidence& captureEvidence, bool runtimeOwnsSwapchain, bool runtimeOwnershipJustActivated, ID3D12CommandQueue* capturedQueue) {
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
