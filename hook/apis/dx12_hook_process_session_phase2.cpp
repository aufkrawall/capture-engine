#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::Phase2() {
if (processLogicalSwapchainReplacement) {
    if (pSwapChain == dx12_hook_g_LastSwapChain &&
        (exactPostDLSSOffNormalReturnSwapchainProof || exactPrewarmedPostSLHandoffSwapchainProof)) {
        HookLogImportant(
            "[OVERLAY VISIBILITY] Authoritative %s swapchain creation reused the previous COM pointer "
            "address; processing it as a new lifetime (swapchain=%p)",
            exactPostDLSSOffNormalReturnSwapchainProof ? "native-return" : "prewarmed-PostSL", pSwapChain);
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
            preserveLastWorkingPostSLQueue = dx12_hook_g_PostSLLastWorkingQueue;
        }
        const bool postSLConfirmedForSwapchainChange = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
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
            const bool immediateReinitAfterAuthoritativeDLSSOffNormalReturn =
                ce::dx12_overlay_policy::ShouldReinitOverlayImmediatelyAfterAuthoritativeDLSSOffNormalReturn(
                    exactPostDLSSOffNormalReturnSwapchainProof, postFSRNormalRouteOwnershipProven,
                    fgCurrentlyActive, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    dx12_hook_g_FGRuntimeOwnsSwapchain, swapchainChangeDeviceRemoved);
            authoritativeDLSSOffNormalReturnReinitializedThisPresent =
                immediateReinitAfterAuthoritativeDLSSOffNormalReturn;
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
                    currentSwapchainQueue != nullptr && dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
                        currentSwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue);
            // DLSS-FG OFF over a runtime-owned (FSR-history) swapchain whose ownership latch is
            // STALE: DLSS-PostSL was the actual presenter (change queue == g_PostSLLastWorkingQueue),
            // but the keep-alive could not arm (blocked by runtimeOwnedNativeFGPresentPath), so the
            // suspension predicate above misses it. FSR is not actually presenting (api inactive,
            // present callback quiet), so reinit the warm backend immediately on the same queue
            // instead of the 90-frame cooldown (session 20260614_023730: 89/90-present blanks).
            const ULONGLONG lastFFXCallbackTickMs = dx12_hook_g_LastFFXPresentCallbackTickMs.load(std::memory_order_acquire);
            const bool ffxPresentCallbackActiveForDLSSOff =
                lastFFXCallbackTickMs != 0 && (GetTickCount64() - lastFFXCallbackTickMs) < 1000;
            const bool immediateReinitAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue = ce::dx12_overlay_policy::
                ShouldReinitOverlayImmediatelyAfterDLSSOffOnConfirmedPostSLRuntimeOwnedQueue(
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    g_FGCompat.IsFSRFGApiActive(),
                    dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                    ffxPresentCallbackActiveForDLSSOff, dx12_hook_g_FGRuntimeOwnsSwapchain,
                    currentSwapchainQueue != nullptr && dx12_hook_g_PostSLLastWorkingQueue != nullptr &&

                        currentSwapchainQueue == dx12_hook_g_PostSLLastWorkingQueue,
                    swapchainChangeDeviceRemoved);
            if (guardSwapchainReinit &&
                (immediateReinitAfterNoCallbackFFXTakeover || immediateReinitAfterGameSwapchainRecovery ||
                 immediateReinitAfterAuthoritativeDLSSOffNormalReturn ||
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
                dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
                dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                // Force sync re-init: old allocators/fence were on the old queue.
                if (dx12_hook_g_State.syncInit) {
                    dx12_hook_g_State.syncInit = false;
                }
                if (immediateReinitAfterAuthoritativeDLSSOffNormalReturn) {
                    IDXGISwapChain* expectedSwapchain = pSwapChain;
                    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.compare_exchange_strong(
                        expectedSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
                }
                HookLogImportant(
                    "DX12: Swapchain change is %s — immediate overlay "
                    "reinit on its captured queue instead of FG transition cooldown "
                    "(scQueue=%p origGame=%p cmdQ=%p prevCooldown=%d)",
                    immediateReinitAfterNoCallbackFFXTakeover ? "finalized no-callback official FFX takeover"
                    : immediateReinitAfterAuthoritativeDLSSOffNormalReturn
                        ? "authoritative DLSS-off native swapchain return (exact route, no blank)"
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
                dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
                dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
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
                    postSLLockedQueue = dx12_hook_g_PostSLLockedQueue;
                    postSLLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
                }
                const bool postSLRouteArmed =
                    DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                    dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                    dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
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
                    lock.unlock();
                    const int previousStableFrames =
                        FinishPostSLRouteRetirementForNormalSwapchainReturn("DX12: clean non-FG Present return");
                    lock.lock();
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

// Prefer the swapchain queue(captured at creation time) so that our
// RENDER_TARGET -> PRESENT barrier executes on the queue DXGI syncs with.
// Fall back to the last observed direct queue if it was not captured yet.
//
// EXCEPTION: During SL DLSS FG, g_SwapchainQueue may have been overwritten
// by SL's CreateSwapChainForHwnd (SL creates its own swapchain with its
// internal queue).  In that case, use g_OriginalGameQueue — the game's
// real queue captured before any FG ever activated.
//
// FSR FG: FSR creates a NEW swapchain with its own queue. Our Present
// detour sees pSwapChain = FSR's swapchain, so GetBuffer returns FSR's
// backbuffers.  We MUST submit on the swapchain's associated queue
// (g_SwapchainQueue = FSR's queue) — submitting on origGame causes
// cross-queue resource access without synchronization → DEVICE_REMOVED.
gameQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
    bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    bool fsrFGNow = IsFSRFrameGenerationActive();
    ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* currentPrimaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
    const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    const bool postFSRInactiveRecoveryPending =
        dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
    const bool lastWorkingQueueStillActiveDuringRecentTeardown =
        dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
        GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    if (protectedOfficialFFXStartupOverlayOnly) {
        static std::atomic<int> s_protectedOfficialFFXStartupGpuQuietLogCount{0};
        const int logCount = s_protectedOfficialFFXStartupGpuQuietLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup keeping nested real-swapchain work tracking-only; "
                "proxy-backbuffer prework owns overlay visibility until enabled ffxConfigure/present-callback "
                "proof (sc=%p origGame=%p oldScQueue=%p cmdQ=%p resolved=%d log=%d)",
                pSwapChain, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, currentCommandQueue,
                HasResolvedOfficialFFXStartupPath() ? 1 : 0, logCount + 1);
        }
        return ProcessFrameFlow::kReturn;
    } else {
        const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
            dx12_hook_g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, dx12_hook_g_HadFSRFGPhase, dx12_hook_g_SwapchainQueue != nullptr,
            dx12_hook_g_OriginalGameQueue != nullptr, dx12_hook_g_PostSLLastWorkingQueue != nullptr, postFSRInactiveRecoveryPending,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
            dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
            dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire));

        if (routingDecision ==
            ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue) {
            static int s_fgOwnSkipLog = 0;
            ++s_fgOwnSkipLog;
            if (s_fgOwnSkipLog <= 10 || (s_fgOwnSkipLog % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — FG runtime owns swapchain but scQueue is null, SKIPPING overlay "
                    "(origGame=%p, fsrFGHeur=%d, fgOwnedSince=%llums ago) #%d",
                    dx12_hook_g_OriginalGameQueue, fsrFGNow ? 1 : 0, GetTickCount64() - dx12_hook_g_FGRuntimeOwnsSwapchainSince,
                    s_fgOwnSkipLog);
            }
        return ProcessFrameFlow::kReturn;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
            // After FSR→DLSS: use scQueue (swapchain creation queue).
            // The swapchain was created on FSR's queue; backbuffers are
            // associated with it.  origGame can't access them (cross-queue).
            // SL's wrapper queue also fails.  scQueue is the ONLY queue
            // with authorized backbuffer access.
            if (dx12_hook_g_SwapchainQueue) {
                gameQueue = dx12_hook_g_SwapchainQueue;
                static bool s_loggedPostFSR = false;
                if (!s_loggedPostFSR) {
                    s_loggedPostFSR = true;
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR SL FG, using scQueue %p (swapchain creation queue, "
                        "origGame=%p)",
                        gameQueue, dx12_hook_g_OriginalGameQueue);
                }
            } else {
                // Shouldn't happen — scQueue should be kept alive during hadFSR
                gameQueue = dx12_hook_g_OriginalGameQueue;
                HookLogImportant("DX12: ProcessFrame — post-FSR SL FG but scQueue is null, fallback to origGame %p",
                                 gameQueue);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
            // SL FG (no FSR history): use origGame.
            gameQueue = dx12_hook_g_OriginalGameQueue;
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
            // During the explicit post-FSR inactive recovery epoch with
            // scQueue intentionally unset, reuse the last queue that already
            // proved it could render the still-live transition swapchain.
            gameQueue = dx12_hook_g_PostSLLastWorkingQueue;
            static std::atomic<int> s_postFSRProcessFrameLastWorkingRouteLogCount{0};
            int logCount = s_postFSRProcessFrameLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — post-FSR inactive recovery epoch using preserved PostSL lastWorking "
                    "queue %p (cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                    dx12_hook_g_PostSLLastWorkingQueue, currentCommandQueue, dx12_hook_g_OriginalGameQueue, currentPrimaryQueue,
                    lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveOriginalQueue) {
            // After FSR->DLSS->off, or an explicit native-FSR OFF/suspend while
            // the stale FSR swapchain queue latch is still draining, prefer the
            // known original Present queue over the most recent ECL queue. Talos
            // uses separate render/present DIRECT queues; falling back to
            // g_CommandQueue/primary picked the render queue and immediately hit
            // DEVICE_REMOVED on the first recovered non-FG offscreen composite.
            const auto queueSource =
                ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource(dx12_hook_g_OriginalGameQueue != nullptr);
            if (queueSource == ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue) {
                gameQueue = dx12_hook_g_OriginalGameQueue;
                const bool explicitNativeFSROffPending =
                    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
                static std::atomic<int> s_postFSRInactiveOrigRouteLogCount{0};
                int logCount = s_postFSRInactiveOrigRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR normal/recovery routing using original present queue %p "
                        "(cmdQ=%p primaryQ=%p recoveryPending=%d explicitNativeOff=%d)",
                        gameQueue, currentCommandQueue, currentPrimaryQueue, postFSRInactiveRecoveryPending ? 1 : 0,
                        explicitNativeFSROffPending ? 1 : 0);
                }
            } else {
                gameQueue = currentCommandQueue ? currentCommandQueue : currentPrimaryQueue;
                static std::atomic<int> s_postFSRInactiveFallbackRouteLogCount{0};
                int logCount = s_postFSRInactiveFallbackRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR inactive recovery missing origGame, falling back to current "
                        "command queue %p (primaryQ=%p)",
                        gameQueue, currentPrimaryQueue);
                }
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseFSRSwapchainQueue) {
            // FSR FG: pSwapChain is FSR's swapchain, backbuffers belong to
            // FSR's queue.  Submit on the swapchain queue to avoid cross-queue
            // resource state conflicts.  We use realECL to bypass FSR's ECL
            // hook on this queue.
            gameQueue = dx12_hook_g_SwapchainQueue;
            if (!dx12_hook_g_HadFSRFGPhase &&
                ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), true)) {
                dx12_hook_g_HadFSRFGPhase = true;
                HookLogImportant(
                    "DX12: ProcessFrame — FSR FG history confirmed, origGame potentially corrupted for future DLSS "
                    "FG");
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseRuntimeOwnedSwapchainQueue) {
            // Runtime-owned swapchain without FSR evidence. This covers DLSS/
            // Streamline suspend-resume windows where the live swapchain stays on
            // a non-game queue but must NOT be promoted into post-FSR recovery.
            const bool startupCompatCanUseSettledRuntimeOwnedQueue =
                startupOverlayCompatibilityActive &&
                ce::dx12_overlay_policy::ShouldAllowStartupOverlayRendering(
                    true, dx12_hook_g_SwapchainQueue != nullptr, dx12_hook_g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
                    dx12_hook_kStartupOverlayPostResumeSettleMs,
                    dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire),
                    ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
            const bool useOriginalQueueForStartupCompat =
                startupCompatCanUseSettledRuntimeOwnedQueue && dx12_hook_g_OriginalGameQueue != nullptr;
            gameQueue = useOriginalQueueForStartupCompat ? dx12_hook_g_OriginalGameQueue : dx12_hook_g_SwapchainQueue;
            static int s_runtimeOwnedQueueLogCount = 0;
            ++s_runtimeOwnedQueueLogCount;
            if (s_runtimeOwnedQueueLogCount <= 10 || (s_runtimeOwnedQueueLogCount % 300) == 0) {
                const bool authoritativeFSR = g_FGCompat.IsFSRFGApiActive();
                HookLogImportant(
                    "DX12: ProcessFrame — runtime-owned swapchain %s, using %s %p "
                    "(origGame=%p slFG=%d hadFSR=%d apiFSR=%d startupCompatSettled=%d) #%d",
                    authoritativeFSR ? "with authoritative FSR FG state" : "without FSR evidence",
                    useOriginalQueueForStartupCompat ? "origGame" : "scQueue", gameQueue, dx12_hook_g_OriginalGameQueue,
                    slFGNow ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0, authoritativeFSR ? 1 : 0,
                    startupCompatCanUseSettledRuntimeOwnedQueue ? 1 : 0, s_runtimeOwnedQueueLogCount);
            }
        } else if (routingDecision ==
                   ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipFSRWithoutSwapchainQueue) {
            // FSR FG active but g_SwapchainQueue not captured.
            // DO NOT fall back to origGame — FSR FG uses origGame internally
            // and injecting our ECLs on it will corrupt FSR's fence tracking,
            // causing an internal FSR deadlock (ffxQuery spin-wait or WaitForSingleObject).
            // Instead, skip rendering entirely until scQueue is recaptured.
            static int s_fsrSkipLog = 0;
            ++s_fsrSkipLog;
            if (s_fsrSkipLog <= 5 || (s_fsrSkipLog % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — FSR FG active but scQueue=null, SKIPPING overlay (origGame=%p used by "
                    "FSR, "
                    "#%d)",
                    dx12_hook_g_OriginalGameQueue, s_fsrSkipLog);
            }
        return ProcessFrameFlow::kReturn;
        } else {
            gameQueue = dx12_hook_g_SwapchainQueue;
            if (!gameQueue)
                gameQueue = g_CommandQueue.load();
        }
    }
}
if (!gameQueue) {
    HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return ProcessFrameFlow::kReturn;
}
// Log queue selection decision (rate-limited: first 10, then every 300)
{
    static int s_queueLogCount = 0;
    ++s_queueLogCount;
    if (s_queueLogCount <= 10 || (s_queueLogCount % 300) == 0) {
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGActive = IsFSRFrameGenerationActive();
        const char* qPath = "unknown";
        if (slFGNow && dx12_hook_g_OriginalGameQueue && gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame(SL-FG)";
        else if (fsrFGActive && gameQueue == dx12_hook_g_SwapchainQueue)
            qPath = "scQueue(FSR-FG)";
        else if (fsrFGActive && gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame(FSR-FG-fallback)";
        else if (!slFGNow && !fsrFGActive && dx12_hook_g_HadFSRFGPhase && !dx12_hook_g_SwapchainQueue && dx12_hook_g_PostSLLastWorkingQueue &&
                 gameQueue == dx12_hook_g_PostSLLastWorkingQueue)
            qPath = "lastWorking(post-FSR)";
        else if (!slFGNow && !fsrFGActive && dx12_hook_g_HadFSRFGPhase && !dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue &&
                 gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame(post-FSR)";
        else if (gameQueue == dx12_hook_g_SwapchainQueue)
            qPath = "scQueue";
        else if (gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame";
        else if (gameQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire))
            qPath = "primaryQ";
        else if (gameQueue == g_CommandQueue.load(std::memory_order_acquire))
            qPath = "cmdQueue";
        else
            qPath = "otherQ";
        HookLogImportant(
            "DX12: ProcessFrame queue=%p (slFG=%d fsrFG=%d origQ=%p primaryQ=%p scQ=%p cmdQ=%p lastWorkingQ=%p "
            "path=%s) #%d",
            gameQueue, slFGNow ? 1 : 0, fsrFGActive ? 1 : 0, dx12_hook_g_OriginalGameQueue,
            dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_SwapchainQueue, (void*)g_CommandQueue.load(),
            dx12_hook_g_PostSLLastWorkingQueue, qPath, s_queueLogCount);
    }
}

// Track the application's source Present thread. Streamline and FFX can both
// call Present from runtime-owned workers; never let one of those calls replace
// the provenance used by overlay and pacing policy.
{
    DWORD currentTid = GetCurrentThreadId();
    if (applicationSourcePresent) {
        // Non-FG calls are source Presents by definition and can establish or
        // refresh the thread. During FG this can only rewrite the same proven
        // source ID; unknown/worker provenance remains unable to self-promote.
        dx12_hook_g_GamePresentThreadId.store(currentTid, std::memory_order_release);
        g_RenderWatchdog.SetMonitoredThread(currentTid);
    }
}

// Capture the game's original queue ONCE before any FG activation.
// This queue is guaranteed to be the game's own D3D12 queue (not SL's).
// During FG transitions, g_SwapchainQueue and g_CommandQueue can both
// get polluted by SL/FSR internal queues (via CreateSwapChainForHwnd
// and ECL hooks respectively).
if (!dx12_hook_g_OriginalGameQueue) {
    dx12_hook_g_OriginalGameQueue = gameQueue;
    dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
    gameQueue->AddRef();  // prevent queue from being freed during FG transitions
    HookLogImportant("DX12: Captured original game queue %p (sc=%p cmd=%p)", gameQueue, dx12_hook_g_SwapchainQueue,
                     (void*)g_CommandQueue.load());
}

currentSwapchainProvenOnOriginalQueue = false;
{
    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
    currentSwapchainProvenOnOriginalQueue =
        !IsActualFrameGenerationActive() && !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
        !dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_OriginalGameQueue != nullptr && dx12_hook_g_SwapchainQueue != nullptr &&
        dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue && gameQueue == dx12_hook_g_OriginalGameQueue &&
        dx12_hook_g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain;
}
if (currentSwapchainProvenOnOriginalQueue) {
    RememberOriginalQueueSwapchainIdentity(pSwapChain, "normal Present on captured original queue");
}
    return ProcessFrameFlow::kContinue;
}
