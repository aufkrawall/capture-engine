
        // Track whether an FG runtime owns this swapchain/queue
        bool runtimeOwns = (g_OriginalGameQueue && pQueue != g_OriginalGameQueue);

        if (authoritativeNormalSwapchainReturn) {
            runtimeOwns = false;
            if (g_OriginalGameQueue != pQueue) {
                ID3D12CommandQueue* oldOriginalGameQueue = g_OriginalGameQueue;
                g_OriginalGameQueue = pQueue;
                g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
                pQueue->AddRef();
                HookLogImportant(
                    "DX12: Re-baselined original game queue to authoritative normal-return queue %p "
                    "(was %p; the game replaced the retired Streamline presentation topology)",
                    pQueue, oldOriginalGameQueue);
                if (oldOriginalGameQueue) {
                    oldOriginalGameQueue->Release();
                }
            }
            const bool ownershipWasHeld = g_FGRuntimeOwnsSwapchain;
            g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            g_FGRuntimeOwnsSwapchainSince = 0;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
            HookLogImportant(
                "DX12: Authoritative normal swapchain return ended retired Streamline queue ownership "
                "(queue=%p origGame=%p ownershipWasHeld=%d)",
                pQueue, g_OriginalGameQueue, ownershipWasHeld ? 1 : 0);
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
                gameCreatedSwapchain, g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                g_NativeFSRContextsDestroyedAwaitingGameSwapchain.load(std::memory_order_acquire),
                g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
        if (endNativeFGTeardownOnGameSwapchainCreation) {
            runtimeOwns = false;
            g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
            g_PostNativeFSROffGameSwapchainRecoveryQueue.store(pQueue, std::memory_order_release);
            // The game retired its previous present queue and created this
            // swapchain on a fresh one with game provenance. Re-baseline the
            // original-game-queue anchor so frame classification counts the
            // game's real ECLs again (a stale dead anchor classifies every
            // present as zero-ECL/interpolated and starves ProcessFrame —
            // 20260612_000936: overlay disappeared forever after FSR->off),
            // and so future FG cycles' takeover/teardown proofs compare
            // against the queue that actually presents. Games that recreate
            // on the SAME queue (Talos-style) hit the pointer-equality no-op.
            if (g_OriginalGameQueue != pQueue) {
                ID3D12CommandQueue* oldOriginalGameQueue = g_OriginalGameQueue;
                g_OriginalGameQueue = pQueue;
                g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
                pQueue->AddRef();
                HookLogImportant(
                    "DX12: Re-baselined original game queue to game-created recovery queue %p "
                    "(was %p; old queue retired by the game itself)",
                    pQueue, oldOriginalGameQueue);
                if (oldOriginalGameQueue) {
                    oldOriginalGameQueue->Release();
                }
            }
            const bool ownershipWasHeld = g_FGRuntimeOwnsSwapchain;
            if (g_FGRuntimeOwnsSwapchain) {
                g_FGRuntimeOwnsSwapchain = false;
                DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
                g_FGRuntimeOwnsSwapchainSince = 0;
                ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            }
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
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
                pQueue, g_OriginalGameQueue, ownershipWasHeld ? 1 : 0, "game");
        }

        if (runtimeOwns && !g_FGRuntimeOwnsSwapchain) {
            g_FGRuntimeOwnsSwapchain = true;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(true, std::memory_order_release);
            g_FGRuntimeOwnsSwapchainSince = GetTickCount64();
            runtimeOwnershipJustActivated = true;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            s_pendingLateRuntimeOwnedStartupHandoff.store(true, std::memory_order_release);
            HookLogImportant(
                "DX12: FG runtime now owns swapchain queue %p (origGame=%p) — dedicated/cross-queue overlay work is "
                "disabled on this queue",
                pQueue, g_OriginalGameQueue);
        } else if (!runtimeOwns && g_FGRuntimeOwnsSwapchain) {
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
            const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
            const bool explicitNativeFSROffPending =
                g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
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
                    pQueue == g_OriginalGameQueue, explicitNativeFSROffPending, authoritativeFSRActive, runtimeMode,
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
                                 g_OriginalGameQueue, (pQueue == g_OriginalGameQueue) ? 1 : 0,
                                 g_FGRuntimeOwnsSwapchain ? 1 : 0);
                return runtimeOwnershipJustActivated;
            }

            if (endNativeFGTeardownOnOrigGame) {
                HookLogImportant(
                    "DX12: Explicit native FSR OFF plus origGame swapchain return ending runtime-owned native-FG "
                    "teardown (queue=%p origGame=%p runtime=%s nativeFGPath=%d callbackStalled=%d)",
                    pQueue, g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                    runtimeOwnedNativeFGPresentPath ? 1 : 0, ffxPresentCallbackStalled ? 1 : 0);
                g_FGCompat.SetHeuristicFSRFGActive(false);
                ResetAuthoritativeFSRRealFrameOnlyStreak();
                ForceClearNativeFSRInternalNoCallbackComposition(
                    "explicit native FSR OFF plus origGame swapchain return");
            }

            g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            g_FGRuntimeOwnsSwapchainSince = 0;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
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
        if (g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) &&
            !g_FGCompat.IsFSRFGApiActive() && g_FGCompat.GetRuntimeMode() != ce::fg_runtime::RuntimeMode::kFSRFG) {
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            HookLogImportant(
                "DX12: Swapchain queue changed to %p while FSR is no longer active — cleared stale explicit native FSR "
                "OFF teardown flag (origGame=%p runtime=%s)",
                pQueue, g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()));
        }

        HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                         g_OriginalGameQueue, (pQueue == g_OriginalGameQueue) ? 1 : 0,
                         g_FGRuntimeOwnsSwapchain ? 1 : 0);
    }

    // Only hook the vtable if this is the game's original queue (or we haven't
    // captured origGame yet).  FG runtimes (FSR FG) create their own queues and
    // rely on tight ECL timing.  Hooking their vtable adds overhead to every ECL
    // call (safety checks, heartbeat, queue tracking, lock acquisition, etc.).
    // This cumulative overhead breaks FSR FG's internal fence synchronization,
    // causing ffxQuery to spin-wait or WaitForSingleObject indefinitely.
    // We already hook origGame's queue for watchdog/heartbeat — that's sufficient.
    const bool shouldHookQueueVTable = ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(
        g_OriginalGameQueue != nullptr, pQueue == g_OriginalGameQueue, authoritativeStreamlineRuntimeQueue,
        authoritativeFFXRuntimeQueue);
    if (shouldHookQueueVTable) {
        if (authoritativeStreamlineRuntimeQueue && g_OriginalGameQueue && pQueue != g_OriginalGameQueue) {
            HookLogImportant(
                "DX12: Hooking authoritative Streamline runtime queue vtable %p (origGame=%p) to keep runtime-owned "
                "ECL tracking visible",
                pQueue, g_OriginalGameQueue);
        }
        DX12_HookQueueVTable(pQueue);
    } else {
        if (authoritativeFFXRuntimeQueue && authoritativeStreamlineRuntimeQueue && g_OriginalGameQueue &&
            pQueue != g_OriginalGameQueue) {
            HookLogImportant(
                "DX12: Skipping vtable hook for FFX-owned runtime queue %p despite stale Streamline provenance "
                "(origGame=%p) — preserving FSR timing",
                pQueue, g_OriginalGameQueue);
        } else {
            HookLogImportant("DX12: Skipping vtable hook for FG runtime queue %p (origGame=%p) — preserving FSR timing",
                             pQueue, g_OriginalGameQueue);
        }
    }

    return runtimeOwnershipJustActivated;
}

