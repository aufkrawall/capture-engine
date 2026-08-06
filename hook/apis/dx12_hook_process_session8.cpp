#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

ProcessFrameFlow FrameProcessSession::DrawSubmitSetup() {
                BeginOverlayGpuBreadcrumbFrame(g_Device.load(std::memory_order_acquire));
                WriteOverlayGpuBreadcrumb(list, kOverlayBcStart);
                preserveLiveStartupOverlayDuringInactiveSL =
                    ShouldPreserveLiveStartupOverlayDuringRuntimeInactiveStreamlineHandoff();
                hasPendingStartupOverlayResources = g_OverlayAdapter.HasPendingDX12Resources();
                shouldPrimeStartupOverlayResources =
                    dx12_hook_s_startupOverlayResourcePrimeMs == 0 &&
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
                            dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
return ProcessFrameFlow::kOverlayDone;
                        }
                    }
                    HookLogImportant("DX12: Priming DX12 overlay resources before first GTA overlay draw");
                    if (!g_OverlayAdapter.PrimeDX12Resources(list)) {
                        HookLogImportant(
                            "DX12: DX12 overlay resource priming failed; deferring first overlay draw");
return ProcessFrameFlow::kOverlayDone;
                    }

                    HRESULT closeHr = list->Close();
                    if (FAILED(closeHr)) {
                        HookLog("DX12: Priming command list close failed hr=0x%08X, forcing reinit",
                                closeHr);
                        dx12_hook_g_State.syncInit = false;
return ProcessFrameFlow::kOverlayDone;
                    }

                    if (!SubmitOverlayCommandList(gameQueue, list, idx, "startup resource priming",
                                                  false)) {
                        HookLogImportant(
                            "DX12: Startup resource priming submission failed; deferring first overlay "
                            "draw");
return ProcessFrameFlow::kOverlayDone;
                    }

                    // Check device after priming submit — catch async GPU fault immediately
                    {
                        auto* postPrimeDev = g_Device.load(std::memory_order_acquire);
                        HRESULT postPrimeDevHr =
                            postPrimeDev ? postPrimeDev->GetDeviceRemovedReason() : E_FAIL;
                        if (FAILED(postPrimeDevHr)) {
                            HookLogImportant("DX12: Resource priming CAUSED device removal 0x%08X!",
                                             (unsigned)postPrimeDevHr);
                            dx12_hook_g_DeviceRemoved.store(true, std::memory_order_release);
return ProcessFrameFlow::kOverlayDone;
                        }
                    }

                    dx12_hook_s_startupOverlayResourcePrimeMs = GetTickCount64();
                    HookLogImportant(
                        "DX12: DX12 overlay resource priming submitted, delaying first overlay draw for "
                        "%llums",
                        dx12_hook_kStartupOverlayPostResourcePrimeSettleMs);
