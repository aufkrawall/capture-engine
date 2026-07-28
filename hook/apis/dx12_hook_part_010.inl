        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);

    const D3D12_RESOURCE_DESC td = uiTexture->GetDesc();
    if (td.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.Width == 0 || td.Height == 0) {
        return false;
    }
    // DEGENERATE-TARGET GUARD: never draw the overlay onto a placeholder UI texture (GTA registers a 1x1).
    // DX12_PrepareFFXUiOverlayTarget normally substitutes CE's backbuffer-sized texture for a degenerate
    // game UI texture, but if that substitution was unavailable (no backbuffer geometry yet) the cached
    // target is still the 1x1 placeholder. Drawing onto a 1x1 RT is useless and trips CreateRenderTargetView/
    // Reset E_INVALIDARG (session 20260621_191028), so skip — the overlay stays blank that frame but never
    // crashes. (Same degenerate test ChooseFFXUiOverlayTarget / the old bundle used.)
    {
        const uint32_t bbW = g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
        const uint32_t bbH = g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);
        const bool degenerate =
            td.Width <= 1 || td.Height <= 1 ||
            (bbW > 0 && bbH > 0 &&
             (static_cast<uint32_t>(td.Width) * 2u < bbW || static_cast<uint32_t>(td.Height) * 2u < bbH));
        if (degenerate) {
            static std::atomic<int> s_degenerateSkipLog{0};
            const int n = s_degenerateSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 600) == 0) {
                HookLogImportant(
                    "DX12: FFX UI-composite SKIPPED degenerate target %p (%llux%u bb=%ux%u) — no substitute "
                    "available yet; overlay blank-but-safe this frame (log=%d)",
                    (void*)uiTexture, static_cast<unsigned long long>(td.Width), td.Height, bbW, bbH, n + 1);
            }
            return false;
        }
    }
    const int width = static_cast<int>(td.Width);
    const int height = static_cast<int>(td.Height);
    const DXGI_FORMAT rtvFormat = FFXUiCompositeRtvFormat(td.Format);
    // CE's substitute texture is CE-owned and otherwise empty, so it must be cleared to transparent each
    // frame (only the overlay composites over the game frame). The game's own usable UI texture is never
    // cleared (the overlay blends on top of the HUD already present there). Mirrors the retired bundle.
    const bool needsTransparentClear = g_BundleTargetNeedsTransparentClear.load(std::memory_order_acquire);

    if (g_FFXUiCompositeDevice == nullptr) {
        // FIRST-TIME init on this device: just record it. This is NOT a device change, so it must NOT call
        // ReleaseFFXUiCompositeInfra — that clears g_CachedFFXUiTexture, which Hooked_ffxConfigure just
        // populated and (after the one-shot ffxConfigure VEH disarms) may never repopulate. Session
        // 20260624_001619: the spurious first-call teardown nulled the cache, so the wrapper saw a null cache
        // on every subsequent present and the overlay composited exactly ONE frame then went blank.
        g_FFXUiCompositeDevice = device;
    } else if (g_FFXUiCompositeDevice != device) {
        // GENUINE device change: the infra + cache + substitute all belong to the now-dead device. Tear them
        // down and bail this frame — the passed uiTexture is from the old device. The next present rebuilds once
        // a UI texture is re-registered on the new device (or stays blank-but-safe if the VEH already disarmed).
        ReleaseFFXUiCompositeInfra();
        if (g_FFXPresentOverlayAdapter.IsInitialized()) {
            g_FFXPresentOverlayAdapter.Shutdown();
        }
        g_FFXPresentOverlayDevice = nullptr;
        g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeDevice = device;
        return false;
    }

    // Lazily build the dedicated CE-queue submission infra (separate from the present-thread overlay and
    // separate from g_State.overlayQueue whose lifecycle is tied to FG state transitions).
    if (!g_FFXUiCompositeFence) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_FFXUiCompositeFence)))) {
            return false;
        }
        if (!g_FFXUiCompositeFenceEvent) {
            g_FFXUiCompositeFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (!g_FFXUiCompositeFenceEvent) {
            HookLogImportant("DX12: FFX UI-composite failed to create completion event gle=%lu", GetLastError());
            return false;
        }
    }
    // Step 2 revised: create CE's own dedicated DIRECT queue for the UI-composite submit. AMD does NOT track
    // this queue, so submitting here + signaling the fence here does not perturb AMD's pacing. The UI texture
    // is a game-owned committed resource (not a swapchain backbuffer), so cross-queue writes are legal.
    if (!g_FFXUiCompositeQueue) {
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (gameQueue) {
            qDesc.NodeMask = gameQueue->GetDesc().NodeMask;
        }
        if (FAILED(device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&g_FFXUiCompositeQueue)))) {
            HookLogImportant(
                "DX12: FFX UI-composite FAILED to create dedicated CE queue — refusing the known-wedging "
                "game/runtime-queue fallback");
            g_FFXUiCompositeQueue = nullptr;
            return false;
        } else {
            g_FFXUiCompositeQueue->SetName(L"CE_FFXUiCompositeQueue");
            HookLogImportant("DX12: FFX UI-composite dedicated CE queue created (ptr=%p gameQueue=%p)",
                             g_FFXUiCompositeQueue, gameQueue);
        }
    }
    for (auto& a : g_FFXUiCompositeAlloc) {
        if (!a && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) {
            return false;
        }
    }
    if (!g_FFXUiCompositeList) {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FFXUiCompositeAlloc[0], nullptr,
                                             IID_PPV_ARGS(&g_FFXUiCompositeList)))) {
            return false;
        }
        g_FFXUiCompositeList->Close();
    }
    if (!g_FFXUiCompositeRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
                                                  0};
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_FFXUiCompositeRtvHeap)))) {
            return false;
        }
    }

    // Reuse the dedicated FFX overlay adapter (the callback path is mutually exclusive with no-callback FSR).
    {
        std::lock_guard<std::recursive_mutex> olock(g_OverlayMutex);
        if (!g_FFXPresentOverlayAdapter.IsInitialized() || g_FFXUiCompositeAdapterFormat != rtvFormat) {
            if (g_FFXPresentOverlayAdapter.IsInitialized()) {
                g_FFXPresentOverlayAdapter.Shutdown();
            }
            g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
            if (!g_FFXPresentOverlayAdapter.InitDX12(device, gameQueue, static_cast<int>(rtvFormat))) {
                HookLogImportant("DX12: FFX UI-composite failed to init overlay adapter (fmt=%d)",
                                 static_cast<int>(rtvFormat));
                return false;
            }
            const bool uiTargetHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(rtvFormat);
            g_FFXPresentOverlayAdapter.SetHDR(uiTargetHDR, static_cast<int>(rtvFormat));
            g_FFXPresentOverlayDevice = device;
            g_FFXPresentOverlayFormat = rtvFormat;
            g_FFXUiCompositeAdapterFormat = rtvFormat;
            HookLogImportant("DX12: FFX UI-composite initialized overlay adapter (fmt=%d hdr=%d %dx%d)",
                             static_cast<int>(rtvFormat), uiTargetHDR ? 1 : 0, width, height);
        }
    }

    // Step 2 revised: submit on CE's OWN dedicated queue (g_FFXUiCompositeQueue), NOT the game queue.
    // AMD does not track CE's queue, so the ECL + fence Signal here do not perturb AMD's pacing.
    // The fence IS signaled (on CE's own queue) and we CPU-wait for completion before returning, so the
    // caller forwards RegisterUiResource only after CE's overlay write is GPU-complete — no write/read race
    // with AMD's UI-texture snapshot. The 3-slot rotation is recycled by fence value as before.
    ID3D12CommandQueue* submitQueue = g_FFXUiCompositeQueue;
    if (!submitQueue) {
        return false;
    }
    const int slot = g_FFXUiCompositeFrame % kFFXUiCompositeSlotCount;
    // Recycle the allocator slot by fence value (signaled on CE's own queue). Fast path: already complete.
    if (g_FFXUiCompositeFence && g_FFXUiCompositeFence->GetCompletedValue() < g_FFXUiCompositeAllocFenceVal[slot]) {
        HookLogImportant(
            "DX12: FFX UI-composite refused in-flight allocator reuse (slot=%d guard=%llu completed=%llu) — "
            "prior completion proof is missing; no wait/overwrite",
            slot, static_cast<unsigned long long>(g_FFXUiCompositeAllocFenceVal[slot]),
            static_cast<unsigned long long>(g_FFXUiCompositeFence->GetCompletedValue()));
        return false;
    }
    const HRESULT allocResetHr = g_FFXUiCompositeAlloc[slot]->Reset();
    const HRESULT listResetHr =
        SUCCEEDED(allocResetHr) ? g_FFXUiCompositeList->Reset(g_FFXUiCompositeAlloc[slot], nullptr) : E_FAIL;
    if (FAILED(allocResetHr) || FAILED(listResetHr)) {
        static std::atomic<int> s_resetFailLog{0};
        const int n = s_resetFailLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 600) == 0) {
            HookLogImportant(
                "DX12: FFX UI-composite allocator/list Reset FAILED (slot=%d frame=%d allocHr=0x%08X listHr=0x%08X) — "
                "recreating command list to recover",
                slot, g_FFXUiCompositeFrame, static_cast<unsigned>(allocResetHr), static_cast<unsigned>(listResetHr));
        }
        // RECOVERY: a failed list->Reset (E_INVALIDARG) means the list is stuck OPEN from a prior failed Close.
        // Release it so the lazy-init above recreates a clean closed list next frame — otherwise every subsequent
        // frame fails the same Reset and the overlay blanks permanently.
        if (FAILED(listResetHr) && g_FFXUiCompositeList) {
            g_FFXUiCompositeList->Release();
            g_FFXUiCompositeList = nullptr;
        }
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_FFXUiCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = rtvFormat;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(uiTexture, &rtvDesc, rtv);

    const D3D12_RESOURCE_STATES regState = GetDX12StateFromFFXResourceState(ffxState);
    // GPU-breadcrumb CE's UI-texture write so a freeze dump distinguishes "CE's game-queue write completed
    // (wedge is AMD's pacing/read of the shared UI texture)" from "CE's write hung on the GPU (cross-queue
    // resource-state hazard against AMD's own use of the texture)".
    BeginOverlayGpuBreadcrumbFrame(device);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcStart);
    TransitionResourceIfNeeded(g_FFXUiCompositeList, uiTexture, regState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcAfterRTBarrier);
    if (needsTransparentClear) {
        // CE's substitute texture (degenerate game UI texture) — clear to transparent so ONLY the overlay
        // composites over the game frame (the game's real content shows through AMD's UI composition). The
        // game's own usable UI texture is never cleared (the overlay blends on top of the HUD already there).
        const float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_FFXUiCompositeList->ClearRenderTargetView(rtv, kTransparent, 0, nullptr);
    }
    {
        std::lock_guard<std::recursive_mutex> olock(g_OverlayMutex);
        g_FFXPresentOverlayAdapter.SetIPCClient(g_IPC);
        g_FFXPresentOverlayAdapter.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            g_FFXPresentOverlayAdapter.SetMetrics(perf);
        }
        g_FFXPresentOverlayAdapter.SetGraphicsAPI("DX12");
        g_FFXPresentOverlayAdapter.SetDX12RenderTarget(g_FFXUiCompositeList, reinterpret_cast<void*>(rtv.ptr));
        g_FFXPresentOverlayAdapter.RenderOverlay(width, height);
    }
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcAfterDraw);
    TransitionResourceIfNeeded(g_FFXUiCompositeList, uiTexture, D3D12_RESOURCE_STATE_RENDER_TARGET, regState);
    WriteOverlayGpuBreadcrumb(g_FFXUiCompositeList, kOverlayBcBeforeClose);
    const HRESULT closeHr = g_FFXUiCompositeList->Close();
    if (FAILED(closeHr)) {
        // A Close failure (commonly E_INVALIDARG from an invalid recording — e.g. a barrier whose StateBefore
        // does not match the UI texture's real state) leaves the list OPEN. Log it and release the list so the
        // next frame rebuilds a clean one — otherwise the next list->Reset fails forever and the overlay blanks
        // permanently. A persistent failure points at the registered UI-resource state vs the texture's real state.
        static std::atomic<int> s_compositeCloseFailLog{0};
        const int n = s_compositeCloseFailLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 600) == 0) {
            HookLogImportant(
                "DX12: FFX UI-composite command list Close FAILED hr=0x%08X (uiTex=%p ffxState=0x%X regState=0x%X "
                "needsClear=%d) — recreating list to recover",
                static_cast<unsigned>(closeHr), (void*)uiTexture, ffxState, static_cast<unsigned>(regState),
                needsTransparentClear ? 1 : 0);
        }
        if (g_FFXUiCompositeList) {
            g_FFXUiCompositeList->Release();
            g_FFXUiCompositeList = nullptr;
        }
        return false;
    }

    // QPC stamp at ECL submit (for the timeline ring buffer — CPU-side submit→return causality).
    LARGE_INTEGER submitQpc;
    QueryPerformanceCounter(&submitQpc);
    ID3D12CommandList* lists[] = {g_FFXUiCompositeList};
    ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
    {
        ScopedCEOverlayECLSubmission ceOverlayECLGuard("ffx-ui-composite");
        if (realECL) {
            realECL(submitQueue, 1, lists);
        } else {
            submitQueue->ExecuteCommandLists(1, lists);
        }
    }
    // Step 2 revised: signal the fence on CE's OWN queue (not the game queue). AMD does not track this
    // queue, so this Signal does not perturb AMD's pacing.
    ++g_FFXUiCompositeFenceVal;
    const HRESULT signalHr = submitQueue->Signal(g_FFXUiCompositeFence, g_FFXUiCompositeFenceVal);
    if (FAILED(signalHr)) {
        g_FFXUiCompositeAllocFenceVal[slot] = UINT64_MAX;
        HookLogImportant(
            "DX12: FFX UI-composite completion Signal FAILED (queue=%p frame=%d value=%llu hr=0x%08X) — "
            "overlay not published and allocator permanently quarantined",
            submitQueue, g_FFXUiCompositeFrame, static_cast<unsigned long long>(g_FFXUiCompositeFenceVal),
            static_cast<unsigned>(signalHr));
        return false;
    }
    g_FFXUiCompositeAllocFenceVal[slot] = g_FFXUiCompositeFenceVal;
    ++g_FFXUiCompositeFrame;
    // Step 2 revised: CPU-wait for CE's overlay write to complete on CE's own queue before returning, so
    // the caller (Hooked_ffxConfigure) forwards RegisterUiResource only after the overlay is GPU-complete.
    // This is a deterministic fence completion wait (not a sleep/poll), on the ffxConfigure thread (which is
    // already blocked calling ffxConfigure), not the game's ECL thread. Wait duration = overlay draw time (<1ms).
    uint32_t waitTimedOut = 0;
    if (g_FFXUiCompositeFenceEvent && g_FFXUiCompositeFence &&
        g_FFXUiCompositeFence->GetCompletedValue() < g_FFXUiCompositeFenceVal) {
        const HRESULT eventHr =
            g_FFXUiCompositeFence->SetEventOnCompletion(g_FFXUiCompositeFenceVal, g_FFXUiCompositeFenceEvent);
        const DWORD waitResult =
            SUCCEEDED(eventHr) ? WaitForSingleObject(g_FFXUiCompositeFenceEvent, INFINITE) : WAIT_FAILED;
        if (waitResult != WAIT_OBJECT_0) {
            waitTimedOut = 1;
            HookLogImportant(
                "DX12: FFX UI-composite completion proof FAILED (frame=%d fenceVal=%llu completed=%llu "
                "setEventHr=0x%08X wait=%lu) — overlay not published/re-registered",
                g_FFXUiCompositeFrame, static_cast<unsigned long long>(g_FFXUiCompositeFenceVal),
                static_cast<unsigned long long>(g_FFXUiCompositeFence->GetCompletedValue()),
                static_cast<unsigned>(eventHr), waitResult);
            return false;
        }
    }
    LARGE_INTEGER returnQpc;
    QueryPerformanceCounter(&returnQpc);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);

    // Record this composite call in the timeline ring buffer for freeze diagnosis.
    const int gameEclCount = g_CommandListsExecutedThisFrame.load(std::memory_order_relaxed);
    RecordFFXUiCompositeTimelineEntry(
        {static_cast<uint64_t>(g_FFXUiCompositeFrame), g_FFXUiCompositeFenceVal,
         g_FFXUiCompositeFence ? g_FFXUiCompositeFence->GetCompletedValue() : 0,
         static_cast<uint64_t>(submitQpc.QuadPart), static_cast<uint64_t>(returnQpc.QuadPart), waitTimedOut,
         static_cast<uint32_t>(slot), static_cast<uint32_t>(gameEclCount), uiTexture, ffxState, submitQueue});

    static std::atomic<int> s_uiCompositeLogCount{0};
    const int logCount = s_uiCompositeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        // Heartbeat proving the composite ran AND the CE-queue fence completed (a GTA run shows this with
        // waitTimedOut=0). fenceCompleted >= fenceVal == fence Signal observed by the CPU before Present.
        HookLogImportant(
            "DX12: FFX UI-composite overlay drawn via CE queue %p onto FFX UI resource %p (frame=%d "
            "fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u clear=%d regState=0x%X ffxState=0x%X flags=0x%X "
            "%dx%d fmt=%d slot=%d gameEcl=%d log=%d) — self-signaled, CPU-waited before Present",
            submitQueue, uiTexture, g_FFXUiCompositeFrame, static_cast<unsigned long long>(g_FFXUiCompositeFenceVal),
            static_cast<unsigned long long>(g_FFXUiCompositeFence ? g_FFXUiCompositeFence->GetCompletedValue() : 0),
            waitTimedOut, needsTransparentClear ? 1 : 0, static_cast<unsigned>(regState), ffxState, flags, width,
            height, static_cast<int>(rtvFormat), slot, gameEclCount, logCount + 1);
    }
    return true;
}