static bool IsDX12Swapchain(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    ID3D12Device* pDX12Device = nullptr;
    HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDX12Device));
    if (FAILED(hr) || !pDX12Device)
        return false;

    pDX12Device->Release();
    return true;
}

static bool InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(const char* context,
                                                                        ID3D12CommandQueue* newSwapchainQueue,
                                                                        ID3D12CommandQueue* previousSwapchainQueue,
                                                                        ID3D12CommandQueue* originalGameQueue) {
    ID3D12CommandQueue* lockedQueue = nullptr;
    ID3D12CommandQueue* lastWorkingQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        lockedQueue = g_PostSLLockedQueue;
        lastWorkingQueue = g_PostSLLastWorkingQueue;
    }

    const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
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
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
    g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);

    if (invalidateConfirmed) {
        const int previousStableFrames = g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
        g_PostSLConfirmedRendering.store(false, std::memory_order_release);
        g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
        g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
        g_PostSLOverlayActive.store(false, std::memory_order_release);
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
    if (g_State.overlayInit || g_State.syncInit) {
        std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
        overlayWasLive = g_State.overlayInit && g_State.syncInit && g_OverlayAdapter.IsInitialized();
        const bool preserveLiveOverlayDuringHandoff =
            ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
                s_startupOverlayCompatSettled.load(std::memory_order_acquire), g_State.overlayInit && g_State.syncInit,
                g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
                s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), g_HadFSRFGPhase,
                g_OriginalGameQueue != nullptr);
        if (preserveLiveOverlayDuringHandoff) {
            g_State.cachedSwapChain = nullptr;
            g_State.cachedSC3 = nullptr;
            HookLogImportant(
                "DX12: Fresh authoritative Streamline no-FG handoff preserved live overlay backend "
                "(source=%s newScQueue=%p prevScQueue=%p origGame=%p)",
                context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, originalGameQueue);
        } else {
            // The adapter is device/format scoped and does not retain swapchain buffers or submit through its
            // initialization queue. Keep it warm while retiring only the old swapchain-scoped RTV/sync state.
            // The fresh post-FSR Streamline handoff can then prewarm the replacement state before DLSS is enabled,
            // rather than rebuilding the backend inside the first generated Present.
            g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
            g_State.overlayInit = false;
            g_State.syncInit = false;
            g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
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

static void PublishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
    SetPostSLCallbackInstalled(false, reason);
    // Publish cancellation before waiting for an already-entered callback. The
    // callback compares this epoch before every GPU submission point, exits,
    // and releases the render mutex without a polling delay.
    g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
}

