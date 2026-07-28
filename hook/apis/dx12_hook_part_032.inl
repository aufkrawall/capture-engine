                        HookLogImportant("DX12: Scene transition cooldown complete — resuming overlay");
                    NoteDX12OverlayCoverageGate("scene-transition-cooldown");
                    goto skip_overlay_draw;
                }
            }

            // Periodic health log for debugging stability
            static std::atomic<uint64_t> s_overlayFrameCount{0};
            uint64_t frameNum = s_overlayFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (frameNum == 1 || frameNum == 10 || frameNum == 50 || frameNum == 100 || (frameNum % 500) == 0) {
                ID3D12Device* dev = g_Device.load();
                HRESULT devRemovedHr = dev ? dev->GetDeviceRemovedReason() : E_FAIL;
                HookLogImportant(
                    "DX12: Overlay frame #%llu (deviceRemoved=0x%08X, fgActive=%d, "
                    "queue=%p, allocIdx=%d, slFGRunning=%d)",
                    (unsigned long long)frameNum, (unsigned)devRemovedHr, currentFGActive ? 1 : 0, gameQueue,
                    g_State.allocIndex, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
            }

            // Check device removed BEFORE rendering.  On first detection, tear
            // down overlay resources and set g_DeviceRemoved so heartbeats stop
            // (letting the freeze watchdog create a dump if we spin forever).
            {
                ID3D12Device* devCheck = g_Device.load();
                if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
                    if (!g_DeviceRemoved.load(std::memory_order_relaxed)) {
                        g_DeviceRemoved.store(true, std::memory_order_release);
                        DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
                        g_RenderWatchdog.SetForceMonitor(true);
                        HookLogImportant("DX12: GPU device removed (0x%08X) — cleaning up overlay",
                                         (unsigned)devCheck->GetDeviceRemovedReason());
                        ce::dx12_dred::DumpOnDeviceRemoved(devCheck, "D3D12 device removed before overlay render");
                        const bool recentFocusTransition =
                            g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
                            g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
                        if (s_WrappedPresentFocusLossContext.valid &&
                            (!processHasForeground || recentFocusTransition)) {
                            RequestFocusLossDeviceRemovalDumpOnce(
                                "D3D12 focus-loss device removal before overlay render",
                                devCheck->GetDeviceRemovedReason(), s_WrappedPresentFocusLossContext, foregroundWindow,
                                foregroundPid, frameDesc.OutputWindow, currentProcessId, gameQueue);
                        }
                        g_State.overlayInit = false;
                        CleanupRTVs();
                    }
                    goto overlay_done;
                } else if (g_DeviceRemoved.load(std::memory_order_relaxed)) {
                    // Device recovered (new device set via DX12_SetCommandQueue)
                    g_DeviceRemoved.store(false, std::memory_order_release);
                    DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
                    g_RenderWatchdog.SetForceMonitor(false);
                    ce::dx12_dred::ResetDumpEpoch();
                    HookLogImportant("DX12: Device recovered — overlay will reinitialize");
                }
            }

            {
                int allocatorPoolSize = static_cast<int>(g_State.allocators.size());
                if (allocatorPoolSize <= 0) {
                    goto overlay_done;
                }

                int idx = g_State.allocIndex % allocatorPoolSize;
                g_State.allocIndex = (idx + 1) % allocatorPoolSize;

                // With 16 allocators, we never need to wait under normal conditions.
                // However, during Alt+Tab / GPU throttle, the GPU may stall and the
                // fence value for this allocator slot won't advance.  We must check
                // before Reset() to avoid undefined behaviour (driver hang / crash).
                auto* list = g_State.cmdList;
                auto* alloc = (idx < (int)g_State.allocators.size()) ? g_State.allocators[idx] : nullptr;
                if (list && alloc) {
                    // Guard: skip overlay render if this allocator is still in flight.
                    if (g_State.fence && idx < (int)g_State.fenceValues.size() && g_State.fenceValues[idx] > 0) {
                        UINT64 completed = g_State.fence->GetCompletedValue();
                        if (completed < g_State.fenceValues[idx]) {
                            if (activeDebugSample) {
                                activeDebugSample->flags |= kPresentSampleFlagAllocatorBusy;
                            }
                            static std::atomic<int> s_allocSkipLogs{0};
                            if (s_allocSkipLogs.fetch_add(1, std::memory_order_relaxed) < 30) {
                                HookLog(
                                    "DX12: Allocator[%d] still in-flight (completed=%llu, needed=%llu), "
                                    "skipping overlay this frame",
                                    idx, completed, g_State.fenceValues[idx]);
                            }
                            goto overlay_done;
                        }
                    }
                    HRESULT allocResetHr = alloc->Reset();
                    if (SUCCEEDED(allocResetHr)) {
                        HRESULT listResetHr = list->Reset(alloc, nullptr);
                        // Log Reset results during FG for diagnostics
                        if (g_FGCompat.IsFGActive() || slFGActive) {
                            static std::atomic<int> s_fgResetLogs{0};
                            int fgResetLog = s_fgResetLogs.fetch_add(1, std::memory_order_relaxed);
                            if (fgResetLog < 5) {
                                HookLogImportant(
                                    "DX12: FG overlay alloc/list Reset (allocHr=0x%08X listHr=0x%08X idx=%d)",
                                    (unsigned)allocResetHr, (unsigned)listResetHr, idx);
                            }
                        }
                        if (SUCCEEDED(listResetHr)) {
                            // GPU-breadcrumb: stamp the start of CE's overlay command list so a native-FSR
                            // ffxQuery wedge can be attributed to CE's GPU ops vs a fence/CPU deadlock.
                            BeginOverlayGpuBreadcrumbFrame(g_Device.load(std::memory_order_acquire));
                            WriteOverlayGpuBreadcrumb(list, kOverlayBcStart);
                            const bool preserveLiveStartupOverlayDuringInactiveSL =
                                ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
                            const bool hasPendingStartupOverlayResources = g_OverlayAdapter.HasPendingDX12Resources();
                            const bool shouldPrimeStartupOverlayResources =
                                s_startupOverlayResourcePrimeMs == 0 &&
                                ce::dx12_overlay_policy::ShouldPrimeStartupOverlayResources(
                                    startupOverlayCompatibilityActive, hasPendingStartupOverlayResources,
                                    preserveLiveStartupOverlayDuringInactiveSL);
                            if (startupOverlayCompatibilityActive && hasPendingStartupOverlayResources &&
                                preserveLiveStartupOverlayDuringInactiveSL) {
                                static std::atomic<int> s_skipStartupPrimeForLiveOverlayLogCount{0};
                                const int skipPrimeLog =
                                    s_skipStartupPrimeForLiveOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
                                if (skipPrimeLog < 5 || (skipPrimeLog % 300) == 0) {
                                    HookLogImportant(
                                        "DX12: Skipping startup resource priming delay because live overlay is "
                                        "preserved through runtime-inactive Streamline handoff (log=%d)",
                                        skipPrimeLog + 1);
                                }
                            }
                            if (shouldPrimeStartupOverlayResources) {
                                // Check device before priming — after FG teardown the
                                // device may already be removed (async GPU fault).
                                {
                                    auto* primeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT primeDevHr = primeDev ? primeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(primeDevHr)) {
                                        HookLogImportant("DX12: SKIPPING resource priming — device removed 0x%08X",
                                                         (unsigned)primeDevHr);
                                        g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }
                                HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
                                if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
                                    HookLogImportant(
                                        "DX12: DX12 overlay resource priming failed; deferring first overlay draw");
                                    goto overlay_done;
                                }

                                HRESULT closeHr = list->Close();
                                if (FAILED(closeHr)) {
                                    HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit",
                                            closeHr);
                                    g_State.syncInit = false;
                                    goto overlay_done;
                                }

                                if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming",
                                                              false)) {
                                    HookLogImportant(
                                        "DX12: Startup resource priming submission failed; deferring first overlay "
                                        "draw");
                                    goto overlay_done;
                                }

                                // Check device after priming submit — catch async GPU fault immediately
                                {
                                    auto* postPrimeDev = g_Device.load(std::memory_order_acquire);
                                    HRESULT postPrimeDevHr =
                                        postPrimeDev ? postPrimeDev->GetDeviceRemovedReason() : E_FAIL;
                                    if (FAILED(postPrimeDevHr)) {
                                        HookLogImportant("DX12: Resource priming CAUSED device removal 0x%08X!",
                                                         (unsigned)postPrimeDevHr);
                                        g_DeviceRemoved.store(true, std::memory_order_release);
                                        goto overlay_done;
                                    }
                                }

                                s_startupOverlayResourcePrimeMs = GetTickCount64();
                                HookLogImportant(
                                    "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for "
                                    "%llums",
                                    kStartupOverlayPostResourcePrimeSettleMs);
                                goto overlay_done;
                            }

                            if (shouldRunStartupOverlayDrawProbe &&
                                s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kNone) {
                                // Probe system removed: go straight to rendering.
                                // The 3-stage probe (backbuffer touch → pipeline state → real draw) caused
                                // ERR_GFX_STATE in GTA5 Enhanced because even barrier-only probes on a
                                // dedicated overlay queue conflict with the game's D3D12 state tracking.
                                // With single-queue mode (fix for dedicated queue), we can render directly.
                                s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kActualRender;
                            }

                            IDXGISwapChain3* sc3 = g_State.cachedSC3;
                            if (!sc3) {
                                if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                                    sc3->Release();           // drop QI ref — weak cache is safe
                                    g_State.cachedSC3 = sc3;  // because swapchain is alive during Present
                                }
                            }
                            LARGE_INTEGER perfQI, perfGetBuf, perfRecord, perfSubmit, perfEnd, perfFreq;
                            QueryPerformanceFrequency(&perfFreq);
                            QueryPerformanceCounter(&perfQI);
                            if (sc3) {
                                UINT swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
                                currentBackBufferIdx = swapchainBufferIdx;
                                hasCurrentBackBufferIdx = true;
                                // CRITICAL FIX: Use actual swapchain buffer index directly
                                // CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
                                // so no need to wrap the index - this prevents sync issues
                                UINT bufferIdx = swapchainBufferIdx;
                                // Validate buffer index is within our allocated range
                                if (bufferIdx >= (UINT)g_State.bufferCount) {
                                    HookLog(
                                        "DX12: Buffer index %u exceeds allocated count %d, "
                                        "clamping",
                                        bufferIdx, g_State.bufferCount);
                                    bufferIdx = g_State.bufferCount - 1;
                                }
                                // FG-SAFE: Acquire backbuffer per-frame via GetBuffer.
                                // We do NOT cache backbuffer pointers because FSR FG
                                // monitors reference counts and crashes if extra refs
                                // are held persistently.
                                ID3D12Resource* bb = nullptr;
                                bool bbNeedsRelease = false;
                                QueryPerformanceCounter(&perfGetBuf);
                                if (SUCCEEDED(sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb) {
                                    bbNeedsRelease = true;
                                    // Recreate RTV for this buffer index (cheap CPU-side op).
                                    // Ensures RTV matches current buffer even after FSR FG
                                    // swapchain transitions.
                                    D3D12_CPU_DESCRIPTOR_HANDLE rtvRecreate =
                                        g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                    rtvRecreate.ptr += (SIZE_T)bufferIdx * g_State.rtvDescriptorSize;
                                    g_Device.load()->CreateRenderTargetView(bb, nullptr, rtvRecreate);
                                }
                                if (bb) {
                                    bool cmdRecordOk = false;
                                    static std::atomic<int> s_firstBackBufferLogCount{0};
                                    if (s_firstBackBufferLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                        HookLogImportant(
                                            "DX12: Preparing overlay draw for backbuffer idx=%u resource=%p via %s "
                                            "queue (queue=%p)",
                                            bufferIdx, bb,
                                            g_State.overlayQueue
                                                ? "dedicated overlay"
                                                : (gameQueue == g_SwapchainQueue ? "swapchain" : "game"),
                                            gameQueue);
                                    }

                                    // SL FG diagnostic: log every overlay draw during FG
                                    if (slFGActive) {
                                        static std::atomic<int> s_slFGDrawCount{0};
                                        int fgDraw = s_slFGDrawCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                        if (fgDraw <= 20 || (fgDraw % 10) == 0) {
                                            auto* diagDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT devHr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                                            bool dedicated = g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
                                            HookLogImportant(
                                                "DX12: SL-FG overlay ENTER #%d (bufIdx=%u bb=%p queue=%p dedQ=%d "
                                                "tid=0x%04X devRemoved=0x%08X)",
                                                fgDraw, bufferIdx, bb, gameQueue, dedicated ? 1 : 0,
                                                GetCurrentThreadId(), (unsigned)devHr);
                                        }
                                    }

                                    // GPU drain: flush all in-flight GPU work before first
                                    // overlay render after FSR→DLSS transition.  This ensures
                                    // SL's FG pipeline has fully completed before we touch
                                    // the backbuffer, preventing GPU-side deadlock/TDR.
                                    if (g_NeedGPUDrainBeforeRender && gameQueue) {
                                        auto* drainDev = g_Device.load(std::memory_order_acquire);
                                        if (drainDev) {
                                            if (!g_DrainFence) {
                                                HRESULT hr = drainDev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                                                   IID_PPV_ARGS(&g_DrainFence));
                                                if (SUCCEEDED(hr)) {
                                                    g_DrainEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                                                    g_DrainFenceValue = 0;
                                                    HookLogImportant("DX12: GPU drain fence created");
                                                } else {
                                                    HookLogImportant("DX12: GPU drain fence creation failed hr=0x%08X",
                                                                     (unsigned)hr);
                                                }
                                            }
                                            if (g_DrainFence && g_DrainEvent) {
                                                UINT64 drainVal = ++g_DrainFenceValue;
                                                HRESULT sigHr = gameQueue->Signal(g_DrainFence, drainVal);
                                                if (SUCCEEDED(sigHr)) {
                                                    if (g_DrainFence->GetCompletedValue() < drainVal) {
                                                        g_DrainFence->SetEventOnCompletion(drainVal, g_DrainEvent);
                                                        DWORD waitResult = WaitForSingleObject(g_DrainEvent, 5000);
                                                        HookLogImportant(
                                                            "DX12: GPU drain completed (wait=%s val=%llu queue=%p)",
                                                            waitResult == WAIT_OBJECT_0
                                                                ? "OK"
                                                                : (waitResult == WAIT_TIMEOUT ? "TIMEOUT" : "FAIL"),
                                                            drainVal, gameQueue);
                                                    } else {
                                                        HookLogImportant(
                                                            "DX12: GPU drain — already complete (val=%llu)", drainVal);
                                                    }
                                                } else {
                                                    HookLogImportant("DX12: GPU drain Signal failed hr=0x%08X",
                                                                     (unsigned)sigHr);
                                                }
                                            }
                                        }
                                        g_NeedGPUDrainBeforeRender = false;
                                    }

                                    // ================================================================
                                    // PRIMARY PATH: Barrier-free offscreen compositing.
                                    // Renders overlay to offscreen RT (no swapchain stall),
                                    // then copies to backbuffer with implicit state promotion.
                                    // DComp disabled — Direct Flip prevents reliable display.
                                    // ================================================================

                                    // ============================================================
                                    // PRIMARY: native DX12 overlay.
                                    // x64 keeps the descriptor-free root-SRV font path that avoids
                                    // SetDescriptorHeaps + OMSetRenderTargets(swapchain).  x86 uses
                                    // the Texture2D/SRV backend because DRED isolated the 32-bit
                                    // hang to the descriptor-free text sampling path.
                                    // ============================================================
                                    bool usedPrimaryOverlayBackend = false;
                                    bool usedDescFree = false;
                                    bool offscreenCompositeRequired = false;
                                    {
                                        auto* dev = g_Device.load();
#if defined(_WIN64)
                                        constexpr bool kIs32BitProcess = false;
#else
                                        constexpr bool kIs32BitProcess = true;
#endif
                                        const bool useTextureDx12Backend =
                                            ce::dx12_overlay_policy::ShouldUseTextureDx12OverlayBackendForProcess(
                                                kIs32BitProcess);
                                        // Pre-DescFree device health check
                                        if (dev) {
                                            HRESULT preDescFreeDevHr = dev->GetDeviceRemovedReason();
                                            if (FAILED(preDescFreeDevHr)) {
                                                HookLogImportant(
                                                    "DX12: DEVICE ALREADY REMOVED before DescFree init "
                                                    "(devRemoved=0x%08X tid=0x%04X)",
                                                    (unsigned)preDescFreeDevHr, GetCurrentThreadId());
                                            }
                                        }
                                        if (useTextureDx12Backend && g_DescFreeBackend) {
                                            ShutdownDescFreeBackend("x86 Texture2D backend selected");
                                        }
                                        if (dev && useTextureDx12Backend && !g_D3D11On12Adapter.IsInitialized()) {
                                            ID3D12CommandQueue* backendQueue =
                                                gameQueue ? gameQueue : g_CommandQueue.load(std::memory_order_acquire);
                                            if (backendQueue &&
                                                g_D3D11On12Adapter.InitDX12(dev, backendQueue, g_State.format)) {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend ready "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)g_State.format);
                                            } else {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend init failed "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)g_State.format);
                                            }
                                        } else if (dev && !useTextureDx12Backend) {
                                            // Reuses the warm device-scoped backend when device and
                                            // format still match; rebuilds it otherwise.
                                            EnsureDescFreeBackendForDeviceAndFormat(dev, g_State.format,
                                                                                    "normal route");
                                        }
                                        const bool primaryOverlayReady =
                                            useTextureDx12Backend
                                                ? g_D3D11On12Adapter.IsInitialized()
                                                : (g_DescFreeBackend && g_D3D11On12Adapter.IsInitialized());
                                        if (primaryOverlayReady) {
                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                            g_D3D11On12Adapter.SetReserveInactiveFGSpace(
                                                ShouldReserveInactiveFGOverlaySpaceNow());

                                            // After FSR→DLSS: ANY direct backbuffer access (barriers,
                                            // RT, ClearRTV) causes DEVICE_HUNG because SL's FG pipeline
                                            // has in-flight work on the backbuffer from another queue.
                                            // Instead, render to offscreen RT, then CopyTextureRegion
                                            // to the backbuffer.  CopyTextureRegion uses COPY_DEST state
                                            // which IS implicitly promotable from COMMON/PRESENT — no
                                            // explicit barrier needed on the backbuffer.
                                            // Two-copy offscreen compositing was designed for PostSL
                                            // rendering where the backbuffer's state is unknown.
                                            // For pre-SL rendering on the game thread, the backbuffer
                                            // is in PRESENT state (game transitioned it before Present).
                                            // Use normal direct rendering with PRESENT→RT→PRESENT barriers.
                                            //
                                            // EXCEPTION: After FSR→DLSS→OFF, the backbuffer state is
                                            // indeterminate (FG pipeline may have left it in any state).
                                            // Use offscreen compositing to avoid explicit barriers on
                                            // the backbuffer entirely.  Cleared on clean swapchain
                                            // transition.
                                            const bool usePostFSROffscreenCopy =
                                                g_NeedOffscreenOverlayAfterPostFSRNonFG;
                                            bool useOffscreenCopy = usePostFSROffscreenCopy;

                                            if (useOffscreenCopy) {
                                                offscreenCompositeRequired = true;
                                                const char* offscreenReason = "post-FSR DLSS";
                                                if (!bb) {
                                                    HookLogImportant(
                                                        "DX12: Cannot use offscreen compositing for %s overlay because "
                                                        "backbuffer is null; direct backbuffer fallback suppressed",
                                                        offscreenReason);
                                                } else {
                                                    static int s_postFSROffscreenLog = 0;
                                                    if (s_postFSROffscreenLog++ < 10) {
                                                        HookLogImportant(
                                                            "DX12: Using offscreen compositing for post-FSR non-FG "
                                                            "overlay "
                                                            "(bb=%p queue=%p bufIdx=%u #%d)",
                                                            bb, gameQueue, bufferIdx, s_postFSROffscreenLog);
                                                    }
                                                    // Two-copy compositing: avoids ALL explicit barriers on backbuffer.
                                                    // 1. Copy bb→offscreen (bb implicitly promotes COMMON→COPY_SOURCE)
                                                    // 2. Barrier offscreen COPY_DEST→RT
                                                    // 3. Render overlay on top of game frame in offscreen
                                                    // 4. Barrier offscreen RT→COPY_SOURCE
                                                    // 5. Copy offscreen→bb (bb implicitly promotes COMMON→COPY_DEST)
                                                    // After ECL, both resources decay back to COMMON.
                                                    if (EnsureOffscreenRT(dev, g_State.cachedWidth,
                                                                          g_State.cachedHeight, g_State.format)) {
                                                        // Step 1: Copy backbuffer → offscreen RT
                                                        // bb: implicit promotion COMMON→COPY_SOURCE (no explicit
                                                        // barrier!) offscreen: explicit COMMON→COPY_DEST
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = g_State.offscreenRT;
                                                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                                                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }
                                                        {
                                                            D3D12_TEXTURE_COPY_LOCATION src = {};
                                                            src.pResource = bb;
                                                            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            src.SubresourceIndex = 0;
                                                            D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                            dst.pResource = g_State.offscreenRT;
                                                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            dst.SubresourceIndex = 0;
                                                            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                        }

                                                        // Step 2: Barrier offscreen COPY_DEST → RENDER_TARGET
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = g_State.offscreenRT;
                                                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                                            b.Transition.StateAfter =
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }

                                                        // Step 3: Render overlay to offscreen RT (on top of game frame)
                                                        D3D12_CPU_DESCRIPTOR_HANDLE offRtv =
                                                            g_State.offscreenRtvHeap
                                                                ->GetCPUDescriptorHandleForHeapStart();
                                                        if (useTextureDx12Backend) {
                                                            g_D3D11On12Adapter.SetDX12RenderTarget(list,
                                                                                                   (void*)offRtv.ptr);
                                                            g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                                g_State.fence,
                                                                ce::dx12_overlay_policy::
                                                                    DecideOverlayUploadSlotGuardValue(
                                                                        slFGActive || g_FGCompat.IsFGActive(),
                                                                        g_State.fence != nullptr,
                                                                        g_State.currentFenceValue));
                                                        } else {
                                                            s_descFreeCmdList = list;
                                                            s_descFreeRtv = offRtv;
                                                            // Publish the per-slot UPLOAD-ring guard.  Non-FG path:
                                                            // this frame's overlay work is signaled on g_State.fence at
                                                            // currentFenceValue+1, so the backend can pace slot reuse
                                                            // to the GPU.  FG paths use a separate completion fence
                                                            // (g_State.fence does not advance to this value) and
                                                            // already synchronize per frame, so disable the guard there
                                                            // to avoid a stale-value wait.
                                                            s_descFreeSlotFence = g_State.fence;
                                                            s_descFreeSlotGuardValue = ce::dx12_overlay_policy::
                                                                DecideOverlayUploadSlotGuardValue(
                                                                    slFGActive || g_FGCompat.IsFGActive(),
                                                                    g_State.fence != nullptr,
                                                                    g_State.currentFenceValue);
                                                        }

                                                        g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                        const auto metricsBinding =
                                                            ce::dx12_overlay_policy::DecideOverlayMetricsBinding(
                                                                isRealFrame);
                                                        if (metricsBinding.bindMetrics) {
                                                            g_D3D11On12Adapter.SetMetrics(
                                                                DXGIShared::GetPerformanceMetrics());
                                                        }
                                                        if (metricsBinding.refreshFrameMetadata) {
                                                            const char* api = "DX12";
                                                            g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                        }
                                                        SyncSecondaryDx12OverlayColorState(g_State.format);
                                                        g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth,
                                                                                         g_State.cachedHeight);
                                                        if (!useTextureDx12Backend) {
                                                            s_descFreeCmdList = nullptr;
                                                        }

                                                        // Step 4: Barrier offscreen RT → COPY_SOURCE
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = g_State.offscreenRT;
                                                            b.Transition.StateBefore =
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }

                                                        // Step 5: Copy offscreen → backbuffer
                                                        // bb: implicit promotion COMMON→COPY_DEST (no explicit
                                                        // barrier!)
                                                        {
                                                            D3D12_TEXTURE_COPY_LOCATION src = {};
                                                            src.pResource = g_State.offscreenRT;
                                                            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            src.SubresourceIndex = 0;
                                                            D3D12_TEXTURE_COPY_LOCATION dst = {};
                                                            dst.pResource = bb;
                                                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            dst.SubresourceIndex = 0;
                                                            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                        }

                                                        // After ECL: bb decays COPY_DEST→COMMON, offscreen decays
                                                        // COPY_SOURCE→COMMON

                                                        static int s_offscreenLog = 0;
                                                        if (s_offscreenLog++ < 5) {
                                                            HookLogImportant(
                                                                "DX12: %s overlay via 2-copy compositing (bb=%p "
                                                                "offRT=%p queue=%p)",
                                                                offscreenReason, bb, g_State.offscreenRT, gameQueue);
                                                        }
                                                        usedPrimaryOverlayBackend = true;
                                                        usedDescFree = !useTextureDx12Backend;
                                                    } else {
                                                        HookLogImportant(
                                                            "DX12: Failed to create offscreen RT for %s overlay; "
                                                            "direct backbuffer fallback suppressed",
                                                            offscreenReason);
                                                    }
                                                }
                                            } else {
                                                // Normal path: render directly to backbuffer
                                                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                    g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                                UINT rtvSize = dev->GetDescriptorHandleIncrementSize(
                                                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                                rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;

                                                // ALWAYS add barriers in the primary native DX12 path.
                                                // At startup, extOverlay may be false
                                                // (socialclub.dll not loaded yet); after
                                                // FG teardown, Social Club may not render
                                                // in the menu screen.  The backbuffer is
                                                // in PRESENT state in both cases, so the
                                                // PRESENT→RT transition is correct.
                                                bool fgBarriersNeeded = true;
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                if (useTextureDx12Backend) {
                                                    g_D3D11On12Adapter.SetDX12RenderTarget(list, (void*)rtvHandle.ptr);
                                                    g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                        g_State.fence,
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            g_State.fence != nullptr, g_State.currentFenceValue));
                                                } else {
                                                    s_descFreeCmdList = list;
                                                    s_descFreeRtv = rtvHandle;
                                                    // Publish the per-slot UPLOAD-ring guard (see offscreen path
                                                    // above).
                                                    s_descFreeSlotFence = g_State.fence;
                                                    s_descFreeSlotGuardValue =
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            g_State.fence != nullptr, g_State.currentFenceValue);
                                                }

                                                g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                const auto metricsBinding =
                                                    ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
                                                if (metricsBinding.bindMetrics) {
                                                    g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
                                                }
                                                if (metricsBinding.refreshFrameMetadata) {
                                                    static const bool s_isVKD3D = []() {
                                                        return GetModuleHandleA("d3d12core.dll") &&
                                                               (GetModuleHandleA("libvkd3d-1.dll") ||
                                                                GetModuleHandleA("vkd3d.dll"));
                                                    }();
                                                    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
                                                    g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                }

                                                SyncSecondaryDx12OverlayColorState(g_State.format);
                                                g_D3D11On12Adapter.RenderOverlay(g_State.cachedWidth,
                                                                                 g_State.cachedHeight);

                                                // Transition back to PRESENT after overlay draw
                                                if (fgBarriersNeeded && bb) {
                                                    D3D12_RESOURCE_BARRIER barrier = {};
                                                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                    barrier.Transition.pResource = bb;
                                                    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                                    barrier.Transition.Subresource =
                                                        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                    list->ResourceBarrier(1, &barrier);
                                                }

                                                if (!useTextureDx12Backend) {
                                                    s_descFreeCmdList = nullptr;
                                                }
                                                usedPrimaryOverlayBackend = true;
                                                usedDescFree = !useTextureDx12Backend;
                                            }  // end normal path