// Drive the FFX UI-resource composite from DetourPresent's no-callback FSR FG present path using the cached
// target texture (CE's substitute, or the game's usable UI texture, set by DX12_PrepareFFXUiOverlayTarget).
// Holds g_FFXUiCompositeMutex across the cached-pointer load AND the composite so a concurrent
// substitute-generation replacement on the ffxConfigure thread can never release the texture out from under
// the composite (the cached pointer is otherwise swapped without the lock). Returns true if composited.
bool DX12_CompositeOverlayOntoCachedFFXUiResource() {
    bool composited = false;
    bool alreadyCovered = false;
    uint64_t coveredSequence = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);
        ID3D12Resource* uiTexture = g_CachedFFXUiTexture.load(std::memory_order_acquire);
        if (uiTexture) {
            const uint64_t targetSequence = g_FFXUiCommittedPreparationSequence;
            alreadyCovered = !ce::dx12_overlay_policy::ShouldCompositeFFXPresenterFallback(
                targetSequence, g_FFXUiPresenterFallbackLastSequence);
            coveredSequence = targetSequence;
            if (!alreadyCovered) {
                const uint32_t ffxState = g_CachedFFXUiState.load(std::memory_order_acquire);
                const uint32_t flags = g_CachedFFXUiFlags.load(std::memory_order_acquire);
                composited = DX12_CompositeOverlayOntoFFXUiResource(uiTexture, ffxState, flags);
                if (composited) {
                    g_FFXUiPresenterFallbackLastSequence = targetSequence;
                }
            }
        }
    }
    // NOTE: the substitute UI-resource re-assert is deliberately NOT called here anymore. This function is
    // reachable from DetourPresent on AMD's PRESENTER thread (the real-swapchain fallback driver), and the
    // re-assert's ffxConfigure(RegisterUiResource) takes AMD's FrameInterpolationSwapchain criticalSection —
    // which AMD's Present holds on the GAME thread while spin-waiting (no timeout) on compositionFenceCPU
    // that only the presenter thread can advance. Calling it from here deadlocked GTA permanently on the
    // first FSR-FG frame (session 20260701_213656 freeze dump: presenter thread blocked in
    // RtlEnterCriticalSection under CE's DetourPresent, game thread spinning in amd_fidelityfx ffxQuery).
    // The re-assert now runs ONLY from the FFX proxy-present prework (game thread, before AMD's Present).
    if (alreadyCovered) {
        static std::atomic<int> s_presenterDuplicateSkipLogCount{0};
        const int logCount = s_presenterDuplicateSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX presenter fallback skipped duplicate composite for accepted UI registration "
                "sequence=%llu (log=%d) — real/generated outputs share one post-interpolation UI input",
                static_cast<unsigned long long>(coveredSequence), logCount + 1);
        }
    }
    if (composited || alreadyCovered) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }
    return composited || alreadyCovered;
}

