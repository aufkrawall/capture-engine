#include "dx12_hook_internal.h"


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


bool IsStartupOverlayCompatibilityActive() {
return ce::dx12_overlay_policy::ShouldUseStartupOverlayCompatibilityMode(
    ce::overlay_compat::GetStartupBlockingOverlayModuleName() != nullptr, IsActualFrameGenerationActive(),
    dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
    dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_FGRuntimeOwnsSwapchain);
}


bool ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff() {
return ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
    dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit,
    dx12_hook_g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
    g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
    dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_HadFSRFGPhase, dx12_hook_g_OriginalGameQueue != nullptr);
}


void UpdateStartupOverlayCompatibilityState() {
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


const char* GetStartupOverlayFirstDrawProbeStageName(StartupOverlayFirstDrawProbeStage stage) {
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


void ResetStartupOverlayBackendActivationStage() {
dx12_hook_s_startupOverlayActivationStage = StartupOverlayActivationStage::kNone;
dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kNone;
dx12_hook_s_startupOverlayActivationStageMs = 0;
dx12_hook_s_startupOverlaySyncInitMs = 0;
dx12_hook_s_startupOverlayResourcePrimeMs = 0;
dx12_hook_s_startupOverlayFirstDrawProbeMs = 0;
dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.store(0, std::memory_order_release);
}
