#include "dx12_hook_internal.h"
#include "hook/fg/fg_presenter.h"
#include "hook/runtime/hook_clock.h"
#include "common/logging/log_meter.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::TrackSwapchainAndSelectQueue() {
if (processLogicalSwapchainReplacement) {
    if (pSwapChain == dx12_hook_g_LastSwapChain &&
        (exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof ||
         exactGameSwapchainRecoverySwapchainProof)) {
        HookLogImportant(
            "[OVERLAY VISIBILITY] Authoritative %s swapchain creation reused the previous COM pointer "
            "address; processing it as a new lifetime (swapchain=%p)",
            exactGameSwapchainRecoverySwapchainProof
                ? "game-created FSR-off recovery"
            : exactPostDLSSOffNormalReturnSwapchainProof ? "native-return" : "prewarmed-PostSL",
            pSwapChain);
    }
    if (exactGameSwapchainRecoverySwapchainProof) {
        IDXGISwapChain* expectedSwapchain = pSwapChain;
        dx12_hook_g_ExactGameSwapchainRecoverySwapchain.compare_exchange_strong(
            expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    // The prewarmed identity is consumed below; end the recovery now so every
    // later Present of this swapchain stays off the GPU-quiet gate.
    EndPostFSRNonFGRecoveryOnProvenSwapchainChange(false, exactPrewarmedPostSLHandoffSwapchainProof);
    if (exactPostDLSSOffNormalReturnSwapchainProof) {
        // One-shot consume at the TOP of the replacement handler: an ABA-equal pointer plus a
        // still-armed proof used to reprocess the replacement on EVERY present (the old consume
        // only ran inside the immediate-reinit branch, so the "no FG active" normal-reinit path
        // re-armed a per-present teardown/rebuild storm — sessions 20260813_162959 and
        // 20260813_164314: up to 191 consecutive "processing it as a new lifetime" rounds).
        IDXGISwapChain* expectedSwapchain = pSwapChain;
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.compare_exchange_strong(
            expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    bool deferredFreshStreamlineNoFGSwapchainCleanup = false;
    bool preserveConfirmedPostSLSwapchainChange = false;
    if (dx12_hook_g_LastSwapChain) {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const uint32_t streamlineNoFGPresentCount =
            dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.load(std::memory_order_acquire);
        const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        deferredFreshStreamlineNoFGSwapchainCleanup =
            ce::dx12_overlay_policy::ShouldSuppressFreshRuntimeOwnedStreamlineNoFGSeparateOverlayWork(
                dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, streamlineNoFGPresentCount,
                dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents);
        const bool preserveLiveStreamlineNoFGOverlayResources =
            ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
        ID3D12CommandQueue* preserveSwapchainQueue = nullptr;
        ID3D12CommandQueue* preserveOriginalGameQueue = nullptr;
        ID3D12CommandQueue* preserveCommandQueue = nullptr;
        ID3D12CommandQueue* preserveLastWorkingPostSLQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            preserveSwapchainQueue = dx12_hook_g_SwapchainQueue;
            preserveOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            preserveCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
            preserveLastWorkingPostSLQueue = g_PostSLQueues.LastDeviceHealthyQueue();
        }
        const bool postSLConfirmedForSwapchainChange = g_PostSLLifecycle.RouteConfirmed();
        const int postSLStableFramesForSwapchainChange = dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire);
        const bool confirmedPostSLBackendWarmupProtected =
            ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLBackendAsWarmupProtected(
                postSLConfirmedForSwapchainChange, postSLStableFramesForSwapchainChange);
        preserveConfirmedPostSLSwapchainChange =
            ce::dx12_overlay_policy::ShouldPreserveConfirmedPostSLBackendDuringActiveFGSwapchainChange(
                streamlineFGRunning, postSLConfirmedForSwapchainChange, confirmedPostSLBackendWarmupProtected,
                dx12_hook_g_HadFSRFGPhase, dx12_hook_g_FGRuntimeOwnsSwapchain, preserveSwapchainQueue != nullptr,
                preserveOriginalGameQueue != nullptr,
                preserveSwapchainQueue != nullptr && preserveOriginalGameQueue != nullptr &&
                    preserveSwapchainQueue != preserveOriginalGameQueue,
                g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                pSwapChain != nullptr &&
                    pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire),
                preserveLastWorkingPostSLQueue != nullptr,
                dx12_hook_g_PostSLWarmResumePreservationPending.load(std::memory_order_acquire));
        const bool preserveProtectedOfficialFFXStartupSwapchainChange =
            ce::dx12_overlay_policy::ShouldPreserveOverlayBackendAcrossProtectedOfficialFFXStartupSwapchainChange(
                dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
                HasResolvedOfficialFFXStartupPath());
        auto* prewarmedHandoffDevice = g_Device.load(std::memory_order_acquire);
        const bool prewarmedHandoffDeviceRemoved =
            prewarmedHandoffDevice != nullptr && FAILED(prewarmedHandoffDevice->GetDeviceRemovedReason());
        const bool preserveExactPrewarmedPostSLHandoffBackend =
            ce::dx12_overlay_policy::ShouldPreserveExactPrewarmedPostSLHandoffBackendOnFirstPresent(
                exactPrewarmedPostSLHandoffSwapchainProof, dx12_hook_g_State.overlayInit, dx12_hook_g_State.syncInit,
                dx12_hook_g_State.rtvDescHeap != nullptr, dx12_hook_g_State.cmdList != nullptr, dx12_hook_g_FGRuntimeOwnsSwapchain,
                preserveSwapchainQueue != nullptr,
                dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain,
                g_FGCompat.IsFSRFGApiActive(), HookHasRuntimeOwnedNativeFGPresentPath(),
                prewarmedHandoffDeviceRemoved);
        if (exactPrewarmedPostSLHandoffSwapchainProof) {
            IDXGISwapChain* expectedSwapchain = pSwapChain;
            dx12_hook_g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
                expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if (preserveExactPrewarmedPostSLHandoffBackend) {
            exactPrewarmedPostSLHandoffBackendPreservedThisPresent = true;
            HookLogImportant(
                "[OVERLAY VISIBILITY] First exact prewarmed PostSL handoff Present preserved its ready "
                "overlay backend (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p)",
                dx12_hook_g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                preserveCommandQueue);
        } else if (preserveProtectedOfficialFFXStartupSwapchainChange) {
            // Keep the old backend warm without retargeting it to an unproven nested FFX swapchain.
            // Proxy-backbuffer prework supplies startup visibility until enabled configure resolves
            // the route; the normal backend can then be rebound by the established transition path.
            dx12_hook_g_State.cachedSwapChain = nullptr;
            dx12_hook_g_State.cachedSC3 = nullptr;
            static std::atomic<int> s_preservedProtectedFFXStartupSwapchainCleanupLogCount{0};
            const int logCount =
                s_preservedProtectedFFXStartupSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Preserving overlay backend across protected official FFX startup swapchain change "
                    "until enabled ffxConfigure/present-callback proof (oldSC=%p newSC=%p scQueue=%p "
                    "origGame=%p cmdQ=%p log=%d)",
                    dx12_hook_g_LastSwapChain, pSwapChain, preserveSwapchainQueue, preserveOriginalGameQueue,
                    preserveCommandQueue, logCount + 1);
            }
        } else if (preserveLiveStreamlineNoFGOverlayResources) {
            dx12_hook_g_State.cachedSwapChain = nullptr;
            dx12_hook_g_State.cachedSC3 = nullptr;
            static std::atomic<int> s_preservedFreshSLNoFGSwapchainCleanupLogCount{0};
            const int logCount =
                s_preservedFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Preserving live overlay resources during runtime-inactive Streamline no-FG "
                    "swapchain handoff (oldSC=%p newSC=%p scQueue=%p origGame=%p cmdQ=%p log=%d)",
                    dx12_hook_g_LastSwapChain, pSwapChain, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                    g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
            }
        } else if (preserveConfirmedPostSLSwapchainChange) {
            dx12_hook_g_State.cachedSwapChain = nullptr;
            dx12_hook_g_State.cachedSC3 = nullptr;
            static std::atomic<int> s_preservedConfirmedPostSLSwapchainCleanupLogCount{0};
            const int logCount =
                s_preservedConfirmedPostSLSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Preserving confirmed PostSL backend during active Streamline FG swapchain change "
                    "(oldSC=%p newSC=%p stableFrames=%d warmupProtected=%d hadFSR=%d fgOwned=%d scQueue=%p "
                    "origGame=%p cmdQ=%p lastWorking=%p exactSuccessful=%d cooldown=%d log=%d)",
                    dx12_hook_g_LastSwapChain, pSwapChain, postSLStableFramesForSwapchainChange,
                    confirmedPostSLBackendWarmupProtected ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0,
                    dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, preserveSwapchainQueue, preserveOriginalGameQueue,
                    preserveCommandQueue, preserveLastWorkingPostSLQueue,
                    pSwapChain == dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_relaxed) ? 1 : 0,
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), logCount + 1);
            }
        } else if (deferredFreshStreamlineNoFGSwapchainCleanup) {
            static std::atomic<int> s_deferredFreshSLNoFGSwapchainCleanupLogCount{0};
            const int logCount =
                s_deferredFreshSLNoFGSwapchainCleanupLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Deferring swapchain-change cleanup during fresh runtime-owned Streamline no-FG "
                    "handoff (presentCount=%u settlePresents=%u oldSC=%p newSC=%p scQueue=%p origGame=%p "
                    "cmdQ=%p log=%d)",
                    streamlineNoFGPresentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, dx12_hook_g_LastSwapChain,
                    pSwapChain, dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                    g_CommandQueue.load(std::memory_order_acquire), logCount + 1);
            }
        } else {
            CleanupRTVs();
            {
                std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
                dx12_hook_g_SharedCaptureD3D12.Reset();
            }
            dx12_hook_g_State.overlayInit = false;
            ResetStartupOverlayBackendActivationStage();

            // FG TRANSITION PROTECTION: If FG is currently active (or was recently
            // active per the cooldown), the swapchain change is likely caused by an
            // FG mode switch (e.g., FSR FG → DLSS FG).  SL / FSR runtimes need time
            // to finish initializing before we reinit overlay resources on the new
            // swapchain.  Set a transition cooldown so the reinit path (below) defers
            // until the FG runtime is stable.
            bool fgCurrentlyActive = IsActualFrameGenerationActive() ||
                                     DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            // Also protect if FG was active within the last ~5 seconds (~300 frames).
            // When switching FG modes, the game may disable one FG type many frames
            // before the swapchain actually changes.  Heuristic detection goes inactive
            // immediately, but the swapchain change is delayed.
            constexpr int kFGRecentWindowFrames = 300;
            bool fgRecentlyWasActive = (dx12_hook_g_FramesSinceFGActive < kFGRecentWindowFrames);
            ID3D12CommandQueue* currentSwapchainQueue = nullptr;
            ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
            ID3D12CommandQueue* currentCommandQueue = nullptr;
            ID3D12CommandQueue* currentPrimaryQueue = nullptr;
            IDXGISwapChain* currentQueueAssociatedSwapchain = nullptr;
            {
                std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
                currentOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
                currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
                currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
                currentQueueAssociatedSwapchain =
                    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire);
            }
            postFSRNormalRouteExplicitQueueProof =
                currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                currentSwapchainQueue == currentOriginalGameQueue && currentQueueAssociatedSwapchain == pSwapChain;
            postFSRNormalRouteRememberedSwapchainProof =
                dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire) == pSwapChain;
            postFSRNormalRouteOwnershipProven = ce::dx12_overlay_policy::IsPostFSRNormalRouteOwnershipProven(
                currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                currentSwapchainQueue != nullptr && currentSwapchainQueue == currentOriginalGameQueue,
                currentQueueAssociatedSwapchain == pSwapChain, postFSRNormalRouteRememberedSwapchainProof);
            int slOffSwapchainGrace = dx12_hook_g_SLOffSwapchainReinitGrace.load(std::memory_order_acquire);
            const bool commandQueueSettledToPrimary =
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue;
            bool guardSwapchainReinit = ce::dx12_overlay_policy::ShouldGuardSwapchainReinitAfterChange(
                fgCurrentlyActive, fgRecentlyWasActive, dx12_hook_g_FGTransitionCooldown > 0, slOffSwapchainGrace > 0,
                dx12_hook_g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr, currentOriginalGameQueue != nullptr,
                currentSwapchainQueue != nullptr && currentOriginalGameQueue != nullptr &&
                    currentSwapchainQueue != currentOriginalGameQueue);
            const bool immediateReinitAfterNoCallbackFFXTakeover =
                ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterNoCallbackFFXTakeoverSwapchainChange(
                    g_FGCompat.HasDirectFFXApiConfirmation(), g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    dx12_hook_g_FGRuntimeOwnsSwapchain, currentSwapchainQueue != nullptr,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
            const bool immediateReinitAfterGameSwapchainRecovery =
                ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff(
                    currentSwapchainQueue != nullptr && dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.load(
                                                            std::memory_order_acquire) == currentSwapchainQueue,
                    g_FGCompat.IsFSRFGApiActive(),
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
            auto* swapchainChangeDevice = g_Device.load(std::memory_order_acquire);
            const bool swapchainChangeDeviceRemoved =
                swapchainChangeDevice != nullptr && FAILED(swapchainChangeDevice->GetDeviceRemovedReason());
            const bool immediateReinitOnFGInactiveOriginalQueue =
                ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyOnFGInactiveOriginalQueueSwapchain(
                    postFSRNormalRouteOwnershipProven, fgCurrentlyActive,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    dx12_hook_g_FGRuntimeOwnsSwapchain, swapchainChangeDeviceRemoved);
            const bool immediateReinitAfterAuthoritativeDLSSOffNormalReturn =
                exactPostDLSSOffNormalReturnSwapchainProof && immediateReinitOnFGInactiveOriginalQueue;
            authoritativeDLSSOffNormalReturnReinitializedThisPresent =
                immediateReinitAfterAuthoritativeDLSSOffNormalReturn;
            nativeFSRGameSwapchainRecoveryReinitializedThisPresent =
                immediateReinitAfterGameSwapchainRecovery;
            // DLSS-FG SUSPEND (slDLSSGSetOptions(off), proxy stays live): the active-FG
            // preserve path can't fire (streamlineFGRunning already false), so a fresh
            // proxy swapchain pointer on the same live queue used to blank the live overlay
            // for the full 90-frame cooldown. The make-before-break keep-alive latch marks
            // a CONFIRMED PostSL path that is merely suspended (never set during an FSR/
            // native-FG takeover), so reinit the warm backend immediately on its live queue.
            const bool immediateReinitAfterConfirmedPostSLSuspension = ce::dx12_overlay_policy::
                ShouldReinitOverlayImmediatelyAfterConfirmedPostSLSuspensionSwapchainChange(
                    dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    dx12_hook_g_FGRuntimeOwnsSwapchain,
                    currentSwapchainQueue != nullptr && currentCommandQueue != nullptr &&
                        currentSwapchainQueue == currentCommandQueue,
                    // The DLSS-G proxy renders the overlay on g_PostSLLastWorkingQueue
                    // (== scQueue), which persists across a suspend even when the live
                    // wrapper cmdQueue differs. Accept it as the confirmed PostSL queue.
                    currentSwapchainQueue != nullptr && g_PostSLQueues.LastDeviceHealthyQueue() != nullptr &&
                        currentSwapchainQueue == g_PostSLQueues.LastDeviceHealthyQueue());
            // DLSS-FG OFF over a runtime-owned (FSR-history) swapchain whose ownership latch is
            // stale: DLSS-PostSL was the actual presenter (change queue == g_PostSLLastWorkingQueue),
            // but the keep-alive could not arm (blocked by runtimeOwnedNativeFGPresentPath), so the
            // suspension predicate above misses it. Reinit warm backend immediately on the same queue
            // instead of waiting for the 90-frame cooldown.
            const ULONGLONG lastFFXCallbackTickMs = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
            const bool ffxPresentCallbackActiveForDLSSOff =
                lastFFXCallbackTickMs != 0 && (ce::hook_clock::TickCount64() - lastFFXCallbackTickMs) < 1000;
            const bool immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue = ce::dx12_overlay_policy::
                ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    ffxPresentCallbackActiveForDLSSOff, dx12_hook_g_FGRuntimeOwnsSwapchain,
                    currentSwapchainQueue != nullptr && g_PostSLQueues.LastDeviceHealthyQueue() != nullptr &&

                        currentSwapchainQueue == g_PostSLQueues.LastDeviceHealthyQueue(),
                    swapchainChangeDeviceRemoved);
            // The unguarded branch below ends the recovery in order with PostSL
            // retirement; a guarded change only defers the reinit, so a proven
            // normal return has to end it here.
            if (guardSwapchainReinit) {
                EndPostFSRNonFGRecoveryOnProvenSwapchainChange(postFSRNormalRouteOwnershipProven, false);
            }
            if (guardSwapchainReinit &&
                (immediateReinitAfterNoCallbackFFXTakeover || immediateReinitAfterGameSwapchainRecovery ||
                 immediateReinitOnFGInactiveOriginalQueue ||
                 immediateReinitAfterConfirmedPostSLSuspension ||
                 immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue)) {
                // Enable direction: the enabled ffxConfigure already finalized
                // the official FFX takeover, applied the staged runtime queue,
                // and drained CE's overlay GPU work; normal overlay rendering on
                // the runtime-owned swapchain queue is the approved transport
                // for the no-callback route. Off direction: the game-created
                // recovery swapchain already ended the runtime-owned teardown
                // and its queue was captured at creation. Either way, rebuild
                // the overlay immediately instead of blanking it for the
                // generic transition cooldown.
                const int previousCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
                EndFGTransitionCooldown();
                dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                g_PostSLLifecycle.SuspendRoute();
                g_PostSLLifecycle.InvalidateRouteProof();
                // Force sync re-init: old allocators/fence were on the old queue.
                if (dx12_hook_g_State.syncInit) {
                    dx12_hook_g_State.syncInit = false;
                }
                HookLogImportant(
                    "DX12: Swapchain change is %s — immediate overlay "
                    "reinit on its captured queue instead of FG transition cooldown "
                    "(scQueue=%p origGame=%p cmdQ=%p prevCooldown=%d)",
                    immediateReinitAfterNoCallbackFFXTakeover ? "finalized no-callback official FFX takeover"
                    : immediateReinitAfterAuthoritativeDLSSOffNormalReturn
                        ? "authoritative DLSS-off native swapchain return (exact route, no blank)"
                    : immediateReinitOnFGInactiveOriginalQueue ? "proven on the game queue with FG inactive (no blank)"
                    : immediateReinitAfterConfirmedPostSLSuspension
                        ? "confirmed-PostSL DLSS-FG suspension (proxy stays live, no blank)"
                    : immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue
                        ? "DLSS-off over confirmed-PostSL runtime-owned queue (FSR latch stale, callback quiet)"
                        : "game-created swapchain recovery after explicit native FSR OFF/destroy",
                    currentSwapchainQueue, currentOriginalGameQueue, currentCommandQueue, previousCooldown);
            } else if (guardSwapchainReinit) {
                int cooldownFrames = 90;  // ~1.5s at 60fps — longer than normal transition
                if (ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                        commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, slOffSwapchainGrace > 0)) {
                    // Post-FSR non-FG recovery: Streamline teardown may still be
                    // destabilizing GPU resources.  Use an extended cooldown so the
                    // overlay stays completely idle until the GPU is fully settled.
                    // The first overlay GPU submit after the cooldown can still cause
                    // DEVICE_REMOVED if Streamline teardown isn't complete, so we give
                    // a generous 15-second window.
                    cooldownFrames = ce::dx12_overlay_policy::ResolvePostFSRExtendedCooldownFrames(
                        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                }
                dx12_hook_g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                    ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                        commandQueueSettledToPrimary, dx12_hook_g_HadFSRFGPhase, slOffSwapchainGrace > 0));
                dx12_hook_g_PostSLCooldownRemaining.store(dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                std::memory_order_release);
                dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                g_PostSLLifecycle.SuspendRoute();
                g_PostSLLifecycle.InvalidateRouteProof();
                // Force sync re-init: old allocators/fence were on the old queue.
                if (dx12_hook_g_State.syncInit) {
                    dx12_hook_g_State.syncInit = false;
                }
                HookLogImportant(
                    "DX12: Swapchain change during active FG — cooldown %d frames "
                    "(fgActive=%d, fgRecentFrames=%d, slSignal=%d, prevCooldown=%d, slOffGrace=%d, "
                    "fgOwned=%d, scQueue=%p, origGame=%p cmdQ=%p primaryQ=%p)",
                    cooldownFrames, fgCurrentlyActive ? 1 : 0, dx12_hook_g_FramesSinceFGActive,
                    DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0,
                    dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire), slOffSwapchainGrace,
                    dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, currentSwapchainQueue, currentOriginalGameQueue,
                    currentCommandQueue, currentPrimaryQueue);
            } else {
                HookLogImportant("DX12: Swapchain change (no FG active) — normal reinit");
                const bool endingPostFSRNonFGRecovery =
                    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
                if (endingPostFSRNonFGRecovery && !postFSRNormalRouteOwnershipProven) {
                    NoteDX12OverlayCoverageGate("postfsr-normal-ownership-raced-unproven");
                    HookLogImportant(
                        "DX12: Refusing to end post-FSR recovery on a bare swapchain pointer change "
                        "(sc=%p scQueue=%p origGame=%p rememberedProof=%d explicitQueueProof=%d)",
                        pSwapChain, currentSwapchainQueue, currentOriginalGameQueue,
                        postFSRNormalRouteRememberedSwapchainProof ? 1 : 0,
                        postFSRNormalRouteExplicitQueueProof ? 1 : 0);
        return ProcessFrameFlow::kReturn;
                }
                ID3D12CommandQueue* postSLLockedQueue = nullptr;
                ID3D12CommandQueue* postSLLastWorkingQueue = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                    postSLLockedQueue = g_PostSLQueues.SelectedQueue();
                    postSLLastWorkingQueue = g_PostSLQueues.LastDeviceHealthyQueue();
                }
                const bool postSLRouteArmed =
                    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                    g_PostSLLifecycle.RouteActive() ||
                    g_PostSLLifecycle.RouteConfirmed() ||
                    dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || postSLLockedQueue != nullptr ||
                    postSLLastWorkingQueue != nullptr;
                const bool hasDistinctPostSLQueueProof =
                    currentOriginalGameQueue != nullptr &&
                    ((postSLLockedQueue && postSLLockedQueue != currentOriginalGameQueue) ||
                     (postSLLastWorkingQueue && postSLLastWorkingQueue != currentOriginalGameQueue));
                const bool retirePostSLRoute =
                    ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(
                        endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven, postSLRouteArmed,
                        hasDistinctPostSLQueueProof);

                if (retirePostSLRoute) {
                    PublishPostSLRouteRetirementForNormalSwapchainReturn("DX12: clean non-FG Present return");
                }
                if (endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven) {
                    // Publish the authoritative normal-return boundary before
                    // waiting for an already-entered PostSL callback. Concurrent
                    // routing must immediately stop treating its historical queue
                    // as eligible for the replacement swapchain.
                    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.store(false, std::memory_order_release);
                }
                if (retirePostSLRoute) {
                    // PostSL owns its render mutex before it can enter overlay
                    // initialization (render -> overlay lock order). Release
                    // ProcessFrame's overlay lock while the cancellation epoch
                    // drains an already-entered callback, then reacquire it
                    // before rebuilding/drawing on the normal route below.
                    const int previousStableFrames = renderAdmission.RetireOutsideLock([&] {
                        return FinishPostSLRouteRetirementForNormalSwapchainReturn(
                            "DX12: clean non-FG Present return");
                    });
                    HookLogImportant(
                        "DX12: Clean non-FG Present return retired stale PostSL route before normal overlay "
                        "reinit (locked=%p lastWorking=%p origGame=%p stableFrames=%d)",
                        postSLLockedQueue, postSLLastWorkingQueue, currentOriginalGameQueue, previousStableFrames);
                }
                if (endingPostFSRNonFGRecovery && postFSRNormalRouteExplicitQueueProof) {
                    HookLogImportant(
                        "DX12: Ended post-FSR non-FG recovery on explicit swapchain-queue proof "
                        "(scQueue=%p matches origGame=%p)",
                        currentSwapchainQueue, currentOriginalGameQueue);
                } else if (endingPostFSRNonFGRecovery && postFSRNormalRouteRememberedSwapchainProof) {
                    HookLogImportant(
                        "DX12: Ended post-FSR non-FG recovery on remembered exact original-queue swapchain "
                        "identity (sc=%p origGame=%p)",
                        pSwapChain, currentOriginalGameQueue);
                } else {
                    HookLogImportant("DX12: Ordinary non-FG swapchain change outside post-FSR recovery");
                }
                if (ce::dx12_overlay_policy::ShouldResetQueueChangeHeuristicAfterCleanNonFGSwapchainChange(
                        endingPostFSRNonFGRecovery && postFSRNormalRouteOwnershipProven)) {
                    RequestFGDetectionHeuristicReset();
                    if (g_FGCompat.IsHeuristicFSRFGActive()) {
                        g_FGCompat.SetHeuristicFSRFGActive(false);
                    }
                    HookLogImportant(
                        "DX12: Reset queue-change heuristic after clean non-FG swapchain transition ending "
                        "post-FSR "
                        "recovery");
                }
            }
        }
    }
    if (!deferredFreshStreamlineNoFGSwapchainCleanup) {
        // Store raw pointer for change detection only - no AddRef to avoid
        // interfering with FSR FG's swapchain lifecycle management
        dx12_hook_g_LastSwapChain = pSwapChain;

        if (!g_Device.load()) {
        return ProcessFrameFlow::kReturn;
        }
        HookLog("DX12: ProcessFrame - new swapchain tracked (device=%p)", g_Device.load());
    }
    if (exactPrewarmedPostSLHandoffSwapchainProof) {
        IDXGISwapChain* expectedSwapchain = pSwapChain;
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
            expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
    }
}
return SelectFrameQueue();
}
