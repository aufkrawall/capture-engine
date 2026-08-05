#include "dx12_hook_internal.h"
#include "dx12_hook_postsl_session.h"

PostSLFlow PostSLRenderSession::Chunk2() {
if (isPostFSRProbe) {
    // Log comprehensive diagnostics on first probe frame
    if (dx12_hook_g_PostFSRProbeFrames.load(std::memory_order_acquire) == 0 &&
        dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        HookLogImportant("DX12: PostSL post-FSR DIAG: pSwapChain=%p bb=%p bufIdx=%u bbW=%u bbH=%u", pSwapChain, bb,
                         bufIdx, (unsigned)bbDesc.Width, bbDesc.Height);
        HookLogImportant("DX12: PostSL post-FSR DIAG: queue=%p origGame=%p slWrapper=%p scQ=%p", queue,
                         dx12_hook_g_OriginalGameQueue, slWrapperQueue, dx12_hook_g_SwapchainQueue);
    }

    bool probeHandled = true;
    const bool preferRealQueueBehindWrapperAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(dx12_hook_g_HadFSRFGPhase, cachedSLFGActive,
                                                                               realQ != nullptr);
    const bool bootstrapRealQueueCaptureViaWrapperProbe =
        ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
            dx12_hook_g_HadFSRFGPhase, cachedSLFGActive, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire),
            realQ != nullptr, slWrapperQueue != nullptr, hasSelectedQueueSubmitPath, isSLWrapperQ);
    if (preferRealQueueBehindWrapperAfterFSR && dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
        dx12_hook_g_PostFSRProbeLevel.store(3, std::memory_order_release);
        dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
        HookLogImportant(
            "DX12: PostSL post-FSR switching to direct real queue behind wrapper %p — skipping level 2 probe",
            realQ);
        bb->Release();
        return PostSLFlow::kReturn;
    }
    // CRITICAL: Always use the locked queue (stable across frames) for probe
    // submissions, NOT the transient slWrapperQueue (g_SLWrapperQueue) which
    // changes as different SL wrapper queues are seen by the ECL detour on
    // other threads.  Using a transient wrapper mid-probe causes DEVICE_REMOVED
    // when the new wrapper doesn't own the swapchain's resource state.
    ID3D12CommandQueue* probeQueue = bootstrapRealQueueCaptureViaWrapperProbe
                                         ? queue
                                         : ((dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1 &&
                                             slWrapperQueue && !preferSelectedSwapchainQueueSubmitAfterFSR)
                                                ? slWrapperQueue
                                                : queue);

    if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
        // Probe 0: Scratch resource barrier on origGame — confirms queue works.
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC scratchDesc = {};
        scratchDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        scratchDesc.Width = 64;
        scratchDesc.Height = 64;
        scratchDesc.DepthOrArraySize = 1;
        scratchDesc.MipLevels = 1;
        scratchDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scratchDesc.SampleDesc.Count = 1;
        scratchDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ID3D12Resource* scratch = nullptr;
        HRESULT scratchHr =
            dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &scratchDesc,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&scratch));
        if (SUCCEEDED(scratchHr) && scratch) {
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = scratch;
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = scratch;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(2, barriers);
            scratch->Release();
        }
    } else if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1) {
        // Probe 1: PRESENT→RT→PRESENT on backbuffer via SL's wrapper queue.
        // SL's ECL interception dispatches to its internal queue which has correct
        // resource state tracking for the swapchain backbuffers.
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = bb;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = bb;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(2, barriers);
    } else if (ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCopyOnlyProbeAfterFSR(
                   dx12_hook_g_HadFSRFGPhase, dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire),
                   usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue)) {
        if (!EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
            HookLogImportant(
                "DX12: PostSL post-FSR copy-only probe could not create offscreen RT (w=%d h=%d fmt=%d)",
                dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format);
            bb->Release();
        return PostSLFlow::kReturn;
        }

        D3D12_RESOURCE_BARRIER toCopyDest = {};
        toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopyDest.Transition.pResource = dx12_hook_g_State.offscreenRT;
        toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toCopyDest);

        D3D12_RESOURCE_BARRIER bbToCopySource = {};
        if (useExplicitPostFSRBackbufferCopyTransitions) {
            bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToCopySource.Transition.pResource = bb;
            bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToCopySource);
        }

        D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
        bbSrc.pResource = bb;
        bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bbSrc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION offDst = {};
        offDst.pResource = dx12_hook_g_State.offscreenRT;
        offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        offDst.SubresourceIndex = 0;
        list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

        D3D12_RESOURCE_BARRIER toCopySource = {};
        toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySource.Transition.pResource = dx12_hook_g_State.offscreenRT;
        toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toCopySource);

        D3D12_RESOURCE_BARRIER bbToCopyDest = {};
        if (useExplicitPostFSRBackbufferCopyTransitions) {
            bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToCopyDest.Transition.pResource = bb;
            bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToCopyDest);
        }

        D3D12_TEXTURE_COPY_LOCATION offSrc = {};
        offSrc.pResource = dx12_hook_g_State.offscreenRT;
        offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        offSrc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION bbDst = {};
        bbDst.pResource = bb;
        bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bbDst.SubresourceIndex = 0;
        list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

        if (useExplicitPostFSRBackbufferCopyTransitions) {
            D3D12_RESOURCE_BARRIER bbToPresent = {};
            bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToPresent.Transition.pResource = bb;
            bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToPresent);
        }
    } else if (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
        probeHandled = false;
    }

    if (probeHandled) {
        list->Close();
        ID3D12CommandList* lists[] = {list};
        // Keep probe submission on the queue that actually owns the tested path.
        // Post-FSR copy probes have only been observed to survive when routed
        // through the SL wrapper path rather than forcing an immediate direct
        // queue handoff.
        {
            ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL post-FSR probe submit");
            if (bootstrapRealQueueCaptureViaWrapperProbe && isSLWrapperQ) {
                dx12_hook_s_insidePostSLOverlayECL = true;
                probeQueue->ExecuteCommandLists(1, lists);
                dx12_hook_s_insidePostSLOverlayECL = false;
            } else if (isSLWrapperQ && !dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire)) {
                dx12_hook_s_insidePostSLOverlayECL = true;
                probeQueue->ExecuteCommandLists(1, lists);
                dx12_hook_s_insidePostSLOverlayECL = false;
                ID3D12CommandQueue* capturedReal = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
                if (capturedReal) {
                    HookLogImportant(
                        "DX12: PostSL post-FSR probe captured real queue %p behind wrapper bootstrap %p",
                        capturedReal, probeQueue);
                }
            } else {
                probeQueue->ExecuteCommandLists(1, lists);
            }
        }

        if (dx12_hook_g_State.fence) {
            UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
            HRESULT sigHr = probeQueue->Signal(dx12_hook_g_State.fence, next);
            if (SUCCEEDED(sigHr)) {
                dx12_hook_g_State.currentFenceValue = next;
                if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                    dx12_hook_g_State.fenceValues[idx] = next;
            }
        }

        HRESULT probeHr = dev->GetDeviceRemovedReason();
        dx12_hook_g_PostFSRProbeFrames.fetch_add(1, std::memory_order_acq_rel);
        const char* probeNames[] = {"scratch-barrier", "SLwrapper-bb-barrier", "offscreen-copy-only"};
        const char* probeName = dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3
                                    ? probeNames[dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire)]
                                    : "unknown";
        HookLogImportant(
            "DX12: PostSL post-FSR PROBE level=%d (%s) frame=%d/%d queue=%p devRemoved=0x%08X %s (fast=%d)",
            dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire), probeName,
            dx12_hook_g_PostFSRProbeFrames.load(std::memory_order_acquire), postFSRProbeFramesPerLevel, probeQueue, probeHr,
            FAILED(probeHr) ? "FAILED" : "OK", fastPostFSRDLSSProbe ? 1 : 0);

        if (FAILED(probeHr)) {
            // DEVICE_REMOVED from BB barrier is FATAL — skip to barrier-free.
            // Scratch barrier failures are non-fatal (queue just isn't ready).
            int skipTo = (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1)
                             ? 2
                             : dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
            dx12_hook_g_PostFSRProbeLevel.store(static_cast<int>(skipTo), std::memory_order_release);
            dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
            HookLogImportant("DX12: PostSL post-FSR probe FAILED, advancing to level %d",
                             dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire));
            bb->Release();
        return PostSLFlow::kReturn;
        }

        if (dx12_hook_g_PostFSRProbeFrames >= postFSRProbeFramesPerLevel) {
            // Skip level 1 (BB barrier): go directly from level 0 to level 2.
            // BB barriers cause FATAL DEVICE_REMOVED on queues that don't own the
            // swapchain's resource state. Level 2 only validates copy traffic on
            // the swapchain timeline before any real overlay rendering is attempted.
            int nextLevel = (dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0)
                                ? (selectedQueueIsSwapchainQueue ? 3 : 2)
                                : dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
            dx12_hook_g_PostFSRProbeLevel.store(static_cast<int>(nextLevel), std::memory_order_release);
            dx12_hook_g_PostFSRProbeFrames.store(0, std::memory_order_release);
            HookLogImportant(
                "DX12: PostSL post-FSR probe PASSED, advancing to level %d (selectedScQueue=%d skipped BB barrier "
                "probe)",
                dx12_hook_g_PostFSRProbeLevel.load(std::memory_order_acquire), selectedQueueIsSwapchainQueue ? 1 : 0);
        }
        bb->Release();
        return PostSLFlow::kReturn;
    }
}
EnsureDescFreeBackendForDeviceAndFormat(dev, dx12_hook_g_State.format, "PostSL lazy init");
bool willRender = dx12_hook_g_DescFreeBackend && dx12_hook_g_D3D11On12Adapter.IsInitialized();
if (willRender && dx12_hook_g_DescFreeBackend) {
    D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
    DXGI_FORMAT bbFmt = bbDesc.Format;
    DXGI_FORMAT psoFmt = dx12_hook_g_State.format;
    static int s_fmtLogCount = 0;
    if (s_fmtLogCount < 5 || (bbFmt != psoFmt && s_fmtLogCount < 50)) {
        HookLogImportant("DX12: PostSL format check — backbuffer=%d psoFmt=%d %s", (int)bbFmt, (int)psoFmt,
                         bbFmt == psoFmt ? "MATCH" : "MISMATCH");
        s_fmtLogCount++;
    }
    if (bbFmt != psoFmt) {
        // Recreate DescFree backend with correct format
        HookLogImportant("DX12: PostSL format MISMATCH (bb=%d pso=%d) — recreating DescFree backend", (int)bbFmt,
                         (int)psoFmt);
        dx12_hook_g_State.format = bbFmt;
        willRender = EnsureDescFreeBackendForDeviceAndFormat(dev, bbFmt, "PostSL format mismatch");
    }
}
const bool transitionProbeDeviceHealthy = !FAILED(dev->GetDeviceRemovedReason());
const bool renderDirectlyOnTransitionProbe =
    ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(selectedQueueIsSwapchainQueue,
                                                                                transitionProbeDeviceHealthy);