return ProcessFrameFlow::kOverlayDone;
                }

                if (shouldRunStartupOverlayDrawProbe &&
                    dx12_hook_s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kNone) {
                    // Probe system removed: go straight to rendering.
                    // The 3-stage probe (backbuffer touch → pipeline state → real draw) caused
                    // ERR_GFX_STATE in GTA5 Enhanced because even barrier-only probes on a
                    // dedicated overlay queue conflict with the game's D3D12 state tracking.
                    // With single-queue mode (fix for dedicated queue), we can render directly.
                    dx12_hook_s_startupOverlayFirstDrawProbeStage = StartupOverlayFirstDrawProbeStage::kActualRender;
                }

                sc3 = dx12_hook_g_State.cachedSC3;
                if (!sc3) {
                    if (SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
                        sc3->Release();           // drop QI ref — weak cache is safe
                        dx12_hook_g_State.cachedSC3 = sc3;  // because swapchain is alive during Present
                    }
                }
                QueryPerformanceFrequency(&perfFreq);
                QueryPerformanceCounter(&perfQI);
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSc3() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
    if (sc3) {
    flow = DrawSc3Front();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawSubmitCore();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    } else {
        flow = DrawSc3Else();
        if (flow != ProcessFrameFlow::kContinue) {
            return flow;
        }
    }
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSc3Front() {
swapchainBufferIdx = sc3->GetCurrentBackBufferIndex();
currentBackBufferIdx = swapchainBufferIdx;
hasCurrentBackBufferIdx = true;
// CRITICAL FIX: Use actual swapchain buffer index directly
// CreateRTVs now creates RTVs for all swapchain buffers (up to 8)
// so no need to wrap the index - this prevents sync issues
bufferIdx = swapchainBufferIdx;
// Validate buffer index is within our allocated range
if (bufferIdx >= (UINT)dx12_hook_g_State.bufferCount) {
    HookLog(
        "DX12: Buffer index %u exceeds allocated count %d, "
        "clamping",
        bufferIdx, dx12_hook_g_State.bufferCount);
    bufferIdx = dx12_hook_g_State.bufferCount - 1;
}
// FG-SAFE: Acquire backbuffer per-frame via GetBuffer.
// We do NOT cache backbuffer pointers because FSR FG
// monitors reference counts and crashes if extra refs
// are held persistently.
bb = nullptr;
bbNeedsRelease = false;
QueryPerformanceCounter(&perfGetBuf);
if (SUCCEEDED(sc3->GetBuffer(swapchainBufferIdx, IID_PPV_ARGS(&bb))) && bb) {
    bbNeedsRelease = true;
    // Recreate RTV for this buffer index (cheap CPU-side op).
    // Ensures RTV matches current buffer even after FSR FG
    // swapchain transitions.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvRecreate =
        dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtvRecreate.ptr += (SIZE_T)bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
    g_Device.load()->CreateRenderTargetView(bb, nullptr, rtvRecreate);
} else {
    // GetBuffer failure: the swapchain/backbuffer state is not usable.
    // Force a full RTV reinit on the next ProcessFrame instead of drawing
    // against a stale/null backbuffer. (Regression guard: this cleanup must
    // stay in the FAILURE branch only - see DrawSubmitCoreTail.)
    HookLog("DX12: GetBuffer(%u) failed, forcing RTV reinit", swapchainBufferIdx);
    CleanupRTVs();
    dx12_hook_g_State.overlayInit = false;
}
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSubmitCore() {
    ProcessFrameFlow flow = ProcessFrameFlow::kContinue;
                                if (bb) {
    flow = DrawSubmitCoreFront();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
    flow = DrawSubmitCoreTail();
    if (flow != ProcessFrameFlow::kContinue) {
        return flow;
    }
                                }
    return ProcessFrameFlow::kContinue;
}

ProcessFrameFlow FrameProcessSession::DrawSubmitCoreFront() {
                                    cmdRecordOk = false;
                                    static std::atomic<int> s_firstBackBufferLogCount{0};
                                    if (s_firstBackBufferLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                                        HookLogImportant(
                                            "DX12: Preparing overlay draw for backbuffer idx=%u resource=%p via %s "
                                            "queue (queue=%p)",
                                            bufferIdx, bb,
                                            dx12_hook_g_State.overlayQueue
                                                ? "dedicated overlay"
                                                : (gameQueue == dx12_hook_g_SwapchainQueue ? "swapchain" : "game"),
                                            gameQueue);
                                    }

                                    // SL FG diagnostic: log every overlay draw during FG
                                    if (slFGActive) {
                                        static std::atomic<int> s_slFGDrawCount{0};
                                        int fgDraw = s_slFGDrawCount.fetch_add(1, std::memory_order_relaxed) + 1;
                                        if (fgDraw <= 20 || (fgDraw % 10) == 0) {
                                            auto* diagDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT devHr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                                            bool dedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
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
                                    if (dx12_hook_g_NeedGPUDrainBeforeRender && gameQueue) {
                                        auto* drainDev = g_Device.load(std::memory_order_acquire);
                                        if (drainDev) {
                                            if (!dx12_hook_g_DrainFence) {
                                                HRESULT hr = drainDev->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                                                   IID_PPV_ARGS(&dx12_hook_g_DrainFence));
                                                if (SUCCEEDED(hr)) {
                                                    dx12_hook_g_DrainEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                                                    dx12_hook_g_DrainFenceValue = 0;
                                                    HookLogImportant("DX12: GPU drain fence created");
                                                } else {
                                                    HookLogImportant("DX12: GPU drain fence creation failed hr=0x%08X",
                                                                     (unsigned)hr);
                                                }
                                            }
                                            if (dx12_hook_g_DrainFence && dx12_hook_g_DrainEvent) {
                                                UINT64 drainVal = ++dx12_hook_g_DrainFenceValue;
                                                HRESULT sigHr = gameQueue->Signal(dx12_hook_g_DrainFence, drainVal);
                                                if (SUCCEEDED(sigHr)) {
                                                    if (dx12_hook_g_DrainFence->GetCompletedValue() < drainVal) {
                                                        dx12_hook_g_DrainFence->SetEventOnCompletion(drainVal, dx12_hook_g_DrainEvent);
                                                        DWORD waitResult = WaitForSingleObject(dx12_hook_g_DrainEvent, 5000);
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
                                        dx12_hook_g_NeedGPUDrainBeforeRender = false;
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
                                    usedPrimaryOverlayBackend = false;
                                    usedDescFree = false;
                                    offscreenCompositeRequired = false;
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
                                        if (useTextureDx12Backend && dx12_hook_g_DescFreeBackend) {
                                            ShutdownDescFreeBackend("x86 Texture2D backend selected");
                                        }
                                        if (dev && useTextureDx12Backend && !dx12_hook_g_D3D11On12Adapter.IsInitialized()) {
                                            ID3D12CommandQueue* backendQueue =
                                                gameQueue ? gameQueue : g_CommandQueue.load(std::memory_order_acquire);
                                            if (backendQueue &&
                                                dx12_hook_g_D3D11On12Adapter.InitDX12(dev, backendQueue, dx12_hook_g_State.format)) {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend ready "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)dx12_hook_g_State.format);
                                            } else {
                                                HookLogImportant(
                                                    "DX12: x86 native Texture2D overlay backend init failed "
                                                    "(device=%p queue=%p fmt=%d)",
                                                    dev, backendQueue, (int)dx12_hook_g_State.format);
                                            }
                                        } else if (dev && !useTextureDx12Backend) {
                                            // Reuses the warm device-scoped backend when device and
                                            // format still match; rebuilds it otherwise.
                                            EnsureDescFreeBackendForDeviceAndFormat(dev, dx12_hook_g_State.format,
                                                                                    "normal route");
                                        }
                                        const bool primaryOverlayReady =
                                            useTextureDx12Backend
                                                ? dx12_hook_g_D3D11On12Adapter.IsInitialized()
                                                : (dx12_hook_g_DescFreeBackend && dx12_hook_g_D3D11On12Adapter.IsInitialized());
                                        if (primaryOverlayReady) {
                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                            dx12_hook_g_D3D11On12Adapter.SetReserveInactiveFGSpace(
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
                                                dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG;
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
                                                    if (EnsureOffscreenRT(dev, dx12_hook_g_State.cachedWidth,
                                                                          dx12_hook_g_State.cachedHeight, dx12_hook_g_State.format)) {
                                                        // Step 1: Copy backbuffer → offscreen RT
                                                        // bb: implicit promotion COMMON→COPY_SOURCE (no explicit
                                                        // barrier!) offscreen: explicit COMMON→COPY_DEST
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
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
                                                            dst.pResource = dx12_hook_g_State.offscreenRT;
                                                            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                                            dst.SubresourceIndex = 0;
                                                            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                                                        }

                                                        // Step 2: Barrier offscreen COPY_DEST → RENDER_TARGET
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
                                                            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                                                            b.Transition.StateAfter =
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                            b.Transition.Subresource =
                                                                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                            list->ResourceBarrier(1, &b);
                                                        }

                                                        // Step 3: Render overlay to offscreen RT (on top of game frame)
                                                        D3D12_CPU_DESCRIPTOR_HANDLE offRtv =
                                                            dx12_hook_g_State.offscreenRtvHeap
                                                                ->GetCPUDescriptorHandleForHeapStart();
                                                        if (useTextureDx12Backend) {
                                                            dx12_hook_g_D3D11On12Adapter.SetDX12RenderTarget(list,
                                                                                                   (void*)offRtv.ptr);
                                                            dx12_hook_g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                                dx12_hook_g_State.fence,
                                                                ce::dx12_overlay_policy::
                                                                    DecideOverlayUploadSlotGuardValue(
                                                                        slFGActive || g_FGCompat.IsFGActive(),
                                                                        dx12_hook_g_State.fence != nullptr,
                                                                        dx12_hook_g_State.currentFenceValue));
                                                        } else {
                                                            dx12_hook_s_descFreeCmdList = list;
                                                            dx12_hook_s_descFreeRtv = offRtv;
                                                            // Publish the per-slot UPLOAD-ring guard.  Non-FG path:
                                                            // this frame's overlay work is signaled on g_State.fence at
                                                            // currentFenceValue+1, so the backend can pace slot reuse
                                                            // to the GPU.  FG paths use a separate completion fence
                                                            // (g_State.fence does not advance to this value) and
                                                            // already synchronize per frame, so disable the guard there
                                                            // to avoid a stale-value wait.
                                                            dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
                                                            dx12_hook_s_descFreeSlotGuardValue = ce::dx12_overlay_policy::
                                                                DecideOverlayUploadSlotGuardValue(
                                                                    slFGActive || g_FGCompat.IsFGActive(),
                                                                    dx12_hook_g_State.fence != nullptr,
                                                                    dx12_hook_g_State.currentFenceValue);
                                                        }

                                                        dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                        const auto metricsBinding =
                                                            ce::dx12_overlay_policy::DecideOverlayMetricsBinding(
                                                                isRealFrame);
                                                        if (metricsBinding.bindMetrics) {
                                                            dx12_hook_g_D3D11On12Adapter.SetMetrics(
                                                                DXGIShared::GetPerformanceMetrics());
                                                        }
                                                        if (metricsBinding.refreshFrameMetadata) {
                                                            const char* api = "DX12";
                                                            dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                        }
                                                        SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
                                                        dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth,
                                                                                         dx12_hook_g_State.cachedHeight);
                                                        if (!useTextureDx12Backend) {
                                                            dx12_hook_s_descFreeCmdList = nullptr;
                                                        }

                                                        // Step 4: Barrier offscreen RT → COPY_SOURCE
                                                        {
                                                            D3D12_RESOURCE_BARRIER b = {};
                                                            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                            b.Transition.pResource = dx12_hook_g_State.offscreenRT;
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
                                                            src.pResource = dx12_hook_g_State.offscreenRT;
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
                                                                offscreenReason, bb, dx12_hook_g_State.offscreenRT, gameQueue);
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
                                                    dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
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
                                                    dx12_hook_g_D3D11On12Adapter.SetDX12RenderTarget(list, (void*)rtvHandle.ptr);
                                                    dx12_hook_g_D3D11On12Adapter.SetDX12UploadSlotFence(
                                                        dx12_hook_g_State.fence,
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue));
                                                } else {
                                                    dx12_hook_s_descFreeCmdList = list;
                                                    dx12_hook_s_descFreeRtv = rtvHandle;
                                                    // Publish the per-slot UPLOAD-ring guard (see offscreen path
                                                    // above).
                                                    dx12_hook_s_descFreeSlotFence = dx12_hook_g_State.fence;
                                                    dx12_hook_s_descFreeSlotGuardValue =
                                                        ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                                                            slFGActive || g_FGCompat.IsFGActive(),
                                                            dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue);
                                                }

                                                dx12_hook_g_D3D11On12Adapter.SetIPCClient(g_IPC);
                                                const auto metricsBinding =
                                                    ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
                                                if (metricsBinding.bindMetrics) {
                                                    dx12_hook_g_D3D11On12Adapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
                                                }
                                                if (metricsBinding.refreshFrameMetadata) {
                                                    static const bool s_isVKD3D = []() {
                                                        return GetModuleHandleA("d3d12core.dll") &&
                                                               (GetModuleHandleA("libvkd3d-1.dll") ||
                                                                GetModuleHandleA("vkd3d.dll"));
                                                    }();
                                                    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
                                                    dx12_hook_g_D3D11On12Adapter.SetGraphicsAPI(api);
                                                }

                                                SyncSecondaryDx12OverlayColorState(dx12_hook_g_State.format);
                                                dx12_hook_g_D3D11On12Adapter.RenderOverlay(dx12_hook_g_State.cachedWidth,
                                                                                 dx12_hook_g_State.cachedHeight);

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
                                                    dx12_hook_s_descFreeCmdList = nullptr;
                                                }
                                                usedPrimaryOverlayBackend = true;
                                                usedDescFree = !useTextureDx12Backend;
                                            }  // end normal path

                                        }
                                    }

                                    // Fallback: standard DX12 rendering (uses SetDescriptorHeaps —
                                    // may cause 60% GPU on some NVIDIA configs)
                                    if (!usedPrimaryOverlayBackend) {
                                        if (offscreenCompositeRequired) {
                                            static std::atomic<int> s_offscreenRequiredNoFallbackLogCount{0};
                                            const int logCount = s_offscreenRequiredNoFallbackLogCount.fetch_add(
                                                1, std::memory_order_relaxed);
                                            if (logCount < 20 || (logCount % 300) == 0) {
                                                HookLogImportant(
                                                    "DX12: Skipping direct backbuffer fallback because offscreen "
                                                    "composite is required for this frame "
                                                    "(postFSR=%d queue=%p bufIdx=%u log=%d)",
                                                    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0, gameQueue,
                                                    bufferIdx, logCount + 1);
                                            }
                                        } else {
                                            if (!startupOverlayPresent) {
                                                D3D12_RESOURCE_BARRIER barrier = {};
                                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                barrier.Transition.pResource = bb;
                                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                barrier.Transition.Subresource =
                                                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                list->ResourceBarrier(1, &barrier);
                                            }
                                            WriteOverlayGpuBreadcrumb(list, kOverlayBcAfterRTBarrier);

                                            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                                                dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
                                            UINT rtvSize = g_Device.load()->GetDescriptorHandleIncrementSize(
                                                D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
                                            rtvHandle.ptr += (SIZE_T)bufferIdx * rtvSize;
                                            list->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                                            bool isRealFrame = g_FGCompat.IsCurrentFrameReal();
                                            DrawOverlay(list, isRealFrame, bufferIdx);
                                            WriteOverlayGpuBreadcrumb(list, kOverlayBcAfterDraw);

                                            if (!startupOverlayPresent) {
                                                D3D12_RESOURCE_BARRIER barrier = {};
                                                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                                                barrier.Transition.pResource = bb;
                                                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                                                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                                                barrier.Transition.Subresource =
                                                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                                                list->ResourceBarrier(1, &barrier);
                                            }
                                        }
                                    }

                                    overlayDrawRecorded =
                                        usedPrimaryOverlayBackend || !offscreenCompositeRequired;
                                    QueryPerformanceCounter(&perfRecord);

                                    // GPU-breadcrumb: stamp the END of CE's overlay command list. If the GPU reaches
                                    // this marker but ffxQuery still wedges, CE's GPU work is NOT the stall (look to a
                                    // fence/CPU deadlock or AMD's own work); if it stops earlier, CE's list is the
                                    // stall.
                                    WriteOverlayGpuBreadcrumb(list, kOverlayBcBeforeClose);

                                    closeHr = list->Close();
                                    // Log Close result during FG
                                    if (g_FGCompat.IsFGActive() || slFGActive) {
                                        static std::atomic<int> s_fgCloseLogs{0};
                                        if (s_fgCloseLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                                            HookLogImportant("DX12: FG overlay list->Close hr=0x%08X",
                                                             (unsigned)closeHr);
                                        }
                                    }
                                    // Always log Close result for first N reinit frames
                                    {
                                        static int s_reinitCloseLogCount = 0;
                                        if (dx12_hook_g_ResetReinitSubmitCounter.load(std::memory_order_relaxed))
                                            s_reinitCloseLogCount = 0;
                                        if (s_reinitCloseLogCount < 5) {
                                            s_reinitCloseLogCount++;
                                            auto* closeDev = g_Device.load(std::memory_order_acquire);
                                            HRESULT closeDevHr = closeDev ? closeDev->GetDeviceRemovedReason() : E_FAIL;
                                            HookLogImportant(
                                                "DX12: Reinit Close #%d hr=0x%08X devRemoved=0x%08X primaryOverlay=%d",
                                                s_reinitCloseLogCount, (unsigned)closeHr, (unsigned)closeDevHr,
                                                usedPrimaryOverlayBackend ? 1 : 0);
                                        }
                                    }
    return ProcessFrameFlow::kContinue;
}