static int FinishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
    std::lock_guard<std::mutex> renderLock(g_PostSLRenderMutex);

    const int previousStableFrames = g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
    g_PostSLOverlayActive.store(false, std::memory_order_release);
    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(false, std::memory_order_release);
    g_PostSLStallCounter.store(0, std::memory_order_release);
    g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
    ResetPostSLLifecycleForTransition(reason, true);
    SetPostSLLastWorkingQueue(nullptr);
    ReleaseStreamlineStartupActivationSwapchain(reason);

    if (g_State.overlayInit || g_State.syncInit) {
        std::lock_guard<std::recursive_mutex> overlayLock(g_OverlayMutex);
        g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        g_State.overlayInit = false;
        g_State.syncInit = false;
        g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
        CleanupRTVs();
    }

    return previousStableFrames;
}

static int RetirePostSLRouteForNormalSwapchainReturn(const char* reason) {
    PublishPostSLRouteRetirementForNormalSwapchainReturn(reason);
    return FinishPostSLRouteRetirementForNormalSwapchainReturn(reason);
}

static bool HandlePostSLRouteForNormalSwapchainReturn(const char* context, ID3D12CommandQueue* returnedQueue,
                                                      IDXGISwapChain* returnedSwapchain,
                                                      ID3D12CommandQueue* originalGameQueue,
                                                      const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    const bool originalQueueNormalSwapchainReturn =
        ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
            captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.callerFromStreamlineFGModule,
            captureEvidence.streamlineFrameGenerationInStack,
            g_StreamlineEnableCallsInFlight.load(std::memory_order_acquire) != 0, originalGameQueue != nullptr,
            returnedQueue == originalGameQueue);
    const bool gameCreatedSwapchain =
        !captureEvidence.callerFromThirdPartyOverlay && !captureEvidence.authoritativeFFXRuntimeCreator &&
        !captureEvidence.officialAMDFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator;
    const bool gameSwapchainAfterExplicitDLSSOff =
        ce::dx12_overlay_policy::ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
            gameCreatedSwapchain, g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
            IsActualFrameGenerationActive(), DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
    const bool normalSwapchainReturn = originalQueueNormalSwapchainReturn || gameSwapchainAfterExplicitDLSSOff;
    if (!normalSwapchainReturn) {
        return false;
    }

    if (gameSwapchainAfterExplicitDLSSOff && returnedSwapchain) {
        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(returnedSwapchain, std::memory_order_release);
        HookLogImportant(
            "[OVERLAY VISIBILITY] Armed exact native swapchain takeover after authoritative DLSS OFF "
            "(swapchain=%p queue=%p)",
            returnedSwapchain, returnedQueue);
    } else {
        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
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
        lockedQueue = g_PostSLLockedQueue;
        lastWorkingQueue = g_PostSLLastWorkingQueue;
    }
    const bool routeArmed = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                            g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                            g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
                            g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || lockedQueue != nullptr ||
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