int probesNeeded = (fastPostFSRDLSSProbe || renderDirectlyOnTransitionProbe) ? 0 : 1;
bool isPostTransitionProbe = (s_reactivationEpoch > 1 && s_postSLProbeFrames < probesNeeded);
if (renderDirectlyOnTransitionProbe && !fastPostFSRDLSSProbe && s_reactivationEpoch > 1 &&
    s_postSLProbeFrames == 0) {
    static int s_skipTransitionProbeLog = 0;
    ++s_skipTransitionProbeLog;
    if (s_skipTransitionProbeLog <= 20 || (s_skipTransitionProbeLog % 120) == 0)
        HookLogImportant(
            "DX12: PostSL rendering overlay directly on first reactivation present — skipping redundant "
            "empty-ECL transition probe (swapchain queue, device healthy; render's pre/post devRemoved check "
            "is the proof) epoch=%d queue=%p scQueue=%p hadFSR=%d",
            s_reactivationEpoch, queue, scQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0);
}
if (isPostTransitionProbe) {
    // Probe frames present without an overlay draw — tag the coverage gate
    // so engage-seam streaks attribute to the probes instead of "unknown".
    NoteDX12OverlayCoverageGate("postsl-transition-probe");
    s_postSLProbeFrames++;
    ID3D12CommandList* probeList[] = {list};

    if (s_postSLProbeFrames == 1) {
        // Probe 1: empty ECL — tests basic queue health
        list->Close();
    } else {
        // Probe 2: ClearRTV with barriers — tests backbuffer access (non-SL only)
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = bb;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE probeRtv = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        probeRtv.ptr += (SIZE_T)bufIdx * rtvSize;
        dev->CreateRenderTargetView(bb, nullptr, probeRtv);

        float clearColor[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(probeRtv, clearColor, 0, nullptr);


        D3D12_RESOURCE_BARRIER barrierBack = {};
        barrierBack.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrierBack.Transition.pResource = bb;
        barrierBack.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrierBack.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrierBack.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrierBack);
        list->Close();
    }

    // Submit probe — use origECL for SL wrapper queue, realECL otherwise
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL transition probe submit");
        if (isSLWrapperQ) {
            ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(queue);
            if (origECL) {
                origECL(queue, 1, probeList);
            } else {
                queue->ExecuteCommandLists(1, probeList);
            }
        } else {
            ExecuteCommandListsPtr eclFn = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
            if (eclFn) {
                eclFn(queue, 1, probeList);
            } else {
                queue->ExecuteCommandLists(1, probeList);
            }
        }
    }

    // Signal fence for allocator tracking
    if (dx12_hook_g_State.fence) {
        UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
        HRESULT sigHr = queue->Signal(dx12_hook_g_State.fence, next);
        if (SUCCEEDED(sigHr)) {
            dx12_hook_g_State.currentFenceValue = next;
            if (idx >= 0 && idx < (int)dx12_hook_g_State.fenceValues.size())
                dx12_hook_g_State.fenceValues[idx] = next;
        }
    }

    HRESULT probeHr = dev->GetDeviceRemovedReason();
    HookLogImportant("DX12: PostSL PROBE #%d on queue=%p (scQ=%p epoch=%d slWrapper=%d) — %s devRemoved=0x%08X %s",
                     s_postSLProbeFrames, queue, scQueue, s_reactivationEpoch, isSLWrapperQ ? 1 : 0,
                     s_postSLProbeFrames == 1 ? "empty ECL" : "ClearRTV+barriers", probeHr,
                     FAILED(probeHr) ? "FAILED" : "OK");
    if (FAILED(probeHr)) {
        bb->Release();
        return PostSLFlow::kReturn;
    }
    bb->Release();
        return PostSLFlow::kReturn;
}
static ID3D12Fence* s_xqSyncFence = nullptr;
static uint64_t s_xqSyncVal = 0;
bool didXQSync = false;
if (willRender && !s_xqSyncFence) {
    // Create fence lazily (needed for SL queue → origGame post-submit sync)
    HRESULT fhr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_xqSyncFence));
    if (SUCCEEDED(fhr)) {
        HookLogImportant("DX12: PostSL created cross-queue sync fence for SL↔origGame sync");
    } else {
        HookLogImportant("DX12: PostSL FAILED to create cross-queue sync fence hr=0x%08X", fhr);
    }
}
const auto postSLBarrierMode = ce::dx12_overlay_policy::DecidePostSLBackbufferBarrierMode(
    cachedSLFGActive, useExplicitPostFSRSwapchainTransitions);