// The FFX game/presentation queue is normally captured from the DX12 FrameGenerationSwapChain creation
// descriptor. If that call was already in flight when interception became live, the retained pre-FSR original
// game queue is the recoverable equivalent; the nested DXGI create queue is FFX's internal presentQueue and is
// never bound here. A proven Streamline wrapper also retains CE's real original game queue so target-device
// validation can select its underlying submission path. Bindings are keyed by raw proxy identity without
// retaining the proxy itself: AddRef'ing a startup/takeover swapchain pins its HWND and can make replacement
// creation fail.
struct NativeFSRSwapchainQueueBinding {
    void* context = nullptr;
    ID3D12CommandQueue* descriptorQueue = nullptr;
    ID3D12CommandQueue* underlyingGameQueue = nullptr;
    bool descriptorQueueUsesAcceptedStreamlineDevice = false;
    bool recoveredOriginalGameQueue = false;
};

static std::mutex g_NativeFSRSwapchainQueueBindingMutex;
static std::unordered_map<void*, NativeFSRSwapchainQueueBinding> g_NativeFSRSwapchainQueueBindings;

static bool RegisterNativeFSRSwapchainPresentationQueue(void* context, void* swapChain,
                                                        ID3D12CommandQueue* presentationQueue, bool onlyWhenMissing,
                                                        bool recoveredOriginalGameQueue,
                                                        bool hasProtectedInnerPresentQueue, const char* source) {
    if (!context || !swapChain || !presentationQueue ||
        presentationQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return false;
    }

    ID3D12Device* descriptorDevice = nullptr;
    presentationQueue->GetDevice(IID_PPV_ARGS(&descriptorDevice));
    const bool streamlineWrappedQueue = descriptorDevice && StreamlineHook::IsAcceptedD3D12Device(descriptorDevice);
    ID3D12CommandQueue* underlyingGameQueue =
        streamlineWrappedQueue ? DX12_AcquireOriginalGameQueueForOverlay() : nullptr;

    presentationQueue->AddRef();
    NativeFSRSwapchainQueueBinding replacedBinding = {};
    {
        std::lock_guard<std::mutex> lock(g_NativeFSRSwapchainQueueBindingMutex);
        const auto existing = g_NativeFSRSwapchainQueueBindings.find(swapChain);
        if (onlyWhenMissing && !ce::dx12_overlay_policy::ShouldRecoverNativeFSRProxyBindingFromProtectedCreate(
                                   existing != g_NativeFSRSwapchainQueueBindings.end(), context != nullptr,
                                   swapChain != nullptr, hasProtectedInnerPresentQueue, presentationQueue != nullptr)) {
            presentationQueue->Release();
            if (underlyingGameQueue) {
                underlyingGameQueue->Release();
            }
            if (descriptorDevice) {
                descriptorDevice->Release();
            }
            return false;
        }
        auto& binding = g_NativeFSRSwapchainQueueBindings[swapChain];
        if (binding.context == context && binding.descriptorQueue == presentationQueue &&
            binding.underlyingGameQueue == underlyingGameQueue &&
            binding.descriptorQueueUsesAcceptedStreamlineDevice == streamlineWrappedQueue &&
            binding.recoveredOriginalGameQueue == recoveredOriginalGameQueue) {
            presentationQueue->Release();
            if (underlyingGameQueue) {
                underlyingGameQueue->Release();
            }
            if (descriptorDevice) {
                descriptorDevice->Release();
            }
            return false;
        }
        replacedBinding = binding;
        binding = {context, presentationQueue, underlyingGameQueue, streamlineWrappedQueue, recoveredOriginalGameQueue};
    }
    if (replacedBinding.descriptorQueue || replacedBinding.underlyingGameQueue) {
        ce::dx12_ffx_suspend_overlay::RetireProxy(swapChain, "FFX proxy queue binding replaced");
        if (replacedBinding.descriptorQueue) {
            replacedBinding.descriptorQueue->Release();
        }
        if (replacedBinding.underlyingGameQueue) {
            replacedBinding.underlyingGameQueue->Release();
        }
    }

    ID3D12Device* underlyingDevice = nullptr;
    if (underlyingGameQueue) {
        underlyingGameQueue->GetDevice(IID_PPV_ARGS(&underlyingDevice));
    }
    HookLogImportant(
        "DX12: Captured native-FSR swapchain presentation queue (context=%p proxy=%p descriptorQueue=%p "
        "descriptorDevice=%p streamlineWrapped=%d recoveredOriginal=%d underlyingGameQueue=%p "
        "underlyingDevice=%p nodeMask=%u) — source=%s; direct overlay work uses the exact descriptor/recovered "
        "owner queue, or the validated underlying game queue for a proven Streamline wrapper",
        context, swapChain, presentationQueue, descriptorDevice, streamlineWrappedQueue ? 1 : 0,
        recoveredOriginalGameQueue ? 1 : 0, underlyingGameQueue, underlyingDevice,
        presentationQueue->GetDesc().NodeMask, source && source[0] ? source : "unknown");
    if (underlyingDevice) {
        underlyingDevice->Release();
    }
    if (descriptorDevice) {
        descriptorDevice->Release();
    }
    return true;
}

