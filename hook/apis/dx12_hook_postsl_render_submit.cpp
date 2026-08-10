#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"
#include "../../common/log_meter.h"

PostSLFlow PostSLRenderSession::Chunk3() {
if (scQueue && scQueue != queue && dx12_hook_g_State.crossQueueFence && !cachedSLFGActive) {
    UINT64 syncVal = ++dx12_hook_g_State.crossQueueFenceValue;
    // Signal on scQueue: "record SL's GPU progress"
    HRESULT sigHr = scQueue->Signal(dx12_hook_g_State.crossQueueFence, syncVal);
    if (SUCCEEDED(sigHr)) {
        // Wait on our queue: "don't execute until scQueue catches up"
        HRESULT waitHr = queue->Wait(dx12_hook_g_State.crossQueueFence, syncVal);
        if (SUCCEEDED(waitHr)) {
            crossQueueSynced = true;
        } else {
            HookLog("DX12: PostSL cross-queue pre-sync Wait failed hr=0x%08X", waitHr);
        }
    } else {
        static int s_preSyncFail = 0;
        if (s_preSyncFail++ < 5)
            HookLog(
                "DX12: PostSL cross-queue pre-sync Signal failed hr=0x%08X "
                "(scQueue=%p may reject external signals)",
                static_cast<unsigned>(sigHr), static_cast<void*>(scQueue));
    }
}
ID3D12CommandList* lists[] = {list};
bool usedRealECL = false;
bool usedOrigECL = false;
bool usedVirtualCall = false;
ID3D12CommandQueue* submittedQueue = queue;
{
    auto* preSubmitDev = g_Device.load(std::memory_order_acquire);
    HRESULT preSubmitHr = preSubmitDev ? preSubmitDev->GetDeviceRemovedReason() : E_FAIL;
    if (FAILED(preSubmitHr)) {
        HookLogImportant("DX12: PostSL SKIPPING ECL — device removed 0x%08X (queue=%p)", (unsigned)preSubmitHr,
                         queue);
        dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
        bb->Release();
        return PostSLFlow::kReturn;
    }
}
bool slFGAtDispatch = cachedSLFGActive;
if (g_PostSLECLDiagCount.load(std::memory_order_relaxed) < 10) {
    ExecuteCommandListsPtr origECLDiag = GetOriginalExecuteCommandLists(queue);
    HookLogImportant(
        "DX12: PostSL ECL diag — queue=%p scQueue=%p origECL=%p realECL=%p match=%d sameQueue=%d slWrapper=%d "
        "slFG=%d hadFSR=%d",
        queue, scQueue, (void*)origECLDiag, (void*)realECL, origECLDiag == realECL ? 1 : 0,
        queue == scQueue ? 1 : 0, isSLWrapperQ ? 1 : 0, slFGAtDispatch ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
    g_PostSLECLDiagCount.fetch_add(1, std::memory_order_relaxed);
}
{
    const uint32_t preSubmitLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(entryLifecycleEpoch,
                                                                             preSubmitLifecycleEpoch)) {
        static std::atomic<int> s_lifecycleSubmitAbortLogCount{0};
        const int logCount = s_lifecycleSubmitAbortLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 128) == 0) {
            HookLogImportant(
                "DX12: PostSL ABORT before ECL — swapchain lifecycle changed during callback "
                "(entryEpoch=%u currentEpoch=%u queue=%p scQueue=%p count=%d)",
                entryLifecycleEpoch, preSubmitLifecycleEpoch, queue, scQueue, logCount + 1);
        }
        bb->Release();
        return PostSLFlow::kReturn;
    }
    ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL overlay submit");
    if (slFGAtDispatch) {
        // When SL FG recreated the swapchain on a different queue (scQueue != origGame),
        // submit directly on scQueue.  SL's wrapper routes to origGame, causing
        // cross-queue backbuffer access → DEVICE_REMOVED.
        // PostSL fires after SL's FG pipeline completes, so scQueue is idle.
        bool scQueueDiffers = (scQueue && scQueue != dx12_hook_g_OriginalGameQueue);

        // DIRECT QUEUE SUBMISSION (bypasses SL's COM wrapper):
        //
        // SL's COM wrapper (g_SLWrapperQueue) adds internal metadata to each ECL.
        // This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
        // Confirmed by testing:
        //   - Full-rate through wrapper: crash at ~500 frames
        //   - 1/10 rate through wrapper: stable (damage drains between submits)
        //   - Direct to real queue: 16,798+ frames stable
        //   - Empty ECL through wrapper: stable (damage requires content)
        //
        // The fix: submit directly to the real D3D12 queue behind SL's wrapper
        // using g_RealD3D12ECL (raw D3D12 function from d3d12core.dll vtable).
        //
        // Bootstrap: First frame submits through SL's wrapper with
        // s_insidePostSLOverlayECL=true.  Our ECL detour sees the real queue
        // as pThis and captures it into g_RealQueueBehindSLWrapper.
        // Subsequent frames use the direct path.
        //
        // CAUTION FOR TALOS/OTHER GAMES: If the game uses FSR FG → DLSS FG
        // transitions, the real queue behind SL might change.  Monitor for
        // DEVICE_REMOVED after transitions and re-bootstrap if needed.
        ID3D12CommandQueue* slQueue = slWrapperQueue;

        const bool allowScQueueVirtualSubmit =
            ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(dx12_hook_g_HadFSRFGPhase, scQueueDiffers);
        const bool preferSelectedSwapchainQueueDirectSubmitForPureDLSS =
            ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(
                dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
                selectedQueueOrigECLMatchesRealECL);

        const bool useWrapperSubmitAfterFSR = ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(
            dx12_hook_g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue, slQueue != nullptr,
            preferSelectedSwapchainQueueSubmitAfterFSR);

        if (useWrapperSubmitAfterFSR) {
            // After an FSR phase, keep swapchain-touching PostSL work on the SL
            // wrapper path when that is the only path that has successfully
            // survived the post-FSR copy probes. We can still capture the real
            // queue behind the wrapper for diagnostics and later promotion.
            submittedQueue = slQueue;
            dx12_hook_s_insidePostSLOverlayECL = true;
            slQueue->ExecuteCommandLists(1, lists);
            dx12_hook_s_insidePostSLOverlayECL = false;
            usedVirtualCall = true;

            static int s_postFSRWrapperSubmitLog = 0;
            if (s_postFSRWrapperSubmitLog < 5 || (s_postFSRWrapperSubmitLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR submit #%d via SL wrapper %p (liveWrapper=%p scQueue=%p realQ=%p "
                    "offscreen=%d pinned=%d)",
                    s_postFSRWrapperSubmitLog, slQueue, liveSLWrapperQueue, scQueue, realQ,
                    usePostSLOffscreenComposite ? 1 : 0, usingPinnedPostFSRWrapperQueue ? 1 : 0);
            }
            s_postFSRWrapperSubmitLog++;
        } else if (preferSelectedSwapchainQueueSubmitAfterFSR) {
            // After an FSR phase, if PostSL already resolved to the runtime's
            // swapchain queue and probe submits on that queue succeeded, keep
            // using that queue directly. Falling back to the SL wrapper here
            // reintroduces the cross-queue handoff we are trying to avoid.
            submittedQueue = queue;
            if (selectedQueueOrigECL) {
                selectedQueueOrigECL(queue, 1, lists);
                usedOrigECL = true;
            } else {
                realECL(queue, 1, lists);
                usedRealECL = true;
            }

            static int s_postFSRDirectScQueueLog = 0;
            if (s_postFSRDirectScQueueLog < 5 || (s_postFSRDirectScQueueLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR submit #%d on selected scQueue %p (origECL=%d realECL=%d wrapper=%p)",
                    s_postFSRDirectScQueueLog, queue, selectedQueueOrigECL ? 1 : 0, realECL ? 1 : 0,
                    liveSLWrapperQueue);
            }
            s_postFSRDirectScQueueLog++;
        } else if (preferSelectedQueueDirectSubmitAfterFSR) {
            // After an FSR phase, the selected queue may already expose the real
            // D3D12 submit entrypoint directly. In that case, do not bounce to a
            // different late-captured "wrapper" queue for the first rendered
            // frame; stay on the queue that already passed our probes.
            submittedQueue = queue;
            selectedQueueOrigECL(queue, 1, lists);
            usedOrigECL = true;

            static int s_postFSRDirectSelectedQueueLog = 0;
            if (s_postFSRDirectSelectedQueueLog < 10 || (s_postFSRDirectSelectedQueueLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL post-FSR direct submit #%d on selected queue %p "
                    "(origECL matches realECL, scQueue=%p latestWrapper=%p)",
                    s_postFSRDirectSelectedQueueLog, queue, scQueue, liveSLWrapperQueue);
            }
            s_postFSRDirectSelectedQueueLog++;
        } else if (preferSelectedSwapchainQueueDirectSubmitForPureDLSS) {
            // Pure-DLSS startup/runtime path: when the live swapchain queue already
            // resolves to the real/original D3D12 ECL entrypoint, avoid bouncing
            // back through the queue's current virtual dispatch.
            submittedQueue = queue;
            selectedQueueOrigECL(queue, 1, lists);
            usedOrigECL = true;

            static int s_pureDLSSDirectScQueueLog = 0;
            if (s_pureDLSSDirectScQueueLog < 10 || (s_pureDLSSDirectScQueueLog % 200) == 0) {
                HookLogImportant(
                    "DX12: PostSL pure-DLSS direct scQueue submit #%d on %p "
                    "(origECL matches realECL, latestWrapper=%p)",
                    s_pureDLSSDirectScQueueLog, queue, liveSLWrapperQueue);
            }
            s_pureDLSSDirectScQueueLog++;
        } else if (allowScQueueVirtualSubmit) {
            // Direct submission on scQueue — backbuffers belong to this queue.
            // Bypass SL's wrapper entirely (routes to origGame → wrong queue).
            submittedQueue = scQueue;
            dx12_hook_s_insidePostSLOverlayECL = true;
            scQueue->ExecuteCommandLists(1, lists);
            dx12_hook_s_insidePostSLOverlayECL = false;
            usedVirtualCall = true;

            static int s_scQSubmitLog = 0;
            if (s_scQSubmitLog < 5 || (s_scQSubmitLog % 500 == 0))
                HookLogImportant("DX12: PostSL scQueue submit #%d on %p (origGame=%p, bypassing SL wrapper)",
                                 s_scQSubmitLog, scQueue, dx12_hook_g_OriginalGameQueue);
            s_scQSubmitLog++;
        } else if (realQ && realECL) {
            // Direct submission: bypass SL's wrapper entirely
            submittedQueue = realQ;
            dx12_hook_s_insidePostSLOverlayECL = true;
            realECL(realQ, 1, lists);
            dx12_hook_s_insidePostSLOverlayECL = false;
            usedRealECL = true;

            static int s_directLog = 0;
            if (s_directLog < 5 || (s_directLog % 500 == 0))
                HookLogImportant("DX12: PostSL DIRECT submit #%d on real queue %p (bypass SL wrapper)", s_directLog,
                                 realQ);
            s_directLog++;
        } else {
            const auto bootstrapPath = ce::dx12_overlay_policy::SelectPostSLBootstrapSubmitPath(
                dx12_hook_g_HadFSRFGPhase, realQ != nullptr, realECL != nullptr,
                selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr);
            if (bootstrapPath == ce::dx12_overlay_policy::PostSLBootstrapSubmitPath::kSelectedQueueOriginal) {
                // Pure-DLSS startup may render before passive real-ECL discovery.
                // The queue hook's saved original is already the final D3D12
                // submit path, so execute it exactly once and do not fall through
                // into the virtual bootstrap path.
                submittedQueue = queue;
                selectedQueueOrigECL(queue, 1, lists);
                usedOrigECL = true;
                static int s_pureDLSSBootstrapFallbackLog = 0;
                if (s_pureDLSSBootstrapFallbackLog < 10) {
                    HookLogImportant(
                        "DX12: PostSL pure-DLSS single-submit fallback via selectedQueueOrigECL on %p "
                        "(realECL not yet probed, scQueue=%p)",
                        queue, scQueue);
                }
                s_pureDLSSBootstrapFallbackLog++;
            } else if (bootstrapPath == ce::dx12_overlay_policy::PostSLBootstrapSubmitPath::kReject) {
                HookLogImportant(
                    "DX12: PostSL refusing SL wrapper bootstrap without direct path "
                    "(queue=%p scQueue=%p wrapper=%p)",
                    queue, scQueue, (void*)slQueue);
                bb->Release();
        return PostSLFlow::kReturn;
            } else {
                // Bootstrap: submit through SL's wrapper to capture real queue on first call
                if (!slQueue && dx12_hook_g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL refusing post-FSR bootstrap without SL wrapper queue (queue=%p scQueue=%p)",
                        queue, scQueue);
                    bb->Release();
        return PostSLFlow::kReturn;
                }
                if (slQueue) {
                    submittedQueue = slQueue;
                    dx12_hook_s_insidePostSLOverlayECL = true;
                    slQueue->ExecuteCommandLists(1, lists);
                    dx12_hook_s_insidePostSLOverlayECL = false;
                    usedVirtualCall = true;
                    HookLogImportant(
                        "DX12: PostSL bootstrap via SL wrapper %p (will capture real queue for direct path)", slQueue);
                } else {
                    if (!ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
                            slFGAtDispatch, slQueue != nullptr, realQ != nullptr, realECL != nullptr,
                            selectedQueueIsSwapchainQueue, selectedQueueOrigECLMatchesRealECL,
                            queue == dx12_hook_g_OriginalGameQueue)) {
                        HookLogImportant(
                            "DX12: PostSL refusing no-wrapper virtual bootstrap during Streamline FG "
                            "(queue=%p scQueue=%p realQ=%p realECL=%p)",
                            queue, scQueue, realQ, (void*)realECL);
                        bb->Release();

        return PostSLFlow::kReturn;
                    }
                    if (slFGAtDispatch && selectedQueueIsSwapchainQueue && selectedQueueOrigECLMatchesRealECL &&
                        selectedQueueOrigECL) {
                        submittedQueue = queue;
                        selectedQueueOrigECL(queue, 1, lists);
                        usedOrigECL = true;
                        static int s_noWrapperDirectSelectedQueueLog = 0;
                        if (s_noWrapperDirectSelectedQueueLog < 10 ||
                            (s_noWrapperDirectSelectedQueueLog % 200) == 0) {
                            HookLogImportant(
                                "DX12: PostSL no-wrapper direct selected-queue submit #%d on %p "
                                "(scQueue=%p origECL matches realECL)",
                                s_noWrapperDirectSelectedQueueLog, queue, scQueue);
                        }
                        s_noWrapperDirectSelectedQueueLog++;
                    } else {
                        dx12_hook_s_insidePostSLOverlayECL = true;
                        queue->ExecuteCommandLists(1, lists);
                        dx12_hook_s_insidePostSLOverlayECL = false;
                        usedVirtualCall = true;
                        static int s_noSlQ = 0;
                        if (s_noSlQ++ < 3)
                            HookLogImportant("DX12: PostSL no SL wrapper queue, using origGame %p", queue);
                    }
                }
            }
        }
    } else if (isSLWrapperQ) {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
        if (origECL) {
            origECL(queue, 1, lists);
            usedOrigECL = true;
        } else {
            queue->ExecuteCommandLists(1, lists);
            usedVirtualCall = true;
        }
    } else if (realECL) {
        realECL(queue, 1, lists);
        usedRealECL = true;
    } else {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
        if (origECL) {
            origECL(queue, 1, lists);
            usedOrigECL = true;
        } else {
            queue->ExecuteCommandLists(1, lists);
            usedVirtualCall = true;
        }
    }
}
const int submitPathCount = (usedRealECL ? 1 : 0) + (usedOrigECL ? 1 : 0) + (usedVirtualCall ? 1 : 0);
if (submitPathCount != 1) {
    static std::atomic<int> s_submitPathInvariantLogCount{0};
    const int logCount = s_submitPathInvariantLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 200) == 0) {
        HookLogImportant(
            "DX12: PostSL submit invariant violated — expected one ECL path, observed %d "
            "(queue=%p scQueue=%p virtual=%d real=%d original=%d epoch=%d call#=%d log=%d)",
            submitPathCount, submittedQueue, scQueue, usedVirtualCall ? 1 : 0, usedRealECL ? 1 : 0,
            usedOrigECL ? 1 : 0, s_reactivationEpoch, s_callsSinceReactivation, logCount + 1);
    }
}
if (rendered) {
    const bool retiredOfficialUiCoverage =
        retireOfficialUiCoverageAfterExactDraw &&
        ce::dx12_streamline_ui_overlay::RetirePostSLCoverageForExactBackbufferTakeover();
    if (retireOfficialUiCoverageAfterExactDraw) {
        static std::atomic<int> s_exactTransportOverridesOfficialUiLogCount{0};
        const int logCount =
            s_exactTransportOverridesOfficialUiLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 120) == 0) {
            HookLogImportant(
                "DX12: PostSL first proven startup output received an exact backbuffer draw; "
                "retiredOfficialUiCoverage=%d so later proxy buffers cannot inherit stale coverage "
                "(transportForced=%d postFSR=%d explicitPureDLSSColdStart=%d call#=%d log=%d)",
                retiredOfficialUiCoverage ? 1 : 0, dx12_hook_g_RequireExactPostSLStartupTransportDraw ? 1 : 0,
                dx12_hook_g_HadFSRFGPhase ? 1 : 0, explicitEnablePureDLSSColdStartProof ? 1 : 0, s_callsSinceReactivation,
                logCount);
        }
    }
    NoteDX12OverlayRendered(DX12OverlayRenderRoute::kPostSL);
    SharedMemoryLayout* postSLShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig postSLOverlayCfg = GetActiveDX12OverlayConfig(postSLShm);
    bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
    if (postSLShm && g_IPC && g_IPC->IsRecording() && isRealFrame && postSLOverlayCfg.showOverlay &&
        postSLOverlayCfg.captureIncludeOverlay) {
        PublishDX12CapturedFrame(pSwapChain, postSLShm, submittedQueue, true, bufIdx);
    }
    const uint64_t postSLScreenshotRequestId = GetPendingScreenshotRequestId(postSLShm);
    if (postSLScreenshotRequestId != 0 && postSLOverlayCfg.showOverlay &&
        postSLOverlayCfg.screenshotIncludeOverlay) {
        CaptureRequestedDX12Screenshot(sc3, postSLShm, postSLScreenshotRequestId, submittedQueue);
    }
}
bool slFGSubmit = cachedSLFGActive;
if (dx12_hook_g_State.fence) {
    UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
    ID3D12CommandQueue* submitQueue = submittedQueue ? submittedQueue : queue;
    HRESULT sigHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
    if (SUCCEEDED(sigHr)) {
        dx12_hook_g_State.currentFenceValue = next;
        if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
            dx12_hook_g_State.fenceValues[idx] = next;

        // Cross-queue GPU sync: only for non-SL-FG, non-same-queue scenarios
        bool crossQueueSafe = scQueue && scQueue != submitQueue && !slFGSubmit;
        if (crossQueueSafe) {
            HRESULT waitHr = scQueue->Wait(dx12_hook_g_State.fence, next);
            crossQueueSynced = true;
            if (FAILED(waitHr)) {
                static int s_waitFail = 0;
                if (s_waitFail++ < 5)
                    HookLog(
                        "DX12: PostSL cross-queue Wait failed hr=0x%08X "
                        "(scQueue=%p fence=%p val=%llu)",
                        waitHr, scQueue, dx12_hook_g_State.fence, (unsigned long long)next);
            }
        }
    }
}
if (dx12_hook_g_State.fence) {
    UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
    static int s_fenceHealthLog = 0;
    s_fenceHealthLog++;
    UINT64 expected = dx12_hook_g_State.currentFenceValue;
    UINT64 gap = (expected > completed) ? (expected - completed) : 0;
    if (s_fenceHealthLog <= 10 || (s_fenceHealthLog % 200 == 0) || gap > 10) {
        HookLogImportant(
            "DX12: PostSL fence health #%d — completed=%llu current=%llu gap=%llu allocators=%d idx=%d",
            s_fenceHealthLog, completed, expected, gap, (int)dx12_hook_g_State.allocators.size(), idx);
    }
}
static std::atomic<int> s_postSLRenderCount{0};
int renderNum = s_postSLRenderCount.fetch_add(1, std::memory_order_relaxed) + 1;
s_postSLRenders.fetch_add(1, std::memory_order_relaxed);
HRESULT postDevReason = dev->GetDeviceRemovedReason();
if (SUCCEEDED(postDevReason) && rendered && pSwapChain && submittedQueue) {
    ++dx12_hook_s_PostSLSuccessfulSubmitSequence;
    if (!dx12_hook_g_HadSuccessfulPostSLPhase.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Latched first device-healthy PostSL submit for future repeated pure-DLSS handoff prewarm "
            "(swapchain=%p queue=%p)",
            pSwapChain, submittedQueue);
    }
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
        dx12_hook_g_PostSLWarmResumePreservationPending.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: PostSL warm-resume preservation completed on first successful active submit "
            "(sc=%p queue=%p)",
            pSwapChain, submittedQueue);
    }
    IDXGISwapChain* previousSuccessfulPostSLSwapchain =
        dx12_hook_g_LastSuccessfulPostSLSwapchain.exchange(pSwapChain, std::memory_order_acq_rel);
    if (previousSuccessfulPostSLSwapchain != pSwapChain) {
        HookLogImportant(
            "DX12: PostSL proved exact swapchain route %p on submitted queue %p "
            "(previousSwapchain=%p epoch=%d)",
            pSwapChain, submittedQueue, previousSuccessfulPostSLSwapchain, s_reactivationEpoch);
    }

    // The same COM identity may be rebound from the normal Present route
    // to a runtime proxy route. The newest successful submit is the useful
    // ownership proof; do not let its pre-FG identity classify it as normal
    // after Streamline is explicitly switched off. Publish this before the
    // confirmed-render release stores below so the OFF callback cannot see
    // confirmation without also seeing the exact swapchain proof.
    IDXGISwapChain* expectedNormalSwapchain = pSwapChain;
    if (dx12_hook_g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
            expectedNormalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
        HookLogImportant(
            "DX12: PostSL superseded remembered original-queue ownership for swapchain %p "
            "with a successful runtime-route submit",
            pSwapChain);
    }
}
dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(true, std::memory_order_release);
if (!dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_relaxed)) {
    dx12_hook_g_PostSLConfirmedRendering.store(true, std::memory_order_release);
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
    dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: PostSL confirmed rendering");
    // kStreamlineStartupTransitionGraceMs from the SL FG activation arm covers the
    // remaining startup churn window. Streamline can still call Present briefly after
    // PostSL confirms; during that family CE keeps using the bypass trampoline for
    // Streamline-stack Presents and keeps stale OFF churn suppressed. The window
    // expires naturally from the arm time; the old ShouldClear... check at ~line 9220
    // is removed because it cleared the window too aggressively on the same call
    // where confirmed became true, re-exposing the startup churn race.
    HookLogImportant("DX12: PostSL CONFIRMED rendering via re-entrant Present — suppressing pre-SL draw");
}
dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
const int stableFrameCount = dx12_hook_g_PostSLStableFrameCount.fetch_add(1, std::memory_order_acq_rel) + 1;
if (stableFrameCount == 1) {
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLFirstConfirmedRender,
                                "DX12::PostSLOverlayRender", submittedQueue, pSwapChain,
                                g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(),
                                HookHasExplicitStreamlineSetOptionsActivation());
}
extendRuntimeStateStabilization =
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.load(std::memory_order_acquire);
const int runtimeStateStabilizationLastFrame =
    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationLastFrame(extendRuntimeStateStabilization);