bool slFGBarrierFree = postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly;
if (willRender && bb) {
    D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
    // AddRef/Release to get refcount without side effects
    bb->AddRef();
    ULONG refCount = bb->Release();
    static int s_bbHealthLog = 0;
    if (s_bbHealthLog < 10 || (s_bbHealthLog % 200 == 0)) {
        HookLogImportant("DX12: PostSL BB health #%d — bb=%p refCnt=%lu w=%u h=%u fmt=%u bufIdx=%d slFG=%d",
                         s_bbHealthLog, bb, refCount, (unsigned)bbDesc.Width, (unsigned)bbDesc.Height,
                         (unsigned)bbDesc.Format, bufIdx, cachedSLFGActive ? 1 : 0);
    }
    s_bbHealthLog++;
}
if (willRender && !usePostSLOffscreenComposite && slFGBarrierFree) {
    // UAV barrier: full GPU pipeline flush, no state tracking modification
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr;  // NULL = global flush
    list->ResourceBarrier(1, &uavBarrier);
} else if (willRender && !usePostSLOffscreenComposite) {
    D3D12_RESOURCE_BARRIER preBarrier = {};
    preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarrier.Transition.pResource = bb;
    preBarrier.Transition.StateBefore =
        postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget
            ? D3D12_RESOURCE_STATE_PRESENT
            : D3D12_RESOURCE_STATE_COMMON;
    preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &preBarrier);
}
if (willRender) {
    static bool s_loggedBarrierMode = false;
    if (!s_loggedBarrierMode) {
        s_loggedBarrierMode = true;
        const char* barrierModeName = "common->rt";
        if (postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly) {
            barrierModeName = "uav-only";
        } else if (postSLBarrierMode ==
                   ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kPresentToRenderTarget) {
            barrierModeName = "present->rt";
        }
        HookLogImportant(
            "DX12: PostSL barrier mode — mode=%s slFGBarrierFree=%d explicitPostFSR=%d offscreen=%d hadFSR=%d "
            "xqSync=%d",
            barrierModeName, slFGBarrierFree ? 1 : 0, useExplicitPostFSRSwapchainTransitions ? 1 : 0,
            usePostSLOffscreenComposite ? 1 : 0, dx12_hook_g_HadFSRFGPhase ? 1 : 0, didXQSync ? 1 : 0);
    }
}
if (willRender) {
    if (ce::dx12_overlay_policy::ShouldSyntheticPostSLRefreshMetrics(cachedSLFGActive, processFrameRecentlySeen)) {
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            perf->Update(PerfLogger::GetQpcUs());
            const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
            ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, g_FGCompat.GetOutputFPS(),
                                                         g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                         "DX12::PostSLOverlayRender");
        }
    }

    // Update text/API labels on real frames, but always keep the overlay
    // bound to the shared metrics object so FPS/history remain visible when
    // the first frame after an FG-driven reinit is classified as interpolated.
    bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
    dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
    dx12_hook_g_D3D11On12Adapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
    const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
    if (metricsBinding.bindMetrics) {
        dx12_hook_g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
    }
    if (metricsBinding.refreshFrameMetadata) {
        static const bool s_isVKD3D = []() {
            return GetModuleHandleA("d3d12core.dll") &&
                   (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
        }();
        const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
        dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
    }

    if (usePostSLOffscreenComposite &&
        EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
        // Avoid binding the post-FSR DLSS backbuffer as an RTV on the first real
        // PostSL render. Instead composite through an offscreen RT and copy back.
        D3D12_RESOURCE_BARRIER toCopyDest = {};
        toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopyDest.Transition.pResource = dx12_hook_g_State.offscreenRT;
        toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toCopyDest);

        D3D12_RESOURCE_BARRIER bbToCopySource = {};
        if (useExplicitPostFSRBackbufferCopyTransitions) {
            bbToCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToCopySource.Transition.pResource = bb;
            bbToCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            bbToCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bbToCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToCopySource);
        }

        D3D12_TEXTURE_COPY_LOCATION bbSrc = {};
        bbSrc.pResource = bb;
        bbSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bbSrc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION offDst = {};
        offDst.pResource = dx12_hook_g_State.offscreenRT;
        offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        offDst.SubresourceIndex = 0;
        list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

        D3D12_RESOURCE_BARRIER toRenderTarget = {};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource = dx12_hook_g_State.offscreenRT;
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toRenderTarget);

        dx12_hook_s_descFreeCmdList = list;
        dx12_hook_s_descFreeRtv = dx12_hook_g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
        // PostSL/FG overlay: synchronized by the FG completion fence each
        // frame, so disable the DescFree per-slot guard (g_State.fence does
        // not track this value here — a non-zero guard would stall reuse).
        dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
        dx12_hook_s_descFreeSlotGuardValue = 0;
        SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
        dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
        dx12_hook_s_descFreeCmdList = nullptr;

        D3D12_RESOURCE_BARRIER toCopySource = {};
        toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySource.Transition.pResource = dx12_hook_g_State.offscreenRT;
        toCopySource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toCopySource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &toCopySource);

        D3D12_RESOURCE_BARRIER bbToCopyDest = {};
        if (useExplicitPostFSRBackbufferCopyTransitions) {
            bbToCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToCopyDest.Transition.pResource = bb;
            bbToCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            bbToCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            bbToCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToCopyDest);
        }

        D3D12_TEXTURE_COPY_LOCATION offSrc = {};
        offSrc.pResource = dx12_hook_g_State.offscreenRT;
        offSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        offSrc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION bbDst = {};
        bbDst.pResource = bb;
        bbDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bbDst.SubresourceIndex = 0;
        list->CopyTextureRegion(&bbDst, 0, 0, 0, &offSrc, nullptr);

        if (useExplicitPostFSRBackbufferCopyTransitions) {
            D3D12_RESOURCE_BARRIER bbToPresent = {};
            bbToPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bbToPresent.Transition.pResource = bb;
            bbToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bbToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            bbToPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &bbToPresent);
        }
    } else {
        // Recreate RTV for this buffer index (cheap CPU-side op)
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        rtvHandle.ptr += (SIZE_T)bufIdx * rtvSize;
        dev->CreateRenderTargetView(bb, nullptr, rtvHandle);

        dx12_hook_s_descFreeCmdList = list;
        dx12_hook_s_descFreeRtv = rtvHandle;
        // PostSL/FG overlay: synchronized by the FG completion fence (see above).
        dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
        dx12_hook_s_descFreeSlotGuardValue = 0;
        SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
        dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
        dx12_hook_s_descFreeCmdList = nullptr;
    }
    rendered = true;
} else {
    // Log why rendering was skipped (HookLogImportant for visibility after reactivation)
    static int s_backendSkip = 0;
    s_backendSkip++;
    if (s_backendSkip <= 10 || (s_backendSkip % 100) == 0)
        HookLogImportant("DX12: PostSL SKIP render #%d — backend=%p adapterInit=%d", s_backendSkip,
                         (void*)dx12_hook_g_DescFreeBackend, dx12_hook_g_D3D11On12Adapter.IsInitialized() ? 1 : 0);
}
if (rendered && !usePostSLOffscreenComposite && slFGBarrierFree) {
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr;
    list->ResourceBarrier(1, &uavBarrier);

    static int s_postBarrierLog = 0;
    if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
        HookLogImportant("DX12: PostSL UAV post-barrier #%d epoch=%d", s_postBarrierLog, s_reactivationEpoch);
    }
    s_postBarrierLog++;
} else if (rendered && !usePostSLOffscreenComposite) {
    D3D12_RESOURCE_BARRIER postBarrier = {};
    postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postBarrier.Transition.pResource = bb;
    postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &postBarrier);

    static int s_postBarrierLog = 0;
    if (s_postBarrierLog < 5 || (s_postBarrierLog % 500 == 0)) {
        HookLogImportant("DX12: PostSL RT→PRESENT post-barrier #%d epoch=%d", s_postBarrierLog,
                         s_reactivationEpoch);
    }
    s_postBarrierLog++;
}
if (!willRender) {
    list->Close();
    bb->Release();
        return PostSLFlow::kReturn;
}
HRESULT closeHr = list->Close();
if (FAILED(closeHr)) {
    static int s_closeFailCount = 0;
    if (s_closeFailCount++ < 10)
        HookLog("DX12: PostSLOverlayRender — list->Close failed hr=0x%08X", closeHr);
    bb->Release();
        return PostSLFlow::kReturn;
}
HRESULT preDevReason = dev->GetDeviceRemovedReason();
if (FAILED(preDevReason)) {
    HookLogImportant("DX12: PostSL PRE-submit device already removed (hr=0x%08X queue=%p) — skipping",
                     (unsigned)preDevReason, queue);
    bb->Release();
        return PostSLFlow::kReturn;
}
const uint32_t preSyncLifecycleEpoch = dx12_hook_g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
if (ce::dx12_overlay_policy::ShouldAbortPostSLSubmitAfterLifecycleChange(entryLifecycleEpoch,
                                                                         preSyncLifecycleEpoch)) {
    static std::atomic<int> s_lifecycleAbortLogCount{0};
    const int logCount = s_lifecycleAbortLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 128) == 0) {
        HookLogImportant(
            "DX12: PostSL ABORT before queue synchronization — swapchain lifecycle changed during callback "
            "(entryEpoch=%u currentEpoch=%u queue=%p scQueue=%p count=%d)",
            entryLifecycleEpoch, preSyncLifecycleEpoch, queue, scQueue, logCount + 1);
    }
    bb->Release();
        return PostSLFlow::kReturn;
}
crossQueueSynced = didXQSync;  // SL→origGame sync from above
    return PostSLFlow::kContinue;
}

