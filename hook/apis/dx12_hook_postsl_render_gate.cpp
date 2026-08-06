#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"

PostSLFlow PostSLRenderSession::Chunk1() {
if (FAILED(devReason)) {
    dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
    DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
    HookLogImportant("DX12: PostSLOverlayRender — device removed (0x%08X), disabling", (unsigned)devReason);
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        return PostSLFlow::kReturn;
}
if (DXGIShared::g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
    static int s_scInvalid = 0;
    if (s_scInvalid++ < 5)
        HookLog("DX12: PostSL SKIP — swapchainInvalid=true");
        return PostSLFlow::kReturn;
}
const bool officialUiCoverageActive = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
const bool requireExactPostSLStartupOutputDraw =
    ce::dx12_overlay_policy::ShouldRequireExactPostSLBackbufferDrawForStartup(
        dx12_hook_g_RequireExactPostSLStartupTransportDraw, dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL,
        explicitEnablePureDLSSColdStartProof, officialUiCoverageActive);
retireOfficialUiCoverageAfterExactDraw = requireExactPostSLStartupOutputDraw && officialUiCoverageActive;
if (!requireExactPostSLStartupOutputDraw && ce::dx12_streamline_ui_overlay::ConsumePostSLCoverage()) {
    NoteDX12OverlayRendered(DX12OverlayRenderRoute::kStreamlineUI);
        return PostSLFlow::kReturn;
}
int cd = dx12_hook_g_SceneTransitionCooldown.load(std::memory_order_acquire);
if (cd > 0) {
    dx12_hook_g_SceneTransitionCooldown.store(cd - 1, std::memory_order_release);
    if (cd == 1) {
        ID3D12CommandQueue* resumeQueue = nullptr;
        {
            std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
            resumeQueue = dx12_hook_g_PostSLLastWorkingQueue;
            if (!resumeQueue)
                resumeQueue = g_CommandQueue.load(std::memory_order_acquire);
            if (!resumeQueue)
                resumeQueue = dx12_hook_g_SwapchainQueue;
        }
        HookLogImportant(
            "DX12: Post-SL scene transition cooldown complete — resuming overlay "
            "(queue=%p overlayInit=%d syncInit=%d bufCount=%d)",
            resumeQueue, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_State.bufferCount);
    }
        return PostSLFlow::kReturn;
}
queue = nullptr;
scQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
    scQueue = dx12_hook_g_SwapchainQueue;

    // Queue selection strategy for PostSL:
    //
    // DLSS FG (no prior FSR FG): use origGame.  It's the swapchain creation
    //   queue with valid NVIDIA driver state and authorized backbuffer access.
    //
    // DLSS FG (after FSR FG was active): prefer the runtime-owned swapchain
    //   queue or a captured direct queue behind SL's wrapper. Keeping PostSL
    //   locked to the wrapper itself can poison long-running FG state and later
    //   crash on teardown, so wrapper use is bootstrap-only at most.
    //
    // Outside SL FG: exact OFF keep-alive lastWorking > locked > scQueue >
    // origGame > preFG > cmdQueue.
    bool slFGNow = cachedSLFGActive;
    // GTA V's DLSS FG activation triggers a heuristic FSR ghost (brief swapchain
    // queue change) that clears within frames.  Setting hadFSR from heuristic forces
    // PostSL onto SL's internal queues which causes DEVICE_HUNG.
    if (ce::dx12_overlay_policy::ShouldLatchFSRFGHistory(g_FGCompat.IsFSRFGApiActive(), false)) {
        if (!dx12_hook_g_HadFSRFGPhase) {
            dx12_hook_g_HadFSRFGPhase = true;
            HookLogImportant("DX12: PostSL — FSR FG history confirmed, origGame driver state may be stale");
        }
    }

    ID3D12CommandQueue* directQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
    ID3D12CommandQueue* latestSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
    ID3D12CommandQueue* validatedCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
    ExecuteCommandListsPtr currentRealECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    const bool validatedCommandQueueIsWrapper =
        validatedCommandQueue && validatedCommandQueue != dx12_hook_g_OriginalGameQueue && validatedCommandQueue != scQueue;
    ID3D12CommandQueue* wrapperBootstrapQueue = latestSLWrapperQueue;
    if (ce::dx12_overlay_policy::ShouldUseValidatedCommandQueueWrapperBootstrapAfterFSR(
            dx12_hook_g_HadFSRFGPhase, slFGNow, directQueueBehindWrapper != nullptr, validatedCommandQueueIsWrapper,
            scQueue != nullptr && scQueue != dx12_hook_g_OriginalGameQueue,
            HookHasExplicitStreamlineSetOptionsActivation())) {
        wrapperBootstrapQueue = validatedCommandQueue;
    }
    bool hasDirectQueueBehindWrapper = directQueueBehindWrapper != nullptr;
    const bool hasRuntimeOwnedSwapchainQueue = scQueue != nullptr && scQueue != dx12_hook_g_OriginalGameQueue;
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
    bool preferRealQueueBehindWrapper = ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(
        dx12_hook_g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper);
    const bool preferValidatedDirectQueueForLock =
        ce::dx12_overlay_policy::ShouldPreferValidatedDirectQueueForPostFSRLock(dx12_hook_g_HadFSRFGPhase, slFGNow,
                                                                                hasDirectQueueBehindWrapper);
    bool allowWrapperBootstrapQueue = ce::dx12_overlay_policy::ShouldUsePostSLWrapperBootstrapQueueAfterFSR(
        dx12_hook_g_HadFSRFGPhase, slFGNow, hasDirectQueueBehindWrapper, wrapperBootstrapQueue != nullptr,
        hasRuntimeOwnedSwapchainQueue, explicitSetOptionsActivation, safePostFSRBootstrapPath);
    const bool resumeOnValidatedLastWorkingQueue = ce::dx12_overlay_policy::
        ShouldReuseValidatedPostSLLastWorkingQueueForStreamlineResumeDuringPostFSRInactiveRecovery(
            dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG.load(std::memory_order_acquire),
            dx12_hook_g_PostSLLastWorkingQueue != nullptr, scQueue != nullptr, explicitSetOptionsActivation,
            safePostFSRBootstrapPath);
    const bool lockedQueueIsSLWrapper =
        dx12_hook_g_PostSLLockedQueue && dx12_hook_g_PostSLLockedQueue != dx12_hook_g_OriginalGameQueue && dx12_hook_g_PostSLLockedQueue != scQueue;
    ExecuteCommandListsPtr scQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
    const bool hasSwapchainQueueSubmitPath = scQueue && (scQueueOrigECL != nullptr || currentRealECL != nullptr);
    const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
    const bool selectDirectQueueInsteadOfLockedWrapper =
        ce::dx12_overlay_policy::ShouldSelectPostSLRealQueueBehindWrapperInsteadOfLockedQueueAfterFSR(
            dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper,
            hasDirectQueueBehindWrapper);
    const bool selectSwapchainQueueInsteadOfLockedWrapper =
        ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
            dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, slFGNow, lockedQueueIsSLWrapper, scQueue != nullptr,
            scQueue != dx12_hook_g_OriginalGameQueue, hasSwapchainQueueSubmitPath, hasWrapperDerivedDirectPath);

    if (preferValidatedDirectQueueForLock && directQueueBehindWrapper) {
        queue = directQueueBehindWrapper;
        static int s_directQueuePreferredLog = 0;
        if (s_directQueuePreferredLog++ < 10) {
            HookLogImportant(
                "DX12: PostSL queue candidate — validated direct queue %p preferred over scQueue %p after FSR",
                queue, scQueue);
        }
    } else if (selectDirectQueueInsteadOfLockedWrapper) {
        queue = directQueueBehindWrapper;
        static int s_promoteSelectionLog = 0;
        if (s_promoteSelectionLog++ < 5) {
            HookLog("DX12: PostSL queue candidate — direct real queue %p replacing locked wrapper %p", queue,
                    dx12_hook_g_PostSLLockedQueue);
        }
    } else if (selectSwapchainQueueInsteadOfLockedWrapper) {
        queue = scQueue;
        static int s_swapchainSelectionLog = 0;
        if (s_swapchainSelectionLog++ < 10) {
            HookLogImportant(
                "DX12: PostSL queue candidate — swapchain queue %p replacing locked wrapper %p after FSR", queue,
                dx12_hook_g_PostSLLockedQueue);
        }
    } else if (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                   keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                   dx12_hook_g_PostSLLastWorkingQueue != nullptr)) {
        queue = dx12_hook_g_PostSLLastWorkingQueue;
        static std::atomic<int> s_exactOffKeepAliveLastWorkingQueueLogCount{0};
        const int logCount = s_exactOffKeepAliveLastWorkingQueueLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: PostSL exact-proxy explicit-OFF keep-alive selecting last successful direct queue %p "
                "ahead of locked queue %p (sc=%p log=%d)",
                queue, dx12_hook_g_PostSLLockedQueue, pSwapChain, logCount + 1);
        }
    } else if (dx12_hook_g_PostSLLockedQueue) {
        queue = dx12_hook_g_PostSLLockedQueue;
    } else if (resumeOnValidatedLastWorkingQueue) {
        queue = dx12_hook_g_PostSLLastWorkingQueue;
        static int s_postFSRResumeQueueLog = 0;
        if (s_postFSRResumeQueueLog++ < 10) {
            HookLogImportant(
                "DX12: PostSL queue — reusing validated lastWorking queue %p for resumed DLSS activation during "
                "post-FSR inactive recovery (origGame=%p explicit=%d safeBootstrap=%d)",
                queue, dx12_hook_g_OriginalGameQueue, explicitSetOptionsActivation ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0);
        }
    } else if (slFGNow) {
        if (preferRealQueueBehindWrapper) {
            queue = directQueueBehindWrapper;
            static int s_realQueueLog = 0;
            if (s_realQueueLog++ < 5) {
                HookLog("DX12: PostSL queue — realQueueBehindWrapper %p (scQueue=%p hadFSR=%d)", queue, scQueue,
                        dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (allowWrapperBootstrapQueue && wrapperBootstrapQueue &&
                   wrapperBootstrapQueue != dx12_hook_g_OriginalGameQueue && wrapperBootstrapQueue != scQueue) {
            queue = wrapperBootstrapQueue;
            static int s_wrapperBootstrapLog = 0;
            if (s_wrapperBootstrapLog++ < 10) {
                HookLogImportant(
                    "DX12: PostSL queue — wrapper bootstrap %p (validatedCmdQ=%p latestWrapper=%p scQueue=%p "
                    "hadFSR=%d)",
                    queue, validatedCommandQueue, latestSLWrapperQueue, scQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (scQueue && scQueue != dx12_hook_g_OriginalGameQueue) {
            if (dx12_hook_g_HadFSRFGPhase) {
                HookLogImportant(
                    "DX12: PostSL queue — WARNING: falling back to scQueue %p in post-FSR DLSS path "
                    "(origGame=%p, hadFSR=%d, no wrapper/direct queue available)",
                    scQueue, dx12_hook_g_OriginalGameQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            }
            queue = scQueue;
            static int s_scQLog = 0;
            if (s_scQLog++ < 5)
                HookLog("DX12: PostSL queue — scQueue %p (SL swapchain, origGame=%p, hadFSR=%d)", queue,
                        dx12_hook_g_OriginalGameQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
        } else if (dx12_hook_g_OriginalGameQueue) {
            queue = dx12_hook_g_OriginalGameQueue;
            static int s_origLog = 0;
            if (s_origLog++ < 5)
                HookLog("DX12: PostSL queue — origGame %p (slFG, hadFSR=%d)", queue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
        }
    } else if (scQueue) {
        queue = scQueue;
    } else if (dx12_hook_g_OriginalGameQueue) {
        queue = dx12_hook_g_OriginalGameQueue;
    } else if (dx12_hook_g_PreFGGameQueue) {
        queue = dx12_hook_g_PreFGGameQueue;
    } else {
        queue = g_CommandQueue.load(std::memory_order_acquire);
    }

    // AddRef the selected queue under the mutex to prevent it from being
    // freed by DX12_SetCommandQueue (which also uses this mutex) or SL's
    // internal cleanup while we use it.  Released by scope guard below.
    if (queue)
        queue->AddRef();
}
auto queueReleaseGuard = ce::make_scope_guard([&]() {
    if (queue)
        queue->Release();
});
if (!queue) {
    static int s_noQueue = 0;
    if (s_noQueue++ < 5)
        HookLog("DX12: PostSL SKIP — no queue (cmdQueue=%p scQueue=%p)", (void*)g_CommandQueue.load(),
                dx12_hook_g_SwapchainQueue);
        return PostSLFlow::kReturn;
}
{
    ID3D12CommandQueue* oldLockedQueue = nullptr;
    bool lockedQueueWasUpdated = false;
    bool shouldKeepExistingLockedQueue = false;
    {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        ID3D12CommandQueue* directQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        ExecuteCommandListsPtr currentRealECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        ExecuteCommandListsPtr lockedScQueueOrigECL = scQueue ? GetOriginalExecuteCommandLists(scQueue) : nullptr;
        const bool lockedQueueIsSLWrapper =
            dx12_hook_g_PostSLLockedQueue && dx12_hook_g_PostSLLockedQueue != dx12_hook_g_OriginalGameQueue && dx12_hook_g_PostSLLockedQueue != scQueue;
        const bool hasSwapchainQueueSubmitPath =
            scQueue && (lockedScQueueOrigECL != nullptr || currentRealECL != nullptr);
        const bool hasWrapperDerivedDirectPath = directQueueBehindWrapper != nullptr && currentRealECL != nullptr;
        bool shouldReplaceLockedQueue =
            ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueBehindWrapperAfterFSR(
                dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper, directQueueBehindWrapper != nullptr) &&
            queue == directQueueBehindWrapper;
        shouldReplaceLockedQueue =
            shouldReplaceLockedQueue ||
            (ce::dx12_overlay_policy::ShouldSelectPostSLSwapchainQueueInsteadOfLockedWrapperAfterFSR(
                 dx12_hook_g_PostSLLockedQueue != nullptr, dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, lockedQueueIsSLWrapper,
                 scQueue != nullptr, scQueue != dx12_hook_g_OriginalGameQueue, hasSwapchainQueueSubmitPath,
                 hasWrapperDerivedDirectPath) &&
             queue == scQueue);
        shouldReplaceLockedQueue =
            shouldReplaceLockedQueue ||
            (ce::dx12_overlay_policy::ShouldUsePostSLLastWorkingQueueForExactExplicitOffKeepAlive(
                 keepAliveRenderAfterExplicitOff, exactExplicitOffKeepAliveSwapchain,
                 dx12_hook_g_PostSLLastWorkingQueue != nullptr) &&
             queue == dx12_hook_g_PostSLLastWorkingQueue);
        const bool selectedQueueMatchesLockedQueue = queue == dx12_hook_g_PostSLLockedQueue;

        if (ce::dx12_overlay_policy::ShouldMutatePostSLLockedQueue(
                dx12_hook_g_PostSLLockedQueue != nullptr, selectedQueueMatchesLockedQueue, shouldReplaceLockedQueue)) {
            oldLockedQueue = dx12_hook_g_PostSLLockedQueue;
            dx12_hook_g_PostSLLockedQueue = queue;
            queue->AddRef();  // prevent locked queue from being freed between PostSL calls
            lockedQueueWasUpdated = true;

            if (oldLockedQueue) {
                if (queue == directQueueBehindWrapper) {
                    HookLogImportant(
                        "DX12: PostSL promoting locked queue %p -> real queue behind wrapper %p after post-FSR "
                        "bootstrap",
                        oldLockedQueue, directQueueBehindWrapper);
                } else if (exactExplicitOffKeepAliveSwapchain && queue == dx12_hook_g_PostSLLastWorkingQueue) {
                    HookLogImportant(
                        "DX12: PostSL replacing stale locked queue %p -> retained exact-proxy queue %p for "
                        "explicit-OFF keep-alive",
                        oldLockedQueue, queue);
                } else {
                    HookLogImportant(
                        "DX12: PostSL replacing locked queue %p -> swapchain queue %p after post-FSR direct path "
                        "recovery",
                        oldLockedQueue, queue);
                }
            } else {
                bool usingSLWrapper = (queue != dx12_hook_g_OriginalGameQueue && queue != scQueue);
                bool slFGAtLock = cachedSLFGActive;
                HookLogImportant(
                    "DX12: PostSL locked to queue %p (origGame=%p scQueue=%p cmdQueue=%p preFG=%p epoch=%d "
                    "slWrapper=%d slFG=%d hadFSR=%d)",
                    queue, dx12_hook_g_OriginalGameQueue, scQueue, (void*)g_CommandQueue.load(), dx12_hook_g_PreFGGameQueue,
                    s_reactivationEpoch, usingSLWrapper ? 1 : 0, slFGAtLock ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
            }
        } else if (!selectedQueueMatchesLockedQueue) {
            shouldKeepExistingLockedQueue = true;
            queue->Release();  // Release per-call AddRef on the rejected queue
            queue = dx12_hook_g_PostSLLockedQueue;
            if (queue) {
                queue->AddRef();  // Per-call AddRef on the locked queue instead
            }
        }
    }


    if (oldLockedQueue) {
        oldLockedQueue->Release();
    }

    if (shouldKeepExistingLockedQueue && queue) {
        ID3D12CommandQueue* newCmdQueue = g_CommandQueue.load(std::memory_order_acquire);
        HookLogImportant("DX12: PostSL REFUSING queue change: locked=%p, cmdQueue=%p (changed!), scQueue=%p", queue,
                         newCmdQueue, scQueue);
    }

    if (!queue) {
        static int s_missingLockedQueue = 0;
        if (s_missingLockedQueue++ < 5) {
            HookLogImportant("DX12: PostSL SKIP — locked queue disappeared during synchronized selection");
        }
        return PostSLFlow::kReturn;
    }
}
if (dx12_hook_g_State.syncDevice) {
    // Belt-and-suspenders: verify queue vtable is intact before virtual call.
    // The AddRef above should keep the queue alive, but if something else
    // (SL internal cleanup) bypassed COM refcounting, the vtable may be gone.
    void* vtbl = *reinterpret_cast<void* volatile*>(queue);
    if (!vtbl) {
        HookLogImportant("DX12: PostSL SKIP — queue %p has null vtable (freed?), clearing lock", queue);
        ClearPostSLQueues("DX12: PostSL null vtable");
        return PostSLFlow::kReturn;
    }
    ID3D12Device* queueDevice = nullptr;
    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
        if (queueDevice != dx12_hook_g_State.syncDevice) {
            HookLogImportant(
                "DX12: PostSL DEVICE MISMATCH! queue=%p queueDev=%p != syncDev=%p — "
                "forcing overlay re-init to prevent cross-device DEVICE_REMOVED",
                queue, queueDevice, dx12_hook_g_State.syncDevice);
            queueDevice->Release();
            // Force full re-initialization on next ProcessFrame
            dx12_hook_g_State.overlayInit = false;
            dx12_hook_g_State.syncInit = false;
            dx12_hook_g_State.syncDevice = nullptr;
            ClearPostSLQueues("DX12: PostSL device mismatch");
            ClearPostSLPinnedSLWrapperQueue("DX12: PostSL device mismatch");
            SetPostSLLastWorkingQueue(nullptr);  // Cross-device — old queue invalid
        return PostSLFlow::kReturn;
        }
        queueDevice->Release();
    }
}
sc3 = nullptr;
if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
    if (s_callsSinceReactivation <= 20)
        HookLogImportant("DX12: PostSL EARLY-EXIT: QI for IDXGISwapChain3 failed (call#%d)",
                         s_callsSinceReactivation);
        return PostSLFlow::kReturn;
}
bufIdx = sc3->GetCurrentBackBufferIndex();
bb = nullptr;
HRESULT getBufHr = sc3->GetBuffer(bufIdx, IID_PPV_ARGS(&bb));
sc3->Release();
if (FAILED(getBufHr) || !bb) {
    if (s_callsSinceReactivation <= 20)
        HookLogImportant("DX12: PostSL EARLY-EXIT: GetBuffer(%u) failed hr=0x%08X (call#%d)", bufIdx, getBufHr,
                         s_callsSinceReactivation);
        return PostSLFlow::kReturn;
}
if (bufIdx >= (UINT)dx12_hook_g_State.bufferCount) {
    if (bufIdx < 8) {
        int newCount = (int)bufIdx + 1;
        HookLogImportant("DX12: PostSL expanding bufferCount %d -> %d (bufIdx=%u from swapchain)",
                         dx12_hook_g_State.bufferCount, newCount, bufIdx);
        dx12_hook_g_State.bufferCount = newCount;
    } else {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: bufIdx=%u too large (>8) (call#%d)", bufIdx,
                             s_callsSinceReactivation);
        bb->Release();
        return PostSLFlow::kReturn;
    }
}
int allocPoolSize = static_cast<int>(dx12_hook_g_State.allocators.size());
if (allocPoolSize <= 0) {
    bb->Release();
        return PostSLFlow::kReturn;
}
const int preferredIdx = dx12_hook_g_State.allocIndex % allocPoolSize;
const UINT64 completedFenceValue = dx12_hook_g_State.fence ? dx12_hook_g_State.fence->GetCompletedValue() : UINT64_MAX;
idx = ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(dx12_hook_g_State.fenceValues.data(), allocPoolSize,
                                                                   preferredIdx, completedFenceValue);
if (idx < 0) {
    idx = preferredIdx;
}
dx12_hook_g_State.allocIndex = (idx + 1) % allocPoolSize;
list = dx12_hook_g_State.cmdList;
alloc = (idx < allocPoolSize) ? dx12_hook_g_State.allocators[idx] : nullptr;
if (!list || !alloc) {
    if (s_callsSinceReactivation <= 20)
        HookLogImportant("DX12: PostSL EARLY-EXIT: list=%p alloc=%p (idx=%d poolSize=%d call#%d)", list, alloc, idx,
                         allocPoolSize, s_callsSinceReactivation);
    bb->Release();
        return PostSLFlow::kReturn;
}
if (dx12_hook_g_State.fence && idx < (int)dx12_hook_g_State.fenceValues.size() && dx12_hook_g_State.fenceValues[idx] > 0) {
    UINT64 completed = dx12_hook_g_State.fence->GetCompletedValue();
    if (completed < dx12_hook_g_State.fenceValues[idx]) {
        // Reusable event handle — created once, persists for the DLL lifetime
        static HANDLE s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        bool fenceReady = false;
        if (s_fenceEvent) {
            HRESULT evHr = dx12_hook_g_State.fence->SetEventOnCompletion(dx12_hook_g_State.fenceValues[idx], s_fenceEvent);
            if (SUCCEEDED(evHr)) {
                DWORD waitResult = WaitForSingleObject(s_fenceEvent, 1);  // 1ms max
                completed = dx12_hook_g_State.fence->GetCompletedValue();
                fenceReady = (completed >= dx12_hook_g_State.fenceValues[idx]);

                static int s_fenceWaitLog = 0;
                if (fenceReady && s_fenceWaitLog++ < 10)
                    HookLogImportant(
                        "DX12: PostSL fence wait resolved via event (alloc[%d] completed=%llu needed=%llu "
                        "waitResult=%lu)",
                        idx, completed, dx12_hook_g_State.fenceValues[idx], waitResult);
            }
        }
        if (!fenceReady) {
            s_postSLSkipFence.fetch_add(1, std::memory_order_relaxed);
            if (s_callsSinceReactivation <= 20)
                HookLogImportant(
                    "DX12: PostSL EARLY-EXIT: alloc[%d] in-flight after 1ms wait (completed=%llu needed=%llu "
                    "call#%d)",
                    idx, completed, dx12_hook_g_State.fenceValues[idx], s_callsSinceReactivation);
            bb->Release();
        return PostSLFlow::kReturn;
        }
    }
}
HRESULT allocResetHr = alloc->Reset();
if (FAILED(allocResetHr)) {
    if (s_callsSinceReactivation <= 20)
        HookLogImportant("DX12: PostSL EARLY-EXIT: alloc->Reset failed hr=0x%08X (call#%d)", allocResetHr,
                         s_callsSinceReactivation);
    bb->Release();
        return PostSLFlow::kReturn;
}
HRESULT listResetHr = list->Reset(alloc, nullptr);
if (FAILED(listResetHr)) {
    if (s_callsSinceReactivation <= 20)
        HookLogImportant("DX12: PostSL EARLY-EXIT: list->Reset failed hr=0x%08X (call#%d)", listResetHr,
                         s_callsSinceReactivation);
    bb->Release();
        return PostSLFlow::kReturn;
}
{
    HRESULT preDevHr = dev->GetDeviceRemovedReason();
    if (FAILED(preDevHr)) {
        HookLogImportant(
            "DX12: PostSL EARLY-EXIT: device already removed BEFORE submit "
            "(hr=0x%08X epoch=%d call#%d)",
            preDevHr, s_reactivationEpoch, s_callsSinceReactivation);
        bb->Release();
        return PostSLFlow::kReturn;
    }
}
rendered = false;
if (s_callsSinceReactivation <= 1 || s_postSLRenders.load(std::memory_order_relaxed) == 0) {
    HookLogImportant(
        "DX12: PostSL first ECL submit approaching (epoch=%d call#=%d queue=%p slFG=%d "
        "runtimeMode=%d hadFSR=%d)",
        s_reactivationEpoch, s_callsSinceReactivation, queue, cachedSLFGActive ? 1 : 0,
        (int)g_FGCompat.GetRuntimeMode(), dx12_hook_g_HadFSRFGPhase ? 1 : 0);
}
selectedQueueIsSwapchainQueue = (queue == scQueue);
fastPostFSRDLSSProbe = ce::dx12_overlay_policy::ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(
    dx12_hook_g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL, selectedQueueIsSwapchainQueue, cachedSLFGActive);
postFSRProbeFramesPerLevel = fastPostFSRDLSSProbe ? 1 : dx12_hook_kPostFSRProbeFramesPerLevel;
realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
realQ = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
selectedQueueOrigECL = GetOriginalExecuteCommandLists(queue);
selectedQueueOrigECLMatchesRealECL = selectedQueueOrigECL && selectedQueueOrigECL == realECL;
isSLWrapperQ = ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(
    queue == dx12_hook_g_OriginalGameQueue, queue == dx12_hook_g_PostSLDedicatedQueue, selectedQueueIsSwapchainQueue,
    selectedQueueOrigECLMatchesRealECL);
useExplicitPostFSRSwapchainTransitions =
    ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
        dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
usePostSLOffscreenComposite = ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(
    dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
useExplicitPostFSRBackbufferCopyTransitions =
    ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
        usePostSLOffscreenComposite, useExplicitPostFSRSwapchainTransitions);
hasSelectedQueueSubmitPath = selectedQueueOrigECL != nullptr || realECL != nullptr;
const bool hasWrapperDerivedDirectPath = realQ != nullptr && realECL != nullptr;
preferSelectedSwapchainQueueSubmitAfterFSR =
    ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(
        dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, isSLWrapperQ, hasSelectedQueueSubmitPath,
        hasWrapperDerivedDirectPath);
preferSelectedQueueDirectSubmitAfterFSR =
    ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(
        dx12_hook_g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
        selectedQueueOrigECLMatchesRealECL, realQ != nullptr);
if (ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(
        fastPostFSRDLSSProbe, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire))) {
    HookLogImportant(
        "DX12: PostSL post-FSR fast bootstrap — rendering overlay directly on first reactivation present "
        "(skipping redundant scratch-barrier probe; render's own pre/post devRemoved check is the health "
        "proof) queue=%p scQueue=%p epoch=%d",
        queue, scQueue, s_reactivationEpoch);
    dx12_hook_g_PostFSRProbeLevel.store(3, std::memory_order_release);
    dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
}
isPostFSRProbe = dx12_hook_g_HadFSRFGPhase && dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3;
slWrapperQueue = nullptr;
liveSLWrapperQueue = nullptr;
usingPinnedPostFSRWrapperQueue = false;
if (dx12_hook_g_HadFSRFGPhase) {
    std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
    liveSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);

    ID3D12CommandQueue* pinnedSLWrapperQueue = dx12_hook_g_PostSLPinnedSLWrapperQueue;
    ID3D12CommandQueue* wrapperCandidate = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : liveSLWrapperQueue;
    if (!wrapperCandidate) {
        // Fallback: try g_CommandQueue if it's not origGame or scQueue.
        ID3D12CommandQueue* cmdQ = g_CommandQueue.load(std::memory_order_acquire);
        if (cmdQ && cmdQ != dx12_hook_g_OriginalGameQueue && cmdQ != dx12_hook_g_SwapchainQueue)
            wrapperCandidate = cmdQ;
    }
    if (wrapperCandidate == dx12_hook_g_OriginalGameQueue || wrapperCandidate == dx12_hook_g_SwapchainQueue)
        wrapperCandidate = nullptr;

    if (ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(
            dx12_hook_g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue,
            pinnedSLWrapperQueue != nullptr, wrapperCandidate != nullptr,
            preferSelectedSwapchainQueueSubmitAfterFSR)) {
        wrapperCandidate->AddRef();
        dx12_hook_g_PostSLPinnedSLWrapperQueue = wrapperCandidate;
        pinnedSLWrapperQueue = wrapperCandidate;
        usingPinnedPostFSRWrapperQueue = true;
        HookLogImportant("DX12: PostSL pinned post-FSR SL wrapper queue %p for epoch=%d (source=%s scQueue=%p)",
                         wrapperCandidate, s_reactivationEpoch,
                         liveSLWrapperQueue ? "captured" : "cmdQueue-fallback", scQueue);
    } else {
        usingPinnedPostFSRWrapperQueue = pinnedSLWrapperQueue != nullptr;
    }

    slWrapperQueue = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : wrapperCandidate;
    if (slWrapperQueue)
        slWrapperQueue->AddRef();
}
auto slWrapperQueueReleaseGuard = ce::make_scope_guard([&]() {
    if (slWrapperQueue)
        slWrapperQueue->Release();
});
    return PostSLFlow::kContinue;
}