void DX12_RegisterNativeFSRSwapchainPresentationQueue(void* context, void* swapChain,
                                                      ID3D12CommandQueue* presentationQueue) {
    RegisterNativeFSRSwapchainPresentationQueue(context, swapChain, presentationQueue, false, false, false,
                                                "ffxCreateContext descriptor");
}

bool DX12_TryRecoverNativeFSRSwapchainPresentationQueue(void* context, void* swapChain) {
    ID3D12CommandQueue* protectedInnerPresentQueue = ReferenceDeferredOfficialFFXTakeoverQueue();
    if (!protectedInnerPresentQueue) {
        return false;
    }

    // FidelityFX creates a fresh high-priority presentQueue and passes THAT queue to its nested
    // CreateSwapChainForHwnd. The creation descriptor's input gameQueue is retained separately and owns the
    // replacement backbuffers plus UI snapshot copy. When the descriptor call itself was missed, the only safe
    // recoverable equivalent is CE's pre-FSR original game/producer queue; using the protected inner queue here
    // races the game UI producer and perturbs AMD's presenter.
    ID3D12CommandQueue* originalGameQueue = DX12_AcquireOriginalGameQueueForOverlay();
    const bool recovered = RegisterNativeFSRSwapchainPresentationQueue(
        context, swapChain, originalGameQueue, true, true, protectedInnerPresentQueue != nullptr,
        "pre-FSR original game queue (protected inner FFX create evidence)");
    if (recovered) {
        HookLogImportant(
            "DX12: Recovered native-FSR proxy owner-queue binding from pre-FSR original game queue "
            "(context=%p proxy=%p ownerQueue=%p protectedInnerPresentQueue=%p) — the nested DXGI queue is "
            "FFX's internal presenter and is evidence only, never CE's overlay submission queue",
            context, swapChain, originalGameQueue, protectedInnerPresentQueue);
    } else if (!originalGameQueue) {
        static std::atomic<int> s_missingOriginalQueueLogCount{0};
        const int logCount = s_missingOriginalQueueLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Native-FSR proxy owner recovery refused because the protected inner FFX present queue "
                "%p has no retained pre-FSR original game queue (context=%p proxy=%p log=%d)",
                protectedInnerPresentQueue, context, swapChain, logCount + 1);
        }
    }
    if (originalGameQueue) {
        originalGameQueue->Release();
    }
    protectedInnerPresentQueue->Release();
    return recovered;
}

