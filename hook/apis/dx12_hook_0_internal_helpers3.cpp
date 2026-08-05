#include "dx12_hook_internal.h"


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


OverlayConfig GetActiveDX12OverlayConfig(SharedMemoryLayout* shm) {
OverlayConfig cfg{};
cfg.captureIncludeOverlay = true;
cfg.screenshotIncludeOverlay = true;
if (shm) {
    cfg = shm->ReadOverlayConfig();
}
return cfg;
}


bool IsDX12ObserverOnlyModeActive(SharedMemoryLayout* shm) {
return IsOverlayObserverOnly(GetActiveDX12OverlayConfig(shm));
}


bool IsDX12ObserverPolicyOnlyModeActive(SharedMemoryLayout* shm) {
return IsOverlayObserverPolicyOnly(GetActiveDX12OverlayConfig(shm));
}


bool IsDX12ObserverStartupPresentOnlyModeActive(SharedMemoryLayout* shm) {
return IsOverlayObserverStartupPresentOnly(GetActiveDX12OverlayConfig(shm));
}


void EnsurePostSLDisabledForObserverOnly(const char* reason, bool preserveStartupTransitionWindow) {
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


bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg) {
return cfg.showOverlay && dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) &&
       dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}


void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm, uint64_t requestId, ID3D12CommandQueue* queueOverride) {
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


void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx, UINT currentBackBufferIdx) {
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


bool CanUseFSRFGHeuristics(const char** blockedReason) {
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


bool IsFFXPresentCallbackStalled() {
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


ProgressResolvedOfficialFFXOverlayFallbackProof EvaluateProgressResolvedOfficialFFXOverlayFallbackProof() {
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


void ResetFFXPresentCallbackFirstStallDetection() {
dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.store(0, std::memory_order_release);
}


ULONGLONG GetFFXPresentCallbackStallDurationMs() {
const ULONGLONG firstStallMs = dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs.load(std::memory_order_acquire);
if (firstStallMs == 0) {
    return 0;
}
const ULONGLONG now = GetTickCount64();
return (now >= firstStallMs) ? (now - firstStallMs) : 0;
}


void UpdateFFXPresentCallbackFirstStallDetection(bool ffxPresentCallbackStalled) {
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


bool ShouldAllowNormalOverlayFallbackForCurrentFFXPresentCallbackStall(bool ffxPresentCallbackStalled) {
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


void LogSuppressedFFXPresentCallbackStallNormalOverlayFallback() {
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

