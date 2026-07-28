
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
            return;
        }
    }

    // CRITICAL: Verify device compatibility before using sync resources.
    // After swapchain recreation (e.g. FSR→DLSS switch), the submission queue may
    // belong to a different D3D12 device than the one used to create allocators,
    // command list, and fence in InitOverlaySync.  Cross-device ECL submission
    // causes DEVICE_REMOVED.  Detect this and force full re-initialization.
    if (g_State.syncDevice) {
        // Belt-and-suspenders: verify queue vtable is intact before virtual call.
        // The AddRef above should keep the queue alive, but if something else
        // (SL internal cleanup) bypassed COM refcounting, the vtable may be gone.
        void* vtbl = *reinterpret_cast<void* volatile*>(queue);
        if (!vtbl) {
            HookLogImportant("DX12: PostSL SKIP — queue %p has null vtable (freed?), clearing lock", queue);
            ClearPostSLQueues("DX12: PostSL null vtable");
            return;
        }
        ID3D12Device* queueDevice = nullptr;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) && queueDevice) {
            if (queueDevice != g_State.syncDevice) {
                HookLogImportant(
                    "DX12: PostSL DEVICE MISMATCH! queue=%p queueDev=%p != syncDev=%p — "
                    "forcing overlay re-init to prevent cross-device DEVICE_REMOVED",
                    queue, queueDevice, g_State.syncDevice);
                queueDevice->Release();
                // Force full re-initialization on next ProcessFrame
                g_State.overlayInit = false;
                g_State.syncInit = false;
                g_State.syncDevice = nullptr;
                ClearPostSLQueues("DX12: PostSL device mismatch");
                ClearPostSLPinnedSLWrapperQueue("DX12: PostSL device mismatch");
                SetPostSLLastWorkingQueue(nullptr);  // Cross-device — old queue invalid
                return;
            }
            queueDevice->Release();
        }
    }

    // Get current backbuffer from the re-entrant swapchain
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: QI for IDXGISwapChain3 failed (call#%d)",
                             s_callsSinceReactivation);
        return;
    }

    UINT bufIdx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = nullptr;
    HRESULT getBufHr = sc3->GetBuffer(bufIdx, IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(getBufHr) || !bb) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: GetBuffer(%u) failed hr=0x%08X (call#%d)", bufIdx, getBufHr,
                             s_callsSinceReactivation);
        return;
    }

    // Validate buffer index against current overlay state.
    // After FG mode switches, SL may create a new swapchain with more buffers
    // (e.g., 3→4 for DLSS FG triple buffering).  g_State.bufferCount reflects
    // the count at init time and may be stale.  Dynamically expand to match.
    if (bufIdx >= (UINT)g_State.bufferCount) {
        if (bufIdx < 8) {
            int newCount = (int)bufIdx + 1;
            HookLogImportant("DX12: PostSL expanding bufferCount %d -> %d (bufIdx=%u from swapchain)",
                             g_State.bufferCount, newCount, bufIdx);
            g_State.bufferCount = newCount;
        } else {
            if (s_callsSinceReactivation <= 20)
                HookLogImportant("DX12: PostSL EARLY-EXIT: bufIdx=%u too large (>8) (call#%d)", bufIdx,
                                 s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    // Pick an allocator from the pool. Preserve round-robin locality, but scan the whole pool for a completed
    // slot before considering a Present-thread wait. At high generated-frame rates the preferred slot can
    // still be busy while a later slot is free; waiting in that case creates avoidable interval variance.
    int allocPoolSize = static_cast<int>(g_State.allocators.size());
    if (allocPoolSize <= 0) {
        bb->Release();
        return;
    }
    const int preferredIdx = g_State.allocIndex % allocPoolSize;
    const UINT64 completedFenceValue = g_State.fence ? g_State.fence->GetCompletedValue() : UINT64_MAX;
    int idx = ce::dx12_overlay_policy::ChooseReadyOverlayAllocatorSlot(g_State.fenceValues.data(), allocPoolSize,
                                                                       preferredIdx, completedFenceValue);
    if (idx < 0) {
        idx = preferredIdx;
    }
    g_State.allocIndex = (idx + 1) % allocPoolSize;

    auto* list = g_State.cmdList;
    auto* alloc = (idx < allocPoolSize) ? g_State.allocators[idx] : nullptr;
    if (!list || !alloc) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list=%p alloc=%p (idx=%d poolSize=%d call#%d)", list, alloc, idx,
                             allocPoolSize, s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Fence check: ensure allocator's GPU work is complete before reset. This is now exceptional: the pool-wide
    // scan above reaches here with an in-flight allocator only when every allocator is still busy.
    // Uses event-based wait (SetEventOnCompletion + WaitForSingleObject) instead
    // of instant bail — at 100% GPU load, the allocator may be just microseconds
    // from completing, and a skip causes visible overlay flicker.  Event-based
    // wait has zero CPU overhead (thread sleeps until GPU signals) with a 1ms
    // timeout cap to avoid blocking the game.
    if (g_State.fence && idx < (int)g_State.fenceValues.size() && g_State.fenceValues[idx] > 0) {
        UINT64 completed = g_State.fence->GetCompletedValue();
        if (completed < g_State.fenceValues[idx]) {
            // Reusable event handle — created once, persists for the DLL lifetime
            static HANDLE s_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            bool fenceReady = false;
            if (s_fenceEvent) {
                HRESULT evHr = g_State.fence->SetEventOnCompletion(g_State.fenceValues[idx], s_fenceEvent);
                if (SUCCEEDED(evHr)) {
                    DWORD waitResult = WaitForSingleObject(s_fenceEvent, 1);  // 1ms max
                    completed = g_State.fence->GetCompletedValue();
                    fenceReady = (completed >= g_State.fenceValues[idx]);

                    static int s_fenceWaitLog = 0;
                    if (fenceReady && s_fenceWaitLog++ < 10)
                        HookLogImportant(
                            "DX12: PostSL fence wait resolved via event (alloc[%d] completed=%llu needed=%llu "
                            "waitResult=%lu)",
                            idx, completed, g_State.fenceValues[idx], waitResult);
                }
            }
            if (!fenceReady) {
                s_postSLSkipFence.fetch_add(1, std::memory_order_relaxed);
                if (s_callsSinceReactivation <= 20)
                    HookLogImportant(
                        "DX12: PostSL EARLY-EXIT: alloc[%d] in-flight after 1ms wait (completed=%llu needed=%llu "
                        "call#%d)",
                        idx, completed, g_State.fenceValues[idx], s_callsSinceReactivation);
                bb->Release();
                return;
            }
        }
    }

    HRESULT allocResetHr = alloc->Reset();
    if (FAILED(allocResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: alloc->Reset failed hr=0x%08X (call#%d)", allocResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }
    HRESULT listResetHr = list->Reset(alloc, nullptr);
    if (FAILED(listResetHr)) {
        if (s_callsSinceReactivation <= 20)
            HookLogImportant("DX12: PostSL EARLY-EXIT: list->Reset failed hr=0x%08X (call#%d)", listResetHr,
                             s_callsSinceReactivation);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g. SL's
    // FG re-init failed), bail early instead of causing a cascade crash.
    {
        HRESULT preDevHr = dev->GetDeviceRemovedReason();
        if (FAILED(preDevHr)) {
            HookLogImportant(
                "DX12: PostSL EARLY-EXIT: device already removed BEFORE submit "
                "(hr=0x%08X epoch=%d call#%d)",
                preDevHr, s_reactivationEpoch, s_callsSinceReactivation);
            bb->Release();
            return;
        }
    }

    bool rendered = false;

    if (s_callsSinceReactivation <= 1 || s_postSLRenders.load(std::memory_order_relaxed) == 0) {
        HookLogImportant(
            "DX12: PostSL first ECL submit approaching (epoch=%d call#=%d queue=%p slFG=%d "
            "runtimeMode=%d hadFSR=%d)",
            s_reactivationEpoch, s_callsSinceReactivation, queue, cachedSLFGActive ? 1 : 0,
            (int)g_FGCompat.GetRuntimeMode(), g_HadFSRFGPhase ? 1 : 0);
    }

    const bool selectedQueueIsSwapchainQueue = (queue == scQueue);
    // Fast post-FSR DLSS probe: when the safe-bootstrap proof holds and the
    // overlay submits on the runtime-owned swapchain queue (not the documented
    // origGame first-ECL crash case), one scratch-barrier health frame replaces
    // the full ~4-frame graduated probe so the DLSS-engage overlay seam drops
    // to a single present. Unproven/off-swapchain-queue paths keep the full probe.
    const bool fastPostFSRDLSSProbe = ce::dx12_overlay_policy::ShouldUseFastPostFSRDLSSProbeForSafeBootstrap(
        g_HadFSRFGPhase, safePostFSRBootstrapPathForPostSL, selectedQueueIsSwapchainQueue, cachedSLFGActive);
    const int postFSRProbeFramesPerLevel = fastPostFSRDLSSProbe ? 1 : kPostFSRProbeFramesPerLevel;
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    ID3D12CommandQueue* realQ = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
    ExecuteCommandListsPtr selectedQueueOrigECL = GetOriginalExecuteCommandLists(queue);
    const bool selectedQueueOrigECLMatchesRealECL = selectedQueueOrigECL && selectedQueueOrigECL == realECL;
    bool isSLWrapperQ = ce::dx12_overlay_policy::ShouldTreatPostSLSelectedQueueAsWrapper(
        queue == g_OriginalGameQueue, queue == g_PostSLDedicatedQueue, selectedQueueIsSwapchainQueue,
        selectedQueueOrigECLMatchesRealECL);
    const bool useExplicitPostFSRSwapchainTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferTransitionsForPostFSRSwapchainQueuePath(
            g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool usePostSLOffscreenComposite = ce::dx12_overlay_policy::ShouldUsePostSLOffscreenCompositeAfterFSR(
        g_HadFSRFGPhase, cachedSLFGActive, selectedQueueIsSwapchainQueue, isSLWrapperQ);
    const bool useExplicitPostFSRBackbufferCopyTransitions =
        ce::dx12_overlay_policy::ShouldUseExplicitBackbufferCopyTransitionsForPostFSROffscreenComposite(
            usePostSLOffscreenComposite, useExplicitPostFSRSwapchainTransitions);
    const bool hasSelectedQueueSubmitPath = selectedQueueOrigECL != nullptr || realECL != nullptr;
    const bool hasWrapperDerivedDirectPath = realQ != nullptr && realECL != nullptr;
    const bool preferSelectedSwapchainQueueSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedSwapchainQueueSubmitAfterFSR(
            g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, isSLWrapperQ, hasSelectedQueueSubmitPath,
            hasWrapperDerivedDirectPath);
    const bool preferSelectedQueueDirectSubmitAfterFSR =
        ce::dx12_overlay_policy::ShouldUsePostSLSelectedQueueDirectSubmitAfterFSR(
            g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
            selectedQueueOrigECLMatchesRealECL, realQ != nullptr);

    // Zero-frame post-FSR DLSS reactivation: when the fast-bootstrap proof holds and we submit on the
    // SL-owned swapchain queue, the real overlay render is itself the device-health proof (the
    // pre-submit GetDeviceRemovedReason bail above + the post-submit device-removed check), so the
    // separate scratch-barrier probe present is redundant and only costs the documented 1-present
    // `postsl-bootstrap-reactivation` flicker on every DLSS engage. Skip straight to full-render level
    // and draw the overlay directly on this first reactivation present. The slower graduated probe is
    // retained for the unproven / off-swapchain-queue fragile paths (fastPostFSRDLSSProbe=false there).
    if (ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnFirstPostFSRDLSSReactivation(
            fastPostFSRDLSSProbe, g_PostFSRProbeLevel.load(std::memory_order_acquire))) {
        HookLogImportant(
            "DX12: PostSL post-FSR fast bootstrap — rendering overlay directly on first reactivation present "
            "(skipping redundant scratch-barrier probe; render's own pre/post devRemoved check is the health "
            "proof) queue=%p scQueue=%p epoch=%d",
            queue, scQueue, s_reactivationEpoch);
        g_PostFSRProbeLevel.store(3, std::memory_order_release);
        g_PostFSRProbeFrames.store(0, std::memory_order_release);
    }

    // --- Post-FSR graduated probing ---
    // Level 0: Scratch resource barrier (confirms queue/device path works)
    // Level 1: Reserved for future backbuffer-specific probes
    // Level 2: Offscreen copy-only pass (touch swapchain only via copy ops)
    // Level 3+: Full offscreen composite/render is allowed
    bool isPostFSRProbe = g_HadFSRFGPhase && g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3;

    // For post-FSR rendering, use SL's wrapper queue captured from ECL detour.
    // origGame's driver-internal state tracking for FSR-created swapchain backbuffers is
    // invalid (FSR created the swapchain on its own queue, origGame never saw the backbuffers).
    // SL's wrapper queue dispatches through SL's ECL interception which knows the correct state.
    ID3D12CommandQueue* slWrapperQueue = nullptr;
    ID3D12CommandQueue* liveSLWrapperQueue = nullptr;
    bool usingPinnedPostFSRWrapperQueue = false;
    if (g_HadFSRFGPhase) {
        std::lock_guard<std::recursive_mutex> ql(g_CommandQueueMutex);
        liveSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);

        ID3D12CommandQueue* pinnedSLWrapperQueue = g_PostSLPinnedSLWrapperQueue;
        ID3D12CommandQueue* wrapperCandidate = pinnedSLWrapperQueue ? pinnedSLWrapperQueue : liveSLWrapperQueue;
        if (!wrapperCandidate) {
            // Fallback: try g_CommandQueue if it's not origGame or scQueue.
            ID3D12CommandQueue* cmdQ = g_CommandQueue.load(std::memory_order_acquire);
            if (cmdQ && cmdQ != g_OriginalGameQueue && cmdQ != g_SwapchainQueue)
                wrapperCandidate = cmdQ;
        }
        if (wrapperCandidate == g_OriginalGameQueue || wrapperCandidate == g_SwapchainQueue)
            wrapperCandidate = nullptr;

        if (ce::dx12_overlay_policy::ShouldPinPostSLWrapperQueueAfterFSR(
                g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue,
                pinnedSLWrapperQueue != nullptr, wrapperCandidate != nullptr,
                preferSelectedSwapchainQueueSubmitAfterFSR)) {
            wrapperCandidate->AddRef();
            g_PostSLPinnedSLWrapperQueue = wrapperCandidate;
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

    if (isPostFSRProbe) {
        // Log comprehensive diagnostics on first probe frame
        if (g_PostFSRProbeFrames.load(std::memory_order_acquire) == 0 &&
            g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
            D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
            HookLogImportant("DX12: PostSL post-FSR DIAG: pSwapChain=%p bb=%p bufIdx=%u bbW=%u bbH=%u", pSwapChain, bb,
                             bufIdx, (unsigned)bbDesc.Width, bbDesc.Height);
            HookLogImportant("DX12: PostSL post-FSR DIAG: queue=%p origGame=%p slWrapper=%p scQ=%p", queue,
                             g_OriginalGameQueue, slWrapperQueue, g_SwapchainQueue);
        }

        bool probeHandled = true;
        const bool preferRealQueueBehindWrapperAfterFSR =
            ce::dx12_overlay_policy::ShouldUsePostSLRealQueueBehindWrapperAfterFSR(g_HadFSRFGPhase, cachedSLFGActive,
                                                                                   realQ != nullptr);
        const bool bootstrapRealQueueCaptureViaWrapperProbe =
            ce::dx12_overlay_policy::ShouldBootstrapPostSLRealQueueCaptureViaWrapperProbeAfterFSR(
                g_HadFSRFGPhase, cachedSLFGActive, g_PostFSRProbeLevel.load(std::memory_order_acquire),
                realQ != nullptr, slWrapperQueue != nullptr, hasSelectedQueueSubmitPath, isSLWrapperQ);
        if (preferRealQueueBehindWrapperAfterFSR && g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
            g_PostFSRProbeLevel.store(3, std::memory_order_release);
            g_PostFSRProbeFrames.store(0, std::memory_order_release);
            HookLogImportant(
                "DX12: PostSL post-FSR switching to direct real queue behind wrapper %p — skipping level 2 probe",
                realQ);
            bb->Release();
            return;
        }
        // CRITICAL: Always use the locked queue (stable across frames) for probe
        // submissions, NOT the transient slWrapperQueue (g_SLWrapperQueue) which
        // changes as different SL wrapper queues are seen by the ECL detour on
        // other threads.  Using a transient wrapper mid-probe causes DEVICE_REMOVED
        // when the new wrapper doesn't own the swapchain's resource state.
        ID3D12CommandQueue* probeQueue = bootstrapRealQueueCaptureViaWrapperProbe
                                             ? queue
                                             : ((g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1 &&
                                                 slWrapperQueue && !preferSelectedSwapchainQueueSubmitAfterFSR)
                                                    ? slWrapperQueue
                                                    : queue);

        if (g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0) {
            // Probe 0: Scratch resource barrier on origGame — confirms queue works.
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
        } else if (g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1) {
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
                       g_HadFSRFGPhase, g_PostFSRProbeLevel.load(std::memory_order_acquire),
                       usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue)) {
            if (!EnsureOffscreenRT(dev, g_State.cachedWidth, g_State.cachedHeight, g_State.format)) {
                HookLogImportant(
                    "DX12: PostSL post-FSR copy-only probe could not create offscreen RT (w=%d h=%d fmt=%d)",
                    g_State.cachedWidth, g_State.cachedHeight, g_State.format);
                bb->Release();
                return;
            }

            D3D12_RESOURCE_BARRIER toCopyDest = {};
            toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopyDest.Transition.pResource = g_State.offscreenRT;
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
            offDst.pResource = g_State.offscreenRT;
            offDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            offDst.SubresourceIndex = 0;
            list->CopyTextureRegion(&offDst, 0, 0, 0, &bbSrc, nullptr);

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = g_State.offscreenRT;
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
            offSrc.pResource = g_State.offscreenRT;
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
        } else if (g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 2) {
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
                    s_insidePostSLOverlayECL = true;
                    probeQueue->ExecuteCommandLists(1, lists);
                    s_insidePostSLOverlayECL = false;
                } else if (isSLWrapperQ && !g_RealQueueBehindSLWrapper.load(std::memory_order_acquire)) {
                    s_insidePostSLOverlayECL = true;
                    probeQueue->ExecuteCommandLists(1, lists);
                    s_insidePostSLOverlayECL = false;
                    ID3D12CommandQueue* capturedReal = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
                    if (capturedReal) {
                        HookLogImportant(
                            "DX12: PostSL post-FSR probe captured real queue %p behind wrapper bootstrap %p",
                            capturedReal, probeQueue);
                    }
                } else {
                    probeQueue->ExecuteCommandLists(1, lists);
                }
            }

            if (g_State.fence) {
                UINT64 next = g_State.currentFenceValue + 1;
                HRESULT sigHr = probeQueue->Signal(g_State.fence, next);
                if (SUCCEEDED(sigHr)) {
                    g_State.currentFenceValue = next;
                    if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                        g_State.fenceValues[idx] = next;
                }
            }

            HRESULT probeHr = dev->GetDeviceRemovedReason();
            g_PostFSRProbeFrames.fetch_add(1, std::memory_order_acq_rel);
            const char* probeNames[] = {"scratch-barrier", "SLwrapper-bb-barrier", "offscreen-copy-only"};
            const char* probeName = g_PostFSRProbeLevel.load(std::memory_order_acquire) < 3
                                        ? probeNames[g_PostFSRProbeLevel.load(std::memory_order_acquire)]
                                        : "unknown";
            HookLogImportant(
                "DX12: PostSL post-FSR PROBE level=%d (%s) frame=%d/%d queue=%p devRemoved=0x%08X %s (fast=%d)",
                g_PostFSRProbeLevel.load(std::memory_order_acquire), probeName,
                g_PostFSRProbeFrames.load(std::memory_order_acquire), postFSRProbeFramesPerLevel, probeQueue, probeHr,
                FAILED(probeHr) ? "FAILED" : "OK", fastPostFSRDLSSProbe ? 1 : 0);

            if (FAILED(probeHr)) {
                // DEVICE_REMOVED from BB barrier is FATAL — skip to barrier-free.
                // Scratch barrier failures are non-fatal (queue just isn't ready).
                int skipTo = (g_PostFSRProbeLevel.load(std::memory_order_acquire) >= 1)
                                 ? 2
                                 : g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
                g_PostFSRProbeLevel.store(static_cast<int>(skipTo), std::memory_order_release);
                g_PostFSRProbeFrames.store(0, std::memory_order_release);
                HookLogImportant("DX12: PostSL post-FSR probe FAILED, advancing to level %d",
                                 g_PostFSRProbeLevel.load(std::memory_order_acquire));
                bb->Release();
                return;
            }

            if (g_PostFSRProbeFrames >= postFSRProbeFramesPerLevel) {
                // Skip level 1 (BB barrier): go directly from level 0 to level 2.
                // BB barriers cause FATAL DEVICE_REMOVED on queues that don't own the
                // swapchain's resource state. Level 2 only validates copy traffic on
                // the swapchain timeline before any real overlay rendering is attempted.
                int nextLevel = (g_PostFSRProbeLevel.load(std::memory_order_acquire) == 0)
                                    ? (selectedQueueIsSwapchainQueue ? 3 : 2)
                                    : g_PostFSRProbeLevel.load(std::memory_order_acquire) + 1;
                g_PostFSRProbeLevel.store(static_cast<int>(nextLevel), std::memory_order_release);
                g_PostFSRProbeFrames.store(0, std::memory_order_release);
                HookLogImportant(
                    "DX12: PostSL post-FSR probe PASSED, advancing to level %d (selectedScQueue=%d skipped BB barrier "
                    "probe)",
                    g_PostFSRProbeLevel.load(std::memory_order_acquire), selectedQueueIsSwapchainQueue ? 1 : 0);
            }
            bb->Release();
            return;
        }
    }

    // Post-FSR: the DescFree backend contains device-level objects (PSO, root sig)
    // that work on any queue. Format mismatch is handled below (~line 4325).
    // No need to force-destroy — just reuse the existing backend.

    // Lazy-init DescFree backend if needed (same logic as pre-SL path).
    // The backend normally survives FG transitions warm (device-scoped); this
    // also rebuilds it after a device change.
    EnsureDescFreeBackendForDeviceAndFormat(dev, g_State.format, "PostSL lazy init");

    bool willRender = g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized();

    // Validate backbuffer format matches DescFree PSO format.
    // After FG transitions the swapchain may be recreated with a different format.
    // PSO/RTV format mismatch causes DEVICE_REMOVED.
    if (willRender && g_DescFreeBackend) {
        D3D12_RESOURCE_DESC bbDesc = bb->GetDesc();
        DXGI_FORMAT bbFmt = bbDesc.Format;
        DXGI_FORMAT psoFmt = g_State.format;
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
            g_State.format = bbFmt;
            willRender = EnsureDescFreeBackendForDeviceAndFormat(dev, bbFmt, "PostSL format mismatch");
        }
    }

    // PROBE: After FG transitions (epoch > 1), test queue health before full render.
    // Only do Probe 1 (empty ECL). Probe 2 (ClearRTV+barriers) is unsafe during PostSL
    // because the backbuffer state on origGame's timeline is unknown — SL manages
    // backbuffer state transitions internally, and cross-queue barrier assumptions fail.
    // The graduated post-FSR scratch-barrier probe above already validated the
    // runtime-owned swapchain queue under the fast-path proof, so the separate
    // empty-ECL probe is redundant there — skipping it removes the last probe
    // present from the DLSS-engage seam.
    // Pure-DLSS off->DLSS (hadFSR=0) does NOT take the fast post-FSR probe, so without this it spends
    // the first reactivation present on the empty-ECL probe and blanks the overlay for 1 present
    // (gate=postsl-transition-probe; session 20260615_014832). On the SL-owned swapchain queue the
    // real overlay render is itself the queue-health proof (pre-submit GetDeviceRemovedReason bail at
    // ~:11174 + post-submit check), so render directly instead. Off-swapchain-queue paths keep the probe.
    const bool transitionProbeDeviceHealthy = !FAILED(dev->GetDeviceRemovedReason());
    const bool renderDirectlyOnTransitionProbe =
        ce::dx12_overlay_policy::ShouldRenderOverlayDirectlyOnPostSLTransitionProbe(selectedQueueIsSwapchainQueue,
                                                                                    transitionProbeDeviceHealthy);
    int probesNeeded = (fastPostFSRDLSSProbe || renderDirectlyOnTransitionProbe) ? 0 : 1;
    bool isPostTransitionProbe = (s_reactivationEpoch > 1 && s_postSLProbeFrames < probesNeeded);
    if (renderDirectlyOnTransitionProbe && !fastPostFSRDLSSProbe && s_reactivationEpoch > 1 &&
        s_postSLProbeFrames == 0) {
        static int s_skipTransitionProbeLog = 0;
        if (s_skipTransitionProbeLog++ < 20 || (s_skipTransitionProbeLog % 120) == 0)
            HookLogImportant(
                "DX12: PostSL rendering overlay directly on first reactivation present — skipping redundant "
                "empty-ECL transition probe (swapchain queue, device healthy; render's pre/post devRemoved check "
                "is the proof) epoch=%d queue=%p scQueue=%p hadFSR=%d",
                s_reactivationEpoch, queue, scQueue, g_HadFSRFGPhase ? 1 : 0);
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

            D3D12_CPU_DESCRIPTOR_HANDLE probeRtv = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            probeRtv.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, probeRtv);

            float clearColor[4] = {0, 0, 0, 0};
            list->ClearRenderTargetView(probeRtv, clearColor, 0, nullptr);
