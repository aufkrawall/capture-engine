#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"


PostSLFlow PostSLRenderSession::Chunk0() {
static std::atomic<int> s_postSLCalls{0};
static std::atomic<int> s_postSLSkipLock{0};
static std::atomic<int> s_postSLSkipOther{0};
const bool normalRouteDrawPendingAtEntry = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire) !=
                                           dx12_hook_g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);
if (!dx12_hook_g_PostSLRenderMutex.try_lock()) {
    DX12_NoteSkippedStreamlineFinalOutput();
    s_postSLSkipLock.fetch_add(1, std::memory_order_relaxed);
    NoteDX12OverlayCoverageGate("postsl-render-lock");
    static int s_lockSkip = 0;
    if (s_lockSkip++ < 10)
        HookLogImportant("DX12: PostSL SKIP — another thread already rendering (tid=0x%04X)", GetCurrentThreadId());
        return PostSLFlow::kReturn;
}
auto renderLockGuard = ce::make_scope_guard([]() { dx12_hook_g_PostSLRenderMutex.unlock(); });
SharedMemoryLayout* finalOutputShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
finalOutputCapture =
    DX12_PlanStreamlineFinalOutputCapture(finalOutputShm, GetActiveDX12OverlayConfig(finalOutputShm));
entryLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
cachedSLFGActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
constexpr ULONGLONG kDormantProcessFrameThresholdMs = 100;
const ULONGLONG nowMs = GetTickCount64();
const ULONGLONG lastProcessFrameTickMs = dx12_hook_g_LastProcessFrameTickMs.load(std::memory_order_acquire);
processFrameRecentlySeen = lastProcessFrameTickMs != 0 && nowMs >= lastProcessFrameTickMs &&
                                      (nowMs - lastProcessFrameTickMs) < kDormantProcessFrameThresholdMs;
const bool startupTopLevelPresentConsumed =
    DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
const int startupWrapperProgressCount =
    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire);
const bool useTopLevelHandoffWrapperProgress =
    ce::dx12_overlay_policy::ShouldUseTopLevelHandoffWrapperProgressForSyntheticPostSLActivation(
        dx12_hook_g_HadFSRFGPhase, startupTopLevelPresentConsumed, startupWrapperProgressCount > 0);
safePostFSRBootstrapPathForPostSL = HookHasSafePostFSRBootstrapPath();
explicitEnablePureDLSSColdStartProof = ce::dx12_overlay_policy::HasExplicitEnablePureDLSSColdStartProof(
    dx12_hook_g_HadFSRFGPhase, HookHasExplicitStreamlineSetOptionsActivation(),
    HasRetainedStreamlineStartupActivationSwapchain(),
    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr);
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
static bool s_wasActive = false;
static uint32_t s_seenLifecycleEpoch = 0;
static HANDLE s_dedicatedFenceEvent = nullptr;
static ID3D12Fence* s_dedicatedSyncFence = nullptr;
static UINT64 s_dedicatedSyncFenceValue = 0;
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
keepAliveRenderAfterExplicitOff =
    ce::dx12_overlay_policy::ShouldAllowPostSLKeepAliveRenderAfterExplicitOff(
        dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire), cachedSLFGActive, IsStreamlineLoaded());
exactExplicitOffKeepAliveSwapchain =
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
        return PostSLFlow::kReturn;
}
if (keepAliveRenderAfterExplicitOff) {
    const int slGrace = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire);
    if (slGrace > 0) {
        dx12_hook_g_SLOffHeuristicGrace.store(slGrace - 1, std::memory_order_release);
    }
    static std::atomic<int> s_keepAliveRenderLogCount{0};
    const int logCount = s_keepAliveRenderLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 200) == 0) {
        HookLogImportant(
            "DX12: PostSL keep-alive render after explicit Streamline OFF #%d (confirmed=%d active=%d grace=%d)",
            logCount + 1, dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed) ? 1 : 0,
            dx12_hook_g_PostSLOverlayActive.load(std::memory_order_relaxed) ? 1 : 0,
            slGrace > 0 ? slGrace - 1 : 0);
    }
}
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
        return PostSLFlow::kReturn;
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
        return PostSLFlow::kReturn;
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
        return PostSLFlow::kReturn;
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
        return PostSLFlow::kReturn;
}
uint32_t lifecycleEpoch = entryLifecycleEpoch;
bool lifecycleChanged = lifecycleEpoch != s_seenLifecycleEpoch;
if (lifecycleChanged) {
    s_wasActive = false;
    s_seenLifecycleEpoch = lifecycleEpoch;
}
active = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
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
if (!active && !keepAliveRenderAfterExplicitOff) {
    s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
    NoteDX12OverlayCoverageGate("postsl-inactive");
    static int s_gateSkip = 0;
    if (s_gateSkip++ < 5)
        HookLog("DX12: PostSL SKIP — g_PostSLOverlayActive=false");
        return PostSLFlow::kReturn;
}
int cooldownLeft = dx12_hook_g_PostSLCooldownRemaining.load(std::memory_order_acquire);
if (cooldownLeft > 0 && !keepAliveRenderAfterExplicitOff) {
    s_postSLSkipOther.fetch_add(1, std::memory_order_relaxed);
    NoteDX12OverlayCoverageGate("postsl-fg-transition-cooldown");
    static int s_cooldownSkip = 0;
    if (s_cooldownSkip++ < 5)
        HookLog("DX12: PostSL SKIP — FG transition cooldown active (%d frames left)", cooldownLeft);
        return PostSLFlow::kReturn;
}
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
        return PostSLFlow::kReturn;
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
        return PostSLFlow::kReturn;
}
dev = dx12_hook_g_State.syncDevice;
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
        return PostSLFlow::kReturn;
}
if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed))
        return PostSLFlow::kReturn;
devReason = dev->GetDeviceRemovedReason();
    return PostSLFlow::kContinue;
}