static void CaptureSwapchainQueueFromCreateDevice(IUnknown* pDevice, IDXGISwapChain* pSwapChain, const char* context,
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
            currentOriginalGameQueue = g_OriginalGameQueue;
            currentSwapchainQueue = g_SwapchainQueue;
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
            capturedOnOriginalQueue = gameCreatedSwapchain && g_OriginalGameQueue != nullptr &&
                                      pQueue == g_OriginalGameQueue && g_SwapchainQueue == pQueue &&
                                      (normalSwapchainReturn || !g_FGRuntimeOwnsSwapchain);
            if (capturedOnOriginalQueue) {
                RememberOriginalQueueSwapchainIdentity(pSwapChain, "CreateSwapChain original-queue association");
            }
        }
        if (freshAuthoritativeStreamlineHandoff) {
            if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(
                    IsDX12Swapchain(pSwapChain), freshAuthoritativeStreamlineHandoff,
                    DXGIShared::DoesFGRuntimeOwnSwapchain())) {
                DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                                "DX12: fresh authoritative Streamline handoff");
            }
            const bool retiredLiveOverlayState = InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(
                context, pQueue, currentSwapchainQueue, currentOriginalGameQueue);
            const bool hadSuccessfulPostSLPhase = g_HadSuccessfulPostSLPhase.load(std::memory_order_acquire);
            const bool prewarmPostSL = ce::dx12_overlay_policy::ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(
                freshAuthoritativeStreamlineHandoff, g_HadFSRFGPhase, hadSuccessfulPostSLPhase,
                DXGIShared::DoesFGRuntimeOwnSwapchain(),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), retiredLiveOverlayState,
                IsDX12Swapchain(pSwapChain));
            g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
            if (prewarmPostSL) {
                const bool prewarmReady = PrewarmPostSLOverlayForFreshStreamlineHandoff(pSwapChain, pQueue, context);
                if (prewarmReady) {
                    g_PrewarmedPostSLHandoffSwapchain.store(pSwapChain, std::memory_order_release);
                    HookLogImportant(
                        "[OVERLAY VISIBILITY] Armed exact prewarmed PostSL handoff backend for its first Present "
                        "(swapchain=%p queue=%p hadFSR=%d priorPostSL=%d)",
                        pSwapChain, pQueue, g_HadFSRFGPhase ? 1 : 0, hadSuccessfulPostSLPhase ? 1 : 0);
                }
            }
            DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
            DXGIShared::ArmStreamlineStartupTransitionWindow();
            StreamlineHook::OnAuthoritativeStreamlineStartupHandoff();
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                        context ? context : "DX12::AuthoritativeStreamlineStartupHandoff", pQueue,
                                        pSwapChain, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
            if (ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(
                    freshAuthoritativeStreamlineHandoff, g_HadFSRFGPhase,
                    g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr)) {
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
                            context, pQueue, (void*)g_RealD3D12ECL.load(std::memory_order_acquire), handoffDevice);
                    } else {
                        g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
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

void DX12_AdjustWrapperResizeDepth_C(int delta) {
    DX12_AdjustWrapperResizeDepth(delta);
}

// Queue-aware wrapper fallback for frame classification.
// The wrapper path is only used when the real queue has not been registered yet;
// once registration succeeds, the vtable ECL detour becomes the authoritative
// source of command-list counts.
__attribute__((noinline)) void DX12_NotifyCommandListsForQueue(ID3D12CommandQueue* pQueue, UINT numCommandLists) {
    if (!pQueue || numCommandLists == 0) {
        return;
    }

    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr) {
        return;
    }

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    ID3D12CommandQueue* classificationQueue = GetFrameClassificationQueue();
    if (!classificationQueue || pQueue != classificationQueue) {
        static std::atomic<int> s_skippedWrapperNotifyLogCount{0};
        int skipCount = s_skippedWrapperNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (skipCount < 10 || (skipCount % 2048) == 2047) {
            HookLog(
