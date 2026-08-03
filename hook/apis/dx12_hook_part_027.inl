                            currentSwapchainQueue == g_PostSLLastWorkingQueue,
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
                    const int previousCooldown = g_FGTransitionCooldown.load(std::memory_order_acquire);
                    g_FGTransitionCooldown.store(0, std::memory_order_release);
                    g_PostSLCooldownRemaining.store(0, std::memory_order_release);
                    g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    // Force sync re-init: old allocators/fence were on the old queue.
                    if (g_State.syncInit) {
                        g_State.syncInit = false;
                    }
                    if (immediateReinitAfterAuthoritativeDLSSOffNormalReturn) {
                        IDXGISwapChain* expectedSwapchain = pSwapChain;
                        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.compare_exchange_strong(
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
                            commandQueueSettledToPrimary, g_HadFSRFGPhase, slOffSwapchainGrace > 0)) {
                        // Post-FSR non-FG recovery: Streamline teardown may still be
                        // destabilizing GPU resources.  Use an extended cooldown so the
                        // overlay stays completely idle until the GPU is fully settled.
                        // The first overlay GPU submit after the cooldown can still cause
                        // DEVICE_REMOVED if Streamline teardown isn't complete, so we give
                        // a generous 15-second window.
                        cooldownFrames = ce::dx12_overlay_policy::ResolvePostFSRExtendedCooldownFrames(
                            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
                    }
                    g_FGTransitionCooldown = ce::dx12_overlay_policy::ResolveTransitionCooldownFrames(
                        g_FGTransitionCooldown.load(std::memory_order_acquire), cooldownFrames,
                        ce::dx12_overlay_policy::ShouldUseShortPostFSRInactiveCooldown(
                            commandQueueSettledToPrimary, g_HadFSRFGPhase, slOffSwapchainGrace > 0));
                    g_PostSLCooldownRemaining.store(g_FGTransitionCooldown.load(std::memory_order_acquire),
                                                    std::memory_order_release);
                    g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    g_PostSLOverlayActive.store(false, std::memory_order_release);
                    g_PostSLConfirmedRendering.store(false, std::memory_order_release);
                    // Force sync re-init: old allocators/fence were on the old queue.
                    if (g_State.syncInit) {
                        g_State.syncInit = false;
                    }
                    HookLogImportant(
                        "DX12: Swapchain change during active FG — cooldown %d frames "
                        "(fgActive=%d, fgRecentFrames=%d, slSignal=%d, prevCooldown=%d, slOffGrace=%d, "
                        "fgOwned=%d, scQueue=%p, origGame=%p cmdQ=%p primaryQ=%p)",
                        cooldownFrames, fgCurrentlyActive ? 1 : 0, g_FramesSinceFGActive,
                        DXGIShared::g_StreamlineFGRunning.load() ? 1 : 0,
                        g_FGTransitionCooldown.load(std::memory_order_acquire), slOffSwapchainGrace,
                        g_FGRuntimeOwnsSwapchain ? 1 : 0, currentSwapchainQueue, currentOriginalGameQueue,
                        currentCommandQueue, currentPrimaryQueue);
                } else {
                    HookLogImportant("DX12: Swapchain change (no FG active) — normal reinit");
                    const bool endingPostFSRNonFGRecovery =
                        g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
                    if (endingPostFSRNonFGRecovery && !postFSRNormalRouteOwnershipProven) {
                        NoteDX12OverlayCoverageGate("postfsr-normal-ownership-raced-unproven");
                        HookLogImportant(
                            "DX12: Refusing to end post-FSR recovery on a bare swapchain pointer change "
                            "(sc=%p scQueue=%p origGame=%p rememberedProof=%d explicitQueueProof=%d)",
                            pSwapChain, currentSwapchainQueue, currentOriginalGameQueue,
                            postFSRNormalRouteRememberedSwapchainProof ? 1 : 0,
                            postFSRNormalRouteExplicitQueueProof ? 1 : 0);
                        return;
                    }
                    ID3D12CommandQueue* postSLLockedQueue = nullptr;
                    ID3D12CommandQueue* postSLLastWorkingQueue = nullptr;
                    {
                        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
                        postSLLockedQueue = g_PostSLLockedQueue;
                        postSLLastWorkingQueue = g_PostSLLastWorkingQueue;
                    }
                    const bool postSLRouteArmed =
                        DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                        g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                        g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
                        g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || postSLLockedQueue != nullptr ||
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
                        g_NeedOffscreenOverlayAfterPostFSRNonFG.store(false, std::memory_order_release);
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
            g_LastSwapChain = pSwapChain;

            if (!g_Device.load()) {
                return;
            }
            HookLog("DX12: ProcessFrame - new swapchain tracked (device=%p)", g_Device.load());
        }
        if (exactPrewarmedPostSLHandoffSwapchainProof) {
            IDXGISwapChain* expectedSwapchain = pSwapChain;
            g_PrewarmedPostSLHandoffSwapchain.compare_exchange_strong(
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
    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGNow = IsFSRFrameGenerationActive();
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentPrimaryQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
        const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
        const bool postFSRInactiveRecoveryPending =
            g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
        const bool lastWorkingQueueStillActiveDuringRecentTeardown =
            g_PostSLLastWorkingQueue != nullptr &&
            GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
        if (protectedOfficialFFXStartupOverlayOnly) {
            static std::atomic<int> s_protectedOfficialFFXStartupGpuQuietLogCount{0};
            const int logCount = s_protectedOfficialFFXStartupGpuQuietLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Protected official FFX startup keeping nested real-swapchain work tracking-only; "
                    "proxy-backbuffer prework owns overlay visibility until enabled ffxConfigure/present-callback "
                    "proof (sc=%p origGame=%p oldScQueue=%p cmdQ=%p resolved=%d log=%d)",
                    pSwapChain, g_OriginalGameQueue, g_SwapchainQueue, currentCommandQueue,
                    HasResolvedOfficialFFXStartupPath() ? 1 : 0, logCount + 1);
            }
            return;
        } else {
            const auto routingDecision = ce::dx12_overlay_policy::DecideSwapchainOverlayRouting(
                g_FGRuntimeOwnsSwapchain, slFGNow, fsrFGNow, g_HadFSRFGPhase, g_SwapchainQueue != nullptr,
                g_OriginalGameQueue != nullptr, g_PostSLLastWorkingQueue != nullptr, postFSRInactiveRecoveryPending,
                currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
                g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
                g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire));

            if (routingDecision ==
                ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue) {
                static int s_fgOwnSkipLog = 0;
                ++s_fgOwnSkipLog;
                if (s_fgOwnSkipLog <= 10 || (s_fgOwnSkipLog % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — FG runtime owns swapchain but scQueue is null, SKIPPING overlay "
                        "(origGame=%p, fsrFGHeur=%d, fgOwnedSince=%llums ago) #%d",
                        g_OriginalGameQueue, fsrFGNow ? 1 : 0, GetTickCount64() - g_FGRuntimeOwnsSwapchainSince,
                        s_fgOwnSkipLog);
                }
                return;
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRStreamlineQueue) {
                // After FSR→DLSS: use scQueue (swapchain creation queue).
                // The swapchain was created on FSR's queue; backbuffers are
                // associated with it.  origGame can't access them (cross-queue).
                // SL's wrapper queue also fails.  scQueue is the ONLY queue
                // with authorized backbuffer access.
                if (g_SwapchainQueue) {
                    gameQueue = g_SwapchainQueue;
                    static bool s_loggedPostFSR = false;
                    if (!s_loggedPostFSR) {
                        s_loggedPostFSR = true;
                        HookLogImportant(
                            "DX12: ProcessFrame — post-FSR SL FG, using scQueue %p (swapchain creation queue, "
                            "origGame=%p)",
                            gameQueue, g_OriginalGameQueue);
                    }
                } else {
                    // Shouldn't happen — scQueue should be kept alive during hadFSR
                    gameQueue = g_OriginalGameQueue;
                    HookLogImportant("DX12: ProcessFrame — post-FSR SL FG but scQueue is null, fallback to origGame %p",
                                     gameQueue);
                }
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUseStreamlineOriginalQueue) {
                // SL FG (no FSR history): use origGame.
                gameQueue = g_OriginalGameQueue;
            } else if (routingDecision ==
                       ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kUsePostFSRInactiveLastWorkingQueue) {
                // During the explicit post-FSR inactive recovery epoch with
                // scQueue intentionally unset, reuse the last queue that already
                // proved it could render the still-live transition swapchain.
                gameQueue = g_PostSLLastWorkingQueue;
                static std::atomic<int> s_postFSRProcessFrameLastWorkingRouteLogCount{0};
                int logCount = s_postFSRProcessFrameLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
                if (logCount < 10 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "DX12: ProcessFrame — post-FSR inactive recovery epoch using preserved PostSL lastWorking "
                        "queue %p (cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                        g_PostSLLastWorkingQueue, currentCommandQueue, g_OriginalGameQueue, currentPrimaryQueue,
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
                    ce::dx12_overlay_policy::DecidePostFSRInactiveRecoveryQueueSource(g_OriginalGameQueue != nullptr);
                if (queueSource == ce::dx12_overlay_policy::PostFSRInactiveRecoveryQueueSource::kOriginalPresentQueue) {
                    gameQueue = g_OriginalGameQueue;
                    const bool explicitNativeFSROffPending =
                        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
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
                gameQueue = g_SwapchainQueue;
                if (!g_HadFSRFGPhase &&
                    ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), true)) {
                    g_HadFSRFGPhase = true;
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
                        true, g_SwapchainQueue != nullptr, g_FGRuntimeOwnsSwapchain, runtimeOwnedSwapchainActiveMs,
                        kStartupOverlayPostResumeSettleMs,
                        s_startupOverlayCompatSettled.load(std::memory_order_acquire),
                        ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff());
                const bool useOriginalQueueForStartupCompat =
                    startupCompatCanUseSettledRuntimeOwnedQueue && g_OriginalGameQueue != nullptr;
                gameQueue = useOriginalQueueForStartupCompat ? g_OriginalGameQueue : g_SwapchainQueue;
                static int s_runtimeOwnedQueueLogCount = 0;
                ++s_runtimeOwnedQueueLogCount;
                if (s_runtimeOwnedQueueLogCount <= 10 || (s_runtimeOwnedQueueLogCount % 300) == 0) {
                    const bool authoritativeFSR = g_FGCompat.IsFSRFGApiActive();
                    HookLogImportant(
                        "DX12: ProcessFrame — runtime-owned swapchain %s, using %s %p "
                        "(origGame=%p slFG=%d hadFSR=%d apiFSR=%d startupCompatSettled=%d) #%d",
                        authoritativeFSR ? "with authoritative FSR FG state" : "without FSR evidence",
                        useOriginalQueueForStartupCompat ? "origGame" : "scQueue", gameQueue, g_OriginalGameQueue,
                        slFGNow ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, authoritativeFSR ? 1 : 0,
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
                        g_OriginalGameQueue, s_fsrSkipLog);
                }
                return;
            } else {
                gameQueue = g_SwapchainQueue;
                if (!gameQueue)
                    gameQueue = g_CommandQueue.load();
            }
        }
    }
    if (!gameQueue) {
        HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return;
    }
    // Log queue selection decision (rate-limited: first 10, then every 300)
    {
        static int s_queueLogCount = 0;
        ++s_queueLogCount;
        if (s_queueLogCount <= 10 || (s_queueLogCount % 300) == 0) {
            bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
            bool fsrFGActive = IsFSRFrameGenerationActive();
            const char* qPath = "unknown";
            if (slFGNow && g_OriginalGameQueue && gameQueue == g_OriginalGameQueue)
                qPath = "origGame(SL-FG)";
            else if (fsrFGActive && gameQueue == g_SwapchainQueue)
                qPath = "scQueue(FSR-FG)";
            else if (fsrFGActive && gameQueue == g_OriginalGameQueue)
                qPath = "origGame(FSR-FG-fallback)";
            else if (!slFGNow && !fsrFGActive && g_HadFSRFGPhase && !g_SwapchainQueue && g_PostSLLastWorkingQueue &&
                     gameQueue == g_PostSLLastWorkingQueue)
                qPath = "lastWorking(post-FSR)";
            else if (!slFGNow && !fsrFGActive && g_HadFSRFGPhase && !g_SwapchainQueue && g_OriginalGameQueue &&
                     gameQueue == g_OriginalGameQueue)
                qPath = "origGame(post-FSR)";
            else if (gameQueue == g_SwapchainQueue)
                qPath = "scQueue";
            else if (gameQueue == g_OriginalGameQueue)
                qPath = "origGame";
            else if (gameQueue == g_PrimaryGameQueue.load(std::memory_order_acquire))
                qPath = "primaryQ";
            else if (gameQueue == g_CommandQueue.load(std::memory_order_acquire))
                qPath = "cmdQueue";
            else
                qPath = "otherQ";
            HookLogImportant(
                "DX12: ProcessFrame queue=%p (slFG=%d fsrFG=%d origQ=%p primaryQ=%p scQ=%p cmdQ=%p lastWorkingQ=%p "
                "path=%s) #%d",
                gameQueue, slFGNow ? 1 : 0, fsrFGActive ? 1 : 0, g_OriginalGameQueue,
                g_PrimaryGameQueue.load(std::memory_order_acquire), g_SwapchainQueue, (void*)g_CommandQueue.load(),
                g_PostSLLastWorkingQueue, qPath, s_queueLogCount);
        }
    }

    // Track the game's Present thread ID for pre-SL overlay rendering.
    // During SL FG, SL's worker threads also call Present (for generated frames).
    // Pre-SL overlay must ONLY run on the game thread — SL's workers call Present
    // at the wrong timing (during FG frame Present, not game frame Present).
    {
        DWORD currentTid = GetCurrentThreadId();
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (!slFGNow) {
            // When SL FG is NOT active, the current thread IS the game thread.
            // Update the tracked ID (game might switch render threads).
            g_GamePresentThreadId.store(currentTid, std::memory_order_release);
            g_RenderWatchdog.SetMonitoredThread(currentTid);
        }
    }

    // Capture the game's original queue ONCE before any FG activation.
    // This queue is guaranteed to be the game's own D3D12 queue (not SL's).
    // During FG transitions, g_SwapchainQueue and g_CommandQueue can both
    // get polluted by SL/FSR internal queues (via CreateSwapChainForHwnd
    // and ECL hooks respectively).
    if (!g_OriginalGameQueue) {
        g_OriginalGameQueue = gameQueue;
        g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
        gameQueue->AddRef();  // prevent queue from being freed during FG transitions
        HookLogImportant("DX12: Captured original game queue %p (sc=%p cmd=%p)", gameQueue, g_SwapchainQueue,
                         (void*)g_CommandQueue.load());
    }

    bool currentSwapchainProvenOnOriginalQueue = false;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        currentSwapchainProvenOnOriginalQueue =
            !IsActualFrameGenerationActive() && !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
            !g_FGRuntimeOwnsSwapchain && g_OriginalGameQueue != nullptr && g_SwapchainQueue != nullptr &&
            g_SwapchainQueue == g_OriginalGameQueue && gameQueue == g_OriginalGameQueue &&
            g_LastSwapchainQueueCaptureSwapchain.load(std::memory_order_acquire) == pSwapChain;
    }
    if (currentSwapchainProvenOnOriginalQueue) {
        RememberOriginalQueueSwapchainIdentity(pSwapChain, "normal Present on captured original queue");
    }

    // Queue-change-based FG detection: FSR FG creates its own command queue
    // and reroutes all ECL calls through it.  Detecting a queue pointer change
    // after the first few frames is a strong signal that FG has activated.
    //
    // IMPORTANT: FSR FG alternates between origGame queue and FSR's internal
    // queue every frame (real frame vs interpolated frame).  We use hysteresis
    // to avoid rapid on/off oscillation:
    //   - Activation: trigger immediately on first queue change
    //   - Deactivation: require N CONSECUTIVE frames on initial queue
    //
    // CRITICAL: Use the RAW command queue (g_CommandQueue from ECL hook), NOT
    // the overridden gameQueue.  When FG is active, gameQueue is forced to
    // g_OriginalGameQueue, which would mask FSR's queue alternation and cause
    // the deactivation counter to fire incorrectly.
    {
        static ID3D12CommandQueue* s_initialQueue = nullptr;
        static ID3D12CommandQueue* s_currentFGQueue = nullptr;
        static int s_queueFrameCount = 0;
        static int s_consecutiveInitialQueueFrames = 0;
        constexpr int kDeactivationThreshold = 120;  // ~2s at 60fps before declaring FG off

        // Decrement SL OFF heuristic grace once per ProcessFrame (not per ECL call).
        int slGrace = g_SLOffHeuristicGrace.load(std::memory_order_acquire);
        if (slGrace > 0) {
            g_SLOffHeuristicGrace.store(slGrace - 1, std::memory_order_release);
            // Force-clear any lingering heuristic FSR_FG during the grace window.
            // CanUseFSRFGHeuristics blocks new detections, but a stale true from
            // before SL FG activated can persist because no code path overwrites it.
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
            // Also suppress phantom NVIDIA_SM re-detection during the grace window.
            // ClearNvidiaSMState resets the confirm counter and cached multiplier,
            // but DetectPattern can re-detect within 3 frames if the multiplier
            // rebuilds from recent frame history.  Force-clear each frame.
            if (IsNvidiaSmoothMotionActiveRuntime()) {
                g_FGCompat.ClearNvidiaSMState();
                static int s_phantomSMClears = 0;
                if (s_phantomSMClears++ < 5)
                    HookLogImportant("DX12: Cleared phantom NVIDIA_SM during SL grace (remaining=%d)", slGrace - 1);
            }
        }

        // FG transition handler sets this flag to force a full reset.
        // Without this, SL's leftover queue persists in s_initialQueue/
        // s_currentFGQueue and immediately re-triggers false FSR FG detection.
        if (g_ResetQueueChangeHeuristic.exchange(false, std::memory_order_acquire)) {
            ID3D12CommandQueue* authoritativeBaseline =
                g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire);
            // After SL FG OFF, SL may have created a new swapchain on a
            // different queue.  The game continues using SL's swapchain queue
            // even after FG teardown.  Anchoring to origGame would permanently
            // see the new queue as "different" → endless false FSR_FG.
            //
            // Ordinary transitions allow recapture from the next five frames.
            // A proven normal swapchain return instead pins the baseline to its
            // authoritative game queue and waits for that queue to be observed.
            HookLog(
                "DX12: Queue-change heuristic reset (FG transition) — "
                "was initial=%p fgQ=%p frame=%d authoritativeBaseline=%p",
                s_initialQueue, s_currentFGQueue, s_queueFrameCount, authoritativeBaseline);
            s_initialQueue = authoritativeBaseline;
            s_currentFGQueue = nullptr;
            s_queueFrameCount = authoritativeBaseline ? 5 : 0;
            s_consecutiveInitialQueueFrames = 0;
        }

        ID3D12CommandQueue* rawQueue = g_CommandQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* authoritativeBaseline =
            g_QueueChangeHeuristicAuthoritativeBaseline.load(std::memory_order_acquire);
        const bool awaitAuthoritativeBaseline = ce::dx12_overlay_policy::ShouldAwaitAuthoritativeQueueChangeBaseline(
            authoritativeBaseline != nullptr, rawQueue == authoritativeBaseline);
        if (awaitAuthoritativeBaseline) {
            static std::atomic<int> s_authoritativeBaselineWaitLogCount{0};
            const int logCount = s_authoritativeBaselineWaitLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 256) == 0) {
                HookLogImportant(
                    "DX12: Ignoring leftover queue traffic while normal swapchain return awaits authoritative "
                    "baseline (raw=%p baseline=%p scQueue=%p origGame=%p count=%d)",
                    rawQueue, authoritativeBaseline, g_SwapchainQueue, g_OriginalGameQueue, logCount + 1);
            }
            if (g_FGCompat.IsHeuristicFSRFGActive()) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
        } else {
            bool mayEvaluateQueueChange = true;
            if (authoritativeBaseline) {
                ID3D12CommandQueue* expectedBaseline = authoritativeBaseline;
                if (g_QueueChangeHeuristicAuthoritativeBaseline.compare_exchange_strong(
                        expectedBaseline, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    s_initialQueue = rawQueue;
                    s_currentFGQueue = nullptr;
                    s_queueFrameCount = 5;
                    s_consecutiveInitialQueueFrames = 0;
                    HookLogImportant(
                        "DX12: Established authoritative queue-change baseline after normal swapchain return "
                        "(baseline=%p scQueue=%p origGame=%p)",
                        rawQueue, g_SwapchainQueue, g_OriginalGameQueue);
                } else {
                    // A concurrent lifecycle event replaced or consumed this
                    // epoch. Do not evaluate the current queue against stale
                    // function-local state; the winning epoch owns the reset.
                    mayEvaluateQueueChange = false;
                }
            }

            if (mayEvaluateQueueChange)
                ++s_queueFrameCount;
            if (mayEvaluateQueueChange && s_queueFrameCount <= 5) {
                // Capture initial queue during first 5 frames (before FG activates)
                s_initialQueue = rawQueue;
            } else if (mayEvaluateQueueChange && s_initialQueue) {
                const bool recentStreamlineTeardown = g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
                ID3D12CommandQueue* currentSwapchainQueue = nullptr;
                {
                    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
                    currentSwapchainQueue = g_SwapchainQueue;
                }
                const bool lastWorkingQueueStillActiveDuringRecentTeardown =
                    g_PostSLLastWorkingQueue != nullptr &&
                    GetTickCount64() < g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
                const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
                    g_HadFSRFGPhase, g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(),
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    currentSwapchainQueue != nullptr);
                const bool ignoreQueueChangeDuringRecentTeardown =
                    rawQueue &&
                    ce::dx12_overlay_policy::ShouldIgnoreQueueChangeHeuristicDuringRecentStreamlineTeardown(
                        recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
                        rawQueue == g_PrimaryGameQueue.load(std::memory_order_acquire), rawQueue == g_OriginalGameQueue,
                        rawQueue == currentSwapchainQueue, rawQueue == g_PostSLLastWorkingQueue);

                if (ignoreQueueChangeDuringRecentTeardown) {
                    static std::atomic<int> s_recentTeardownQueueChangeIgnoreLogCount{0};
                    const int logCount =
                        s_recentTeardownQueueChangeIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 20 || (logCount % 256) == 0) {
                        HookLogImportant(
                            "DX12: Ignoring queue-change heuristic on teardown/recovery queue %p "
                            "(initial=%p orig=%p primary=%p scQ=%p lastWorking=%p slOffGrace=%d postSLRecent=%d "
                            "postFSR=%d frame=%d)",
                            rawQueue, s_initialQueue, g_OriginalGameQueue,
                            g_PrimaryGameQueue.load(std::memory_order_acquire), currentSwapchainQueue,
                            g_PostSLLastWorkingQueue, g_SLOffHeuristicGrace.load(std::memory_order_acquire),
                            lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0,
                            s_queueFrameCount);
                    }
                    s_consecutiveInitialQueueFrames = 0;
                } else {
                    bool isFGQueue = (rawQueue != s_initialQueue);
                    if (isFGQueue) {
                        // Reset consecutive-initial counter — we just saw the FG queue
                        s_consecutiveInitialQueueFrames = 0;

                        if (!s_currentFGQueue) {
                            if (UpdateHeuristicFSRFGState(true, "queue-change")) {
                                // Queue changed away from initial → FSR FG activated
                                s_currentFGQueue = rawQueue;
                                HookLogImportant(
                                    "DX12: FG detected via queue change "
                                    "(initial=%p, current=%p, gameQ=%p, frame=%d)",
                                    s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                            } else {
                                static std::atomic<int> s_ignoredQueueChangeLogCount{0};
                                if (s_ignoredQueueChangeLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                                    HookLog(
                                        "DX12: Ignoring queue change heuristic "
                                        "(initial=%p, current=%p, rawQ=%p, frame=%d)",
                                        s_initialQueue, rawQueue, gameQueue, s_queueFrameCount);
                                }
                            }
                        }
                        // else: FG already active, FG queue seen again — normal FSR FG alternation
                    } else {
                        // Seeing initial queue. During FSR FG this happens every other frame.
                        // Only deactivate after many CONSECUTIVE initial-queue frames.
                        if (s_currentFGQueue) {
                            ++s_consecutiveInitialQueueFrames;
                            if (s_consecutiveInitialQueueFrames >= kDeactivationThreshold) {
                                HookLogImportant(
                                    "DX12: FG deactivated via queue revert after %d consecutive initial-queue "
                                    "frames (initial=%p, fgQueue=%p, frame=%d)",
                                    s_consecutiveInitialQueueFrames, s_initialQueue, s_currentFGQueue,
                                    s_queueFrameCount);
                                s_currentFGQueue = nullptr;