const bool runtimeStateStabilizing =
    ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
        true, stableFrameCount, extendRuntimeStateStabilization);
const bool runtimeStateStabilizingPreviousFrame =
    stableFrameCount > 1 && ce::dx12_overlay_policy::ShouldTreatConfirmedPostSLRenderingAsRuntimeStateStabilizing(
                                true, stableFrameCount - 1, extendRuntimeStateStabilization);
if (runtimeStateStabilizing && !runtimeStateStabilizingPreviousFrame) {
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(true, std::memory_order_release);
    HookLogImportant(
        "DX12: PostSL confirmed startup rendering entered runtime-state stabilization "
        "(stableFrames=%d first=%d last=%d extended=%d epoch=%d)",
        stableFrameCount, ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
        runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0, s_reactivationEpoch);
} else if (!runtimeStateStabilizing && runtimeStateStabilizingPreviousFrame) {
    HookLogImportant(
        "DX12: PostSL confirmed startup rendering left runtime-state stabilization "
        "(stableFrames=%d last=%d extended=%d epoch=%d)",
        stableFrameCount, runtimeStateStabilizationLastFrame, extendRuntimeStateStabilization ? 1 : 0,
        s_reactivationEpoch);
}
if (SUCCEEDED(postDevReason) && submittedQueue != dx12_hook_g_PostSLLastWorkingQueue &&
    ce::dx12_overlay_policy::ShouldRememberPostSLLastWorkingQueue(isSLWrapperQ)) {
    HookLogImportant("DX12: PostSL updating lastWorkingQueue %p -> %p", dx12_hook_g_PostSLLastWorkingQueue, submittedQueue);
    SetPostSLLastWorkingQueue(submittedQueue);
}
// Metered diagnostic: the old condition logged every frame once renderNum
// passed 1800 (added during the SL-metadata DEVICE_HUNG investigation), which
// at PostSL rates produced ~5.2k SUBMIT lines in a 90-second trace session.
// Keep the first-burst + heartbeat cadence, retain a bounded dense window
// around the historical crash region (frames 1700-1900), and always log device
// failure. Routing/state transitions are already covered by dedicated
// transition logs elsewhere.
if (ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(renderNum), 20, 600) ||
    (renderNum >= 1700 && renderNum <= 1900) || FAILED(postDevReason)) {
    HookLogImportant(
        "DX12: Post-SL overlay SUBMIT #%d (bufIdx=%u queue=%p scQueue=%p slWrapper=%d rendered=%d "
        "virtualCall=%d realECL=%d origECL=%d xqSync=%d tid=0x%04X devRemoved=0x%08X epoch=%d)",
        renderNum, bufIdx, submittedQueue, scQueue, isSLWrapperQ ? 1 : 0, rendered ? 1 : 0, usedVirtualCall ? 1 : 0,
        usedRealECL ? 1 : 0, usedOrigECL ? 1 : 0, crossQueueSynced ? 1 : 0, GetCurrentThreadId(),
        (unsigned)postDevReason, s_reactivationEpoch);
}
if (FAILED(postDevReason)) {
    HookLogImportant(
        "DX12: DEVICE_REMOVED detected after PostSL ECL submit #%d "
        "(queue=%p scQueue=%p hr=0x%08X)",
        renderNum, submittedQueue, scQueue, (unsigned)postDevReason);
}
bb->Release();
    return PostSLFlow::kContinue;
}
