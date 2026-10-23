#include "dx12_hook_internal.h"
#include "hook/fg/fg_presenter.h"
#include "hook/runtime/hook_clock.h"
#include "common/logging/log_meter.h"
#include "dx12_hook_process_session.h"

// Stage 2b: the queue this frame's overlay work is submitted on, after TrackSwapchainAndSelectQueue handled a
// swapchain replacement.
ProcessFrameFlow FrameProcessSession::SelectFrameQueue() {
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
    const bool postFSRInactiveRecoveryPending =
        dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire);
    const bool lastWorkingQueueStillActiveDuringRecentTeardown =
        g_PostSLQueues.LastDeviceHealthyQueue() != nullptr &&
        ce::hook_clock::TickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
    // A swapchain a frame-generation runtime presents itself (XeFG, ce::fg_presenter) is an output chain like a
    // present interposer's: its back buffers belong to the queue the runtime presents on.
    const bool runtimeOwnedOutputChain =
        ce::fg_presenter::IsRuntimeOwnedOutputPresenter(ce::fg_presenter::PresenterFor(pSwapChain));
    ID3D12CommandQueue* const interposerOutputQueue = runtimeOwnedOutputChain
                                                          ? ce::fg_presenter::OutputQueue(pSwapChain)
                                                          : DXGIShared::DX12_GetPresentInterposerOutputQueue(pSwapChain);
    const bool interposerPrivateChain =
        runtimeOwnedOutputChain || DXGIShared::DX12_IsPresentInterposerPrivateSwapchain(pSwapChain);
    if (ce::dx12_overlay_policy::ShouldUsePresentInterposerOutputQueue(interposerPrivateChain,
                                                                      interposerOutputQueue != nullptr)) {
        // pSwapChain is the present interposer's own output chain (NVIDIA Smooth Motion), so its
        // backbuffers belong to the queue that created it. This is the same rule as the FSR
        // swapchain queue below, and the game queue here is exactly the cross-queue access that
        // removes the device with DXGI_ERROR_ACCESS_DENIED.
        gameQueue = interposerOutputQueue;
        static std::atomic<int> s_interposerQueueLogCount{0};
        const int logCount = s_interposerQueueLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: ProcessFrame — %s output chain %p, using its own queue %p "
                "(origGame=%p primaryQ=%p cmdQ=%p) #%d",
                runtimeOwnedOutputChain ? "runtime-presented" : "present interposer", pSwapChain, gameQueue,
                dx12_hook_g_OriginalGameQueue,
                dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), (void*)g_CommandQueue.load(),
                logCount + 1);
        }
    } else if (ce::dx12_overlay_policy::ShouldPassThroughPresentInterposerPrivateChainWithoutOverlayDraw(
                   interposerPrivateChain, interposerOutputQueue != nullptr)) {
        // FAIL CLOSED: the interposer's private chain without its recorded create queue has no
        // safe submit queue. Falling through to generic queue routing picks a game/foreign queue
        // for its backbuffers - the founding 0x887A002B device removal of this class (session
        // 20260914_102700). Forward the Present untouched; the overlay stays on the
        // application-facing chain.
        static std::atomic<int> s_interposerNoQueueSkipLogCount{0};
        const int logCount = s_interposerNoQueueSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: ProcessFrame — %s output chain %p has no recorded create queue; "
                "SKIPPING overlay draw fail-closed (origGame=%p primaryQ=%p cmdQ=%p) #%d",
                runtimeOwnedOutputChain ? "runtime-presented" : "present interposer", pSwapChain,
                dx12_hook_g_OriginalGameQueue,
                dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), (void*)g_CommandQueue.load(),
                logCount + 1);
        }
        return ProcessFrameFlow::kReturn;
    } else if (protectedOfficialFFXStartupOverlayOnly) {
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
            dx12_hook_g_OriginalGameQueue != nullptr, g_PostSLQueues.LastDeviceHealthyQueue() != nullptr, postFSRInactiveRecoveryPending,
            currentCommandQueue != nullptr && currentCommandQueue == currentPrimaryQueue,
            dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
            dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
            IsDLSSFrameGenerationActive());

        if (routingDecision ==
            ce::dx12_overlay_policy::SwapchainOverlayRoutingDecision::kSkipRuntimeOwnedSwapchainWithoutQueue) {
            static int s_fgOwnSkipLog = 0;
            ++s_fgOwnSkipLog;
            if (s_fgOwnSkipLog <= 10 || (s_fgOwnSkipLog % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — FG runtime owns swapchain but scQueue is null, SKIPPING overlay "
                    "(origGame=%p, fsrFGHeur=%d, fgOwnedSince=%llums ago) #%d",
                    dx12_hook_g_OriginalGameQueue, fsrFGNow ? 1 : 0,
                    ce::hook_clock::TickCount64() - dx12_hook_g_FGRuntimeOwnsSwapchainSince,
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
            gameQueue = g_PostSLQueues.LastDeviceHealthyQueue();
            static std::atomic<int> s_postFSRProcessFrameLastWorkingRouteLogCount{0};
            int logCount = s_postFSRProcessFrameLastWorkingRouteLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: ProcessFrame — post-FSR inactive recovery epoch using preserved PostSL lastWorking "
                    "queue %p (cmdQ=%p origQ=%p primaryQ=%p recentTraffic=%d)",
                    g_PostSLQueues.LastDeviceHealthyQueue(), currentCommandQueue, dx12_hook_g_OriginalGameQueue, currentPrimaryQueue,
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
            if (!gameQueue) {
                gameQueue = g_CommandQueue.load();
                if (!NoteUncapturedSwapchainQueueFallback(pSwapChain, gameQueue)) {
                    NoteDX12OverlayCoverageGate("uncaptured-queue-fail-closed");
        return ProcessFrameFlow::kReturn;
                }
            }
        }
    }
}
if (!gameQueue) {
    HookLog("DX12: ProcessFrame - no game queue, skipping overlay");
        return ProcessFrameFlow::kReturn;
}
// Queue selection decision: the first frame, every change of queue or path, and every 3000th frame.
{
    static std::atomic<uint32_t> s_queueLogCount{0};
    static ce::log_meter::ChangeGate s_queueLogGate;
    const uint32_t queueLogCount = s_queueLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        bool fsrFGActive = IsFSRFrameGenerationActive();
        const char* qPath = "unknown";
        if (slFGNow && dx12_hook_g_OriginalGameQueue && gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame(SL-FG)";
        else if (fsrFGActive && gameQueue == dx12_hook_g_SwapchainQueue)
            qPath = "scQueue(FSR-FG)";
        else if (fsrFGActive && gameQueue == dx12_hook_g_OriginalGameQueue)
            qPath = "origGame(FSR-FG-fallback)";
        else if (!slFGNow && !fsrFGActive && dx12_hook_g_HadFSRFGPhase && !dx12_hook_g_SwapchainQueue && g_PostSLQueues.LastDeviceHealthyQueue() &&
                 gameQueue == g_PostSLQueues.LastDeviceHealthyQueue())
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
        const auto queueVerdict = s_queueLogGate.ObserveOrEvery(
            ce::log_meter::FieldKey(gameQueue, slFGNow, fsrFGActive, qPath), queueLogCount, 3000);
        if (queueVerdict) {
            HookLogImportant(
                "DX12: ProcessFrame queue=%p (slFG=%d fsrFG=%d origQ=%p primaryQ=%p scQ=%p cmdQ=%p lastWorkingQ=%p "
                "path=%s) #%u%s",
                gameQueue, slFGNow ? 1 : 0, fsrFGActive ? 1 : 0, dx12_hook_g_OriginalGameQueue,
                dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire), dx12_hook_g_SwapchainQueue,
                (void*)g_CommandQueue.load(), g_PostSLQueues.LastDeviceHealthyQueue(), qPath, queueLogCount,
                ce::log_meter::SuppressedNote(queueVerdict.suppressed).c_str());
        }
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
// A chain a frame-generation runtime presents itself (XeFG) is not the game's: its queue is the runtime's, and
// taking it here would leave the game's own queue unhooked once the game presents natively again.
if (!dx12_hook_g_OriginalGameQueue &&
    !ce::fg_presenter::IsRuntimeOwnedOutputPresenter(ce::fg_presenter::PresenterFor(pSwapChain))) {
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
