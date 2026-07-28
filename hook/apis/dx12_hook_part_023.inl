
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
                ExecuteCommandListsPtr eclFn = g_RealD3D12ECL.load(std::memory_order_acquire);
                if (eclFn) {
                    eclFn(queue, 1, probeList);
                } else {
                    queue->ExecuteCommandLists(1, probeList);
                }
            }
        }

        // Signal fence for allocator tracking
        if (g_State.fence) {
            UINT64 next = g_State.currentFenceValue + 1;
            HRESULT sigHr = queue->Signal(g_State.fence, next);
            if (SUCCEEDED(sigHr)) {
                g_State.currentFenceValue = next;
                if (idx >= 0 && idx < (int)g_State.fenceValues.size())
                    g_State.fenceValues[idx] = next;
            }
        }

        HRESULT probeHr = dev->GetDeviceRemovedReason();
        HookLogImportant("DX12: PostSL PROBE #%d on queue=%p (scQ=%p epoch=%d slWrapper=%d) — %s devRemoved=0x%08X %s",
                         s_postSLProbeFrames, queue, scQueue, s_reactivationEpoch, isSLWrapperQ ? 1 : 0,
                         s_postSLProbeFrames == 1 ? "empty ECL" : "ClearRTV+barriers", probeHr,
                         FAILED(probeHr) ? "FAILED" : "OK");
        if (FAILED(probeHr)) {
            bb->Release();
            return;
        }
        bb->Release();
        return;
    }

    // Cross-queue sync fence — used for SL wrapper queue ↔ origGame synchronization
    // Created lazily when needed for PostSL ECL dispatch on SL's wrapper queue.
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

    // Use cached FG state for barrier/queue decisions (prevents mid-function race).
    // The post-FSR selected-scQueue path is special: probes and the stable non-FG
    // ProcessFrame path both indicate the backbuffer behaves like PRESENT on that
    // queue, so keep using explicit PRESENT<->RT transitions there.
    const auto postSLBarrierMode = ce::dx12_overlay_policy::DecidePostSLBackbufferBarrierMode(
        cachedSLFGActive, useExplicitPostFSRSwapchainTransitions);
    bool slFGBarrierFree = postSLBarrierMode == ce::dx12_overlay_policy::PostSLBackbufferBarrierMode::kUavBarrierOnly;

    // During SL FG with direct submission (bypassing SL's wrapper), we can render
    // every frame since we no longer pollute SL's internal pipeline.
    // Keep real-frame detection for metrics updates but don't skip interpolated frames.

    // BB health diagnostic: log BB pointer, dimensions, ref count periodically
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
                usePostSLOffscreenComposite ? 1 : 0, g_HadFSRFGPhase ? 1 : 0, didXQSync ? 1 : 0);
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
        g_D3D11On12Adapter.SetIPCClient(g_IPC);
        g_D3D11On12Adapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
        const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
        if (metricsBinding.bindMetrics) {
            g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
        }
        if (metricsBinding.refreshFrameMetadata) {
            static const bool s_isVKD3D = []() {
                return GetModuleHandleA("d3d12core.dll") &&
                       (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
            }();
            const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
            g_D3D11On12Adapter.SetGraphicsAPI(api);
        }

        if (usePostSLOffscreenComposite &&
            EnsureOffscreenRT(dev, g_State.cachedWidth, g_State.cachedHeight, g_State.format)) {
            // Avoid binding the post-FSR DLSS backbuffer as an RTV on the first real
            // PostSL render. Instead composite through an offscreen RT and copy back.
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

            D3D12_RESOURCE_BARRIER toRenderTarget = {};
            toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toRenderTarget.Transition.pResource = g_State.offscreenRT;
            toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            list->ResourceBarrier(1, &toRenderTarget);

            s_descFreeCmdList = list;
            s_descFreeRtv = g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
            // PostSL/FG overlay: synchronized by the FG completion fence each
            // frame, so disable the DescFree per-slot guard (g_State.fence does
            // not track this value here — a non-zero guard would stall reuse).
            s_descFreeSlotFence = g_State.fence;
            s_descFreeSlotGuardValue = 0;
            SyncSecondaryDx12OverlayColorState(g_State.format);
            g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
            s_descFreeCmdList = nullptr;

            D3D12_RESOURCE_BARRIER toCopySource = {};
            toCopySource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopySource.Transition.pResource = g_State.offscreenRT;
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
        } else {
            // Recreate RTV for this buffer index (cheap CPU-side op)
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
            UINT rtvSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            rtvHandle.ptr += (SIZE_T)bufIdx * rtvSize;
            dev->CreateRenderTargetView(bb, nullptr, rtvHandle);

            s_descFreeCmdList = list;
            s_descFreeRtv = rtvHandle;
            // PostSL/FG overlay: synchronized by the FG completion fence (see above).
            s_descFreeSlotFence = g_State.fence;
            s_descFreeSlotGuardValue = 0;
            SyncSecondaryDx12OverlayColorState(g_State.format);
            g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth, g_State.cachedHeight);
            s_descFreeCmdList = nullptr;
        }
        rendered = true;
    } else {
        // Log why rendering was skipped (HookLogImportant for visibility after reactivation)
        static int s_backendSkip = 0;
        s_backendSkip++;
        if (s_backendSkip <= 10 || (s_backendSkip % 100) == 0)
            HookLogImportant("DX12: PostSL SKIP render #%d — backend=%p adapterInit=%d", s_backendSkip,
                             (void*)g_DescFreeBackend, g_D3D11On12Adapter.IsInitialized() ? 1 : 0);
    }

    // Post-rendering barrier: UAV during SL FG, standard RT→PRESENT otherwise
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

    // If we can't render, bail — don't submit empty command lists.
    if (!willRender) {
        list->Close();
        bb->Release();
        return;
    }

    HRESULT closeHr = list->Close();
    if (FAILED(closeHr)) {
        static int s_closeFailCount = 0;
        if (s_closeFailCount++ < 10)
            HookLog("DX12: PostSLOverlayRender — list->Close failed hr=0x%08X", closeHr);
        bb->Release();
        return;
    }

    // Pre-submit device health check: if device is already removed (e.g., by
    // SL's internal FG queue transition), don't submit — it would fail anyway.
    HRESULT preDevReason = dev->GetDeviceRemovedReason();
    if (FAILED(preDevReason)) {
        HookLogImportant("DX12: PostSL PRE-submit device already removed (hr=0x%08X queue=%p) — skipping",
                         (unsigned)preDevReason, queue);
        bb->Release();
        return;
    }

    const uint32_t preSyncLifecycleEpoch = g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
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
        return;
    }

    // CROSS-QUEUE GPU SYNC: When our overlay queue differs from the swapchain
    // queue (scQueue), the backbuffer was last used by SL's FG pipeline on
    // scQueue.  We MUST ensure SL's GPU work completes before our barriers
    // touch the backbuffer on a different queue.  Without this sync, the GPU
    // may execute our PRESENT→RT barrier in parallel with SL's FG work on the
    // same backbuffer, causing DEVICE_REMOVED.
    //
    // Pattern: Signal on scQueue (records SL's completion point) →
    //          Wait on our queue (stalls until SL finishes)
    //
    // This is the standard D3D12 cross-queue synchronization pattern.
    // During initial DLSS FG (scQueue=NULL), this is skipped — same-queue
    // guarantees GPU ordering naturally.
    //
    // EXCEPTION: During SL FG, scQueue may be SL's internal queue (captured
    // from CreateSwapChainForHwnd during FG init).  Signal/Wait on SL's queue
    // with our fence causes DEVICE_REMOVED.  Skip cross-queue sync entirely
    // during SL FG — SL manages its own synchronization.
    bool crossQueueSynced = didXQSync;  // SL→origGame sync from above
    if (scQueue && scQueue != queue && g_State.crossQueueFence && !cachedSLFGActive) {
        UINT64 syncVal = ++g_State.crossQueueFenceValue;
        // Signal on scQueue: "record SL's GPU progress"
        HRESULT sigHr = scQueue->Signal(g_State.crossQueueFence, syncVal);
        if (SUCCEEDED(sigHr)) {
            // Wait on our queue: "don't execute until scQueue catches up"
            HRESULT waitHr = queue->Wait(g_State.crossQueueFence, syncVal);
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
                    sigHr);
        }
    }

    // Submit ECL via virtual call on origGame during SL FG.
    //
    // CRITICAL: Do NOT use realECL(g_OriginalGameQueue, ...) — g_OriginalGameQueue
    // may be SL's COM wrapper object.  Calling the raw D3D12 ECL with an SL wrapper
    // as `this` is type confusion (internal field offsets differ).
    //
    // Virtual call → SL's COM wrapper vtable → SL processes → SL calls real D3D12
    // queue internally.  This lets SL properly track our ECL in its FG pipeline.
    //
    // For non-SL-FG paths (origECL/realECL): no change, those work as before.
    ID3D12CommandList* lists[] = {list};
    bool usedRealECL = false;
    bool usedOrigECL = false;
    bool usedVirtualCall = false;
    ID3D12CommandQueue* submittedQueue = queue;

    // Pre-submit device health check — if the device is already removed
    // (e.g. after FG teardown), skip the ECL to avoid triggering ERR_GFX_STATE.
    {
        auto* preSubmitDev = g_Device.load(std::memory_order_acquire);
        HRESULT preSubmitHr = preSubmitDev ? preSubmitDev->GetDeviceRemovedReason() : E_FAIL;
        if (FAILED(preSubmitHr)) {
            HookLogImportant("DX12: PostSL SKIPPING ECL — device removed 0x%08X (queue=%p)", (unsigned)preSubmitHr,
                             queue);
            g_DeviceRemoved.store(true, std::memory_order_release);
            bb->Release();
            return;
        }
    }

    // Diagnostic: on first few submits after each transition, log ECL function pointer comparison
    // (reset to 0 on PostSL REACTIVATION for fresh diagnostics)
    bool slFGAtDispatch = cachedSLFGActive;
    if (g_PostSLECLDiagCount.load(std::memory_order_relaxed) < 10) {
        ExecuteCommandListsPtr origECLDiag = GetOriginalExecuteCommandLists(queue);
        HookLogImportant(
            "DX12: PostSL ECL diag — queue=%p scQueue=%p origECL=%p realECL=%p match=%d sameQueue=%d slWrapper=%d "
            "slFG=%d hadFSR=%d",
            queue, scQueue, (void*)origECLDiag, (void*)realECL, origECLDiag == realECL ? 1 : 0,
            queue == scQueue ? 1 : 0, isSLWrapperQ ? 1 : 0, slFGAtDispatch ? 1 : 0, g_HadFSRFGPhase ? 1 : 0);
        g_PostSLECLDiagCount.fetch_add(1, std::memory_order_relaxed);
    }

    {
        const uint32_t preSubmitLifecycleEpoch = g_PostSLLifecycleEpoch.load(std::memory_order_acquire);
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
            return;
        }
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("PostSL overlay submit");
        if (slFGAtDispatch) {
            // When SL FG recreated the swapchain on a different queue (scQueue != origGame),
            // submit directly on scQueue.  SL's wrapper routes to origGame, causing
            // cross-queue backbuffer access → DEVICE_REMOVED.
            // PostSL fires after SL's FG pipeline completes, so scQueue is idle.
            bool scQueueDiffers = (scQueue && scQueue != g_OriginalGameQueue);

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
                ce::dx12_overlay_policy::ShouldUsePostSLScQueueVirtualSubmit(g_HadFSRFGPhase, scQueueDiffers);
            const bool preferSelectedSwapchainQueueDirectSubmitForPureDLSS =
                ce::dx12_overlay_policy::ShouldUseSelectedSwapchainQueueDirectSubmitForPureDLSS(
                    g_HadFSRFGPhase, selectedQueueIsSwapchainQueue, selectedQueueOrigECL != nullptr,
                    selectedQueueOrigECLMatchesRealECL);

            const bool useWrapperSubmitAfterFSR = ce::dx12_overlay_policy::ShouldUsePostSLWrapperSubmitAfterFSR(
                g_HadFSRFGPhase, usePostSLOffscreenComposite, selectedQueueIsSwapchainQueue, slQueue != nullptr,
                preferSelectedSwapchainQueueSubmitAfterFSR);

            if (useWrapperSubmitAfterFSR) {
                // After an FSR phase, keep swapchain-touching PostSL work on the SL
                // wrapper path when that is the only path that has successfully
                // survived the post-FSR copy probes. We can still capture the real
                // queue behind the wrapper for diagnostics and later promotion.
                submittedQueue = slQueue;
                s_insidePostSLOverlayECL = true;
                slQueue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
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
                s_insidePostSLOverlayECL = true;
                scQueue->ExecuteCommandLists(1, lists);
                s_insidePostSLOverlayECL = false;
                usedVirtualCall = true;

                static int s_scQSubmitLog = 0;
                if (s_scQSubmitLog < 5 || (s_scQSubmitLog % 500 == 0))
                    HookLogImportant("DX12: PostSL scQueue submit #%d on %p (origGame=%p, bypassing SL wrapper)",
                                     s_scQSubmitLog, scQueue, g_OriginalGameQueue);
                s_scQSubmitLog++;
            } else if (realQ && realECL) {
                // Direct submission: bypass SL's wrapper entirely
                submittedQueue = realQ;
                s_insidePostSLOverlayECL = true;
                realECL(realQ, 1, lists);
                s_insidePostSLOverlayECL = false;
                usedRealECL = true;

                static int s_directLog = 0;
                if (s_directLog < 5 || (s_directLog % 500 == 0))
                    HookLogImportant("DX12: PostSL DIRECT submit #%d on real queue %p (bypass SL wrapper)", s_directLog,
                                     realQ);
                s_directLog++;
            } else {
                const bool allowWrapperBootstrap = ce::dx12_overlay_policy::ShouldAllowPostSLWrapperBootstrap(
                    g_HadFSRFGPhase, realQ != nullptr, realECL != nullptr);
                if (!allowWrapperBootstrap) {
                    // For pure-DLSS startup (no FSR history), the real ECL might not
                    // be available yet if the deferred ECL probe hasn't fired (it's
                    // deferred until the Streamline startup window expires, and the
                    // window may still be active when PostSL first renders).  In this
                    // case, selectedQueueOrigECL is still valid (saved from the vtable
                    // hook on the swapchain queue).  Fall back to submitting through
                    // selectedQueueOrigECL on the queue itself rather than refusing
                    // and dropping every overlay frame.
                    if (!g_HadFSRFGPhase && selectedQueueOrigECL && selectedQueueIsSwapchainQueue) {
                        submittedQueue = queue;
                        selectedQueueOrigECL(queue, 1, lists);
                        usedOrigECL = true;
                        static int s_pureDLSSBootstrapFallbackLog = 0;
                        if (s_pureDLSSBootstrapFallbackLog < 10) {
                            HookLogImportant(
                                "DX12: PostSL pure-DLSS bootstrap fallback via selectedQueueOrigECL on %p "
                                "(realECL not yet probed, scQueue=%p)",
                                queue, scQueue);
                        }
                        s_pureDLSSBootstrapFallbackLog++;
                    } else {
                        HookLogImportant(
                            "DX12: PostSL refusing SL wrapper bootstrap without direct path "
                            "(queue=%p scQueue=%p wrapper=%p)",
                            queue, scQueue, (void*)slQueue);
                        bb->Release();
                        return;
                    }
                }

                // Bootstrap: submit through SL's wrapper to capture real queue on first call
                if (!slQueue && g_HadFSRFGPhase) {
                    HookLogImportant(
                        "DX12: PostSL refusing post-FSR bootstrap without SL wrapper queue (queue=%p scQueue=%p)",
                        queue, scQueue);
                    bb->Release();
                    return;
                }
                if (slQueue) {
                    submittedQueue = slQueue;
                    s_insidePostSLOverlayECL = true;
                    slQueue->ExecuteCommandLists(1, lists);
                    s_insidePostSLOverlayECL = false;
                    usedVirtualCall = true;
                    HookLogImportant(
                        "DX12: PostSL bootstrap via SL wrapper %p (will capture real queue for direct path)", slQueue);
                } else {
                    if (!ce::dx12_overlay_policy::ShouldAllowPostSLDirectVirtualBootstrapWithoutWrapper(
                            slFGAtDispatch, slQueue != nullptr, realQ != nullptr, realECL != nullptr,
                            selectedQueueIsSwapchainQueue, selectedQueueOrigECLMatchesRealECL,
                            queue == g_OriginalGameQueue)) {
                        HookLogImportant(
                            "DX12: PostSL refusing no-wrapper virtual bootstrap during Streamline FG "
                            "(queue=%p scQueue=%p realQ=%p realECL=%p)",
                            queue, scQueue, realQ, (void*)realECL);
                        bb->Release();