void DX12_UnregisterNativeFSRSwapchainPresentationQueue(void* context, const char* reason) {
    struct ReleasedBinding {
        void* proxy = nullptr;
        NativeFSRSwapchainQueueBinding binding = {};
    };
    std::vector<ReleasedBinding> releasedBindings;
    {
        std::lock_guard<std::mutex> lock(g_NativeFSRSwapchainQueueBindingMutex);
        for (auto it = g_NativeFSRSwapchainQueueBindings.begin(); it != g_NativeFSRSwapchainQueueBindings.end();) {
            if (!context || it->second.context == context) {
                if (it->second.descriptorQueue || it->second.underlyingGameQueue) {
                    releasedBindings.push_back({it->first, it->second});
                }
                it = g_NativeFSRSwapchainQueueBindings.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const ReleasedBinding& binding : releasedBindings) {
        ce::dx12_ffx_suspend_overlay::RetireProxy(binding.proxy, reason);
        if (binding.binding.descriptorQueue) {
            binding.binding.descriptorQueue->Release();
        }
        if (binding.binding.underlyingGameQueue) {
            binding.binding.underlyingGameQueue->Release();
        }
    }
    if (!releasedBindings.empty()) {
        HookLogImportant("DX12: Released %zu native-FSR proxy queue binding(s) (%s)", releasedBindings.size(),
                         reason ? reason : "unregistered");
    }
}

static bool QueueDeviceOwnsResource(ID3D12CommandQueue* queue, ID3D12Resource* target, ID3D12Device** queueDeviceOut) {
    if (queueDeviceOut) {
        *queueDeviceOut = nullptr;
    }
    if (!queue || !target) {
        return false;
    }
    ID3D12Device* queueDevice = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !queueDevice) {
        return false;
    }
    const bool matches = IsResourceOwnedByDevice(target, queueDevice);
    if (queueDeviceOut) {
        *queueDeviceOut = queueDevice;
    } else {
        queueDevice->Release();
    }
    return matches;
}

struct AcquiredNativeFSROwnerQueue {
    ID3D12CommandQueue* queue = nullptr;
    ce::dx12_overlay_policy::NativeFSROwnerQueueRoute route =
        ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kUnavailable;
};

static AcquiredNativeFSROwnerQueue AcquireNativeFSRSwapchainPresentationQueue(IDXGISwapChain* proxy,
                                                                              ID3D12Resource* target) {
    if (!proxy || !target) {
        return {};
    }

    NativeFSRSwapchainQueueBinding binding = {};
    {
        std::lock_guard<std::mutex> lock(g_NativeFSRSwapchainQueueBindingMutex);
        const auto it = g_NativeFSRSwapchainQueueBindings.find(proxy);
        if (it == g_NativeFSRSwapchainQueueBindings.end()) {
            return {};
        }
        binding = it->second;
        if (binding.descriptorQueue) {
            binding.descriptorQueue->AddRef();
        }
        if (binding.underlyingGameQueue) {
            binding.underlyingGameQueue->AddRef();
        }
    }
    if (binding.descriptorQueueUsesAcceptedStreamlineDevice && !binding.underlyingGameQueue) {
        // Some integrations create the FFX context before the first real Present establishes CE's original
        // queue. Resolve it lazily once available; the target-device check below still has final authority.
        binding.underlyingGameQueue = DX12_AcquireOriginalGameQueueForOverlay();
    }

    ID3D12Device* descriptorDevice = nullptr;
    ID3D12Device* underlyingDevice = nullptr;
    const bool exactMatches = QueueDeviceOwnsResource(binding.descriptorQueue, target, &descriptorDevice);
    const bool underlyingMatches = QueueDeviceOwnsResource(binding.underlyingGameQueue, target, &underlyingDevice);
    const auto route = ce::dx12_overlay_policy::ChooseNativeFSROwnerQueueRoute(
        exactMatches, binding.descriptorQueueUsesAcceptedStreamlineDevice, underlyingMatches);

    ID3D12CommandQueue* const descriptorQueueForLog = binding.descriptorQueue;
    ID3D12CommandQueue* const underlyingQueueForLog = binding.underlyingGameQueue;
    ID3D12CommandQueue* selectedQueue = nullptr;
    if (route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kExactDescriptorQueue) {
        selectedQueue = binding.descriptorQueue;
        binding.descriptorQueue = nullptr;
    } else if (route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue) {
        selectedQueue = binding.underlyingGameQueue;
        binding.underlyingGameQueue = nullptr;
    }

    if (binding.descriptorQueue) {
        binding.descriptorQueue->Release();
    }
    if (binding.underlyingGameQueue) {
        binding.underlyingGameQueue->Release();
    }

    static std::atomic<int> s_lastLoggedRoute{-1};
    static std::atomic<int> s_unavailableLogCount{0};
    const int routeValue = static_cast<int>(route);
    const int previousRoute = s_lastLoggedRoute.exchange(routeValue, std::memory_order_relaxed);
    const int unavailableLog = route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kUnavailable
                                   ? s_unavailableLogCount.fetch_add(1, std::memory_order_relaxed)
                                   : 0;
    if (previousRoute != routeValue || (route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kUnavailable &&
                                        (unavailableLog < 20 || (unavailableLog % 300) == 0))) {
        const char* routeName =
            route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kExactDescriptorQueue
                ? (binding.recoveredOriginalGameQueue ? "recovered-original-game" : "exact-descriptor")
                : (route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue
                       ? "streamline-underlying-game"
                       : "unavailable");
        HookLogImportant(
            "DX12: Native-FSR owner queue route=%s proxy=%p target=%p descriptorQueue=%p descriptorDevice=%p "
            "exactMatches=%d streamlineWrapped=%d underlyingQueue=%p underlyingDevice=%p underlyingMatches=%d "
            "recoveredOriginal=%d selected=%p",
            routeName, proxy, target, descriptorQueueForLog, descriptorDevice, exactMatches ? 1 : 0,
            binding.descriptorQueueUsesAcceptedStreamlineDevice ? 1 : 0, underlyingQueueForLog, underlyingDevice,
            underlyingMatches ? 1 : 0, binding.recoveredOriginalGameQueue ? 1 : 0, selectedQueue);
