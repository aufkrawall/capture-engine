    }
    if (state & ce::ffx_api::kResourceStateComputeRead) {
        dx12State |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (state & ce::ffx_api::kResourceStatePixelRead) {
        dx12State |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (state & ce::ffx_api::kResourceStateCopySrc) {
        dx12State |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (state & ce::ffx_api::kResourceStateCopyDest) {
        dx12State |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (state & ce::ffx_api::kResourceStateIndirectArgument) {
        dx12State |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    if (state & ce::ffx_api::kResourceStatePresent) {
        dx12State |= D3D12_RESOURCE_STATE_PRESENT;
    }
    if (state & ce::ffx_api::kResourceStateRenderTarget) {
        dx12State |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (state & ce::ffx_api::kResourceStateDepthAttachment) {
        dx12State |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    return dx12State == static_cast<D3D12_RESOURCE_STATES>(0) ? D3D12_RESOURCE_STATE_COMMON : dx12State;
}

bool DX12_ResolveRuntimeOwnedOverlayTargetHDRState(DXGI_FORMAT format) {
    const bool cachedHDRStateValid = g_LastKnownSwapchainHDRStateValid.load(std::memory_order_acquire);
    const bool cachedHDRState = g_LastKnownSwapchainIsHDR.load(std::memory_order_acquire);
    const int cachedColorSpace = g_LastKnownSwapchainColorSpace.load(std::memory_order_acquire);
    const bool resolvedHDR = ce::dx12_overlay_policy::ResolveRuntimeOwnedCallbackHDRStateFromCachedState(
        static_cast<int>(format), cachedHDRStateValid, cachedHDRState);

    if (ce::dx12_overlay_policy::IsPresentationContractDependentFormat(static_cast<int>(format))) {
        static std::atomic<int> s_ffxPresentCachedHDRLogCount{0};
        const int logCount = s_ffxPresentCachedHDRLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || !cachedHDRStateValid) {
            HookLogImportant(
                "DX12: Runtime-owned overlay target using cached presentation HDR state "
                "(fmt=%d cachedValid=%d cachedHDR=%d cachedColorSpace=%d resolvedHDR=%d)",
                static_cast<int>(format), cachedHDRStateValid ? 1 : 0, cachedHDRState ? 1 : 0, cachedColorSpace,
                resolvedHDR ? 1 : 0);
        }
    }

    return resolvedHDR;
}

static void SyncSecondaryDx12OverlayColorState(DXGI_FORMAT format) {
    const bool isHdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
    g_D3D11On12Adapter.SetHDR(isHdr, static_cast<int>(format));

    static std::atomic<int> s_lastFormat{-1};
    static std::atomic<int> s_lastHdr{-1};
    const int previousFormat = s_lastFormat.exchange(static_cast<int>(format), std::memory_order_acq_rel);
    const int previousHdr = s_lastHdr.exchange(isHdr ? 1 : 0, std::memory_order_acq_rel);
    if (previousFormat != static_cast<int>(format) || previousHdr != (isHdr ? 1 : 0)) {
        HookLogImportant("DX12: Secondary overlay color contract synchronized format=%d hdr=%d",
                         static_cast<int>(format), isHdr ? 1 : 0);
    }
}

static bool ResolveSwapchainOutputHDRState(IDXGISwapChain* swapchain, DXGI_FORMAT format, const char* logPrefix,
                                           int* outColorSpace = nullptr, bool* outSupported = nullptr) {
    if (outColorSpace) {
        *outColorSpace = -1;
    }
    if (outSupported)
        *outSupported = false;

    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool hasTrackedColorSpace = false;
    const auto encoding = DXGIShared::ResolveSwapChainPresentationEncoding(
        swapchain, format, &colorSpace, &hasTrackedColorSpace);
    const bool supported = encoding != ce::presentation_color::Encoding::Unsupported;
    const bool isActualHDR = ce::presentation_color::IsHDR(encoding);
    if (outColorSpace && hasTrackedColorSpace)
        *outColorSpace = static_cast<int>(colorSpace);
    if (outSupported)
        *outSupported = supported;
    if (logPrefix) {
        HookLogImportant("%s - format=%d tracked=%d colorSpace=%d encoding=%s isHDR=%d", logPrefix,
                         static_cast<int>(format), hasTrackedColorSpace ? 1 : 0, static_cast<int>(colorSpace),
                         ce::presentation_color::Describe(encoding), isActualHDR ? 1 : 0);
    }
    return isActualHDR;
}

void DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain(void* swapChain) {
    auto* dxgiSwapChain = static_cast<IDXGISwapChain*>(swapChain);
    if (!dxgiSwapChain || !IsReadableSwapchainPointer(dxgiSwapChain) ||
        !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)dxgiSwapChain))) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(dxgiSwapChain->GetDesc(&desc))) {
        return;
    }

    int outputColorSpace = -1;
    bool presentationContractSupported = false;
    const bool isActualHDR = ResolveSwapchainOutputHDRState(
        dxgiSwapChain, desc.BufferDesc.Format, "DX12: Cached runtime-owned callback HDR source", &outputColorSpace,
        &presentationContractSupported);
    UpdateLastKnownSwapchainHDRStateCache(desc.BufferDesc.Format, isActualHDR, outputColorSpace,
                                          presentationContractSupported);

    // Cache backbuffer geometry for the no-callback FSR FG UI-resource substitution path: CE sizes its own
    // substitute UI texture to the backbuffer and uses these dims to tell a usable game UI texture apart from
    // a degenerate placeholder (GTA's 1x1). Logged on first capture / on change so a GTA run shows the size.
    if (desc.BufferDesc.Width != 0 && desc.BufferDesc.Height != 0) {
        const uint32_t prevW = g_NoCallbackBackbufferWidth.exchange(desc.BufferDesc.Width, std::memory_order_acq_rel);
        const uint32_t prevH = g_NoCallbackBackbufferHeight.exchange(desc.BufferDesc.Height, std::memory_order_acq_rel);
        g_NoCallbackBackbufferFormat.store(static_cast<uint32_t>(desc.BufferDesc.Format), std::memory_order_release);
        if (prevW != desc.BufferDesc.Width || prevH != desc.BufferDesc.Height) {
            HookLogImportant(
                "DX12: Cached no-callback FSR FG backbuffer geometry %ux%u fmt=%d (prev %ux%u) — sizes the CE "
                "substitute UI texture / classifies the game's registered UI texture",
                desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format), prevW, prevH);
        }
    }
}

static void TransitionResourceIfNeeded(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
                                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!cmdList || !resource || before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

static void CopyFFXPresentSourceToOutput(ID3D12GraphicsCommandList* cmdList,
                                         const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!cmdList || !desc || !desc->currentBackBuffer.resource || !desc->outputSwapChainBuffer.resource) {
        return;
    }

    auto* source = static_cast<ID3D12Resource*>(desc->currentBackBuffer.resource);
    auto* output = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    const D3D12_RESOURCE_STATES sourceState = GetDX12StateFromFFXResourceState(desc->currentBackBuffer.state);
    const D3D12_RESOURCE_STATES outputState = GetDX12StateFromFFXResourceState(desc->outputSwapChainBuffer.state);

    TransitionResourceIfNeeded(cmdList, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResourceIfNeeded(cmdList, output, outputState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyResource(output, source);
    TransitionResourceIfNeeded(cmdList, output, D3D12_RESOURCE_STATE_COPY_DEST, outputState);
    TransitionResourceIfNeeded(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
}

static bool EnsureOverlayAdapterReadyForFFXPresentCallback(
    const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!desc || !desc->device || !desc->outputSwapChainBuffer.resource) {
        return false;
    }

    auto* dx12Device = static_cast<ID3D12Device*>(desc->device);
    auto* outputResource = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    const D3D12_RESOURCE_DESC resourceDesc = outputResource->GetDesc();
    ID3D12CommandQueue* callbackQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> qLock(g_CommandQueueMutex);
        callbackQueue = g_SwapchainQueue ? g_SwapchainQueue : g_CommandQueue.load(std::memory_order_acquire);
        if (!callbackQueue) {
            callbackQueue = g_OriginalGameQueue;
        }
    }
    if (!callbackQueue) {
        HookLogImportant(
            "DX12: FFX present callback has no live queue for overlay backend init (device=%p frameId=%llu)",
            dx12Device, static_cast<unsigned long long>(desc->frameID));
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);

    g_State.cachedWidth = static_cast<int>(resourceDesc.Width);
    g_State.cachedHeight = static_cast<int>(resourceDesc.Height);
    g_State.format = resourceDesc.Format;

    if (ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(
            g_FFXPresentOverlayAdapter.IsInitialized(), g_FFXPresentOverlayDevice != dx12Device,
            g_FFXPresentOverlayFormat != resourceDesc.Format)) {
        HookLogImportant(
            "DX12: Resetting FFX present callback overlay backend before runtime-owned FSR render "
            "(oldDevice=%p newDevice=%p oldFmt=%d newFmt=%d)",
            g_FFXPresentOverlayDevice, dx12Device, static_cast<int>(g_FFXPresentOverlayFormat),
            static_cast<int>(resourceDesc.Format));
        g_FFXPresentOverlayAdapter.Shutdown();
    }

    if (!g_FFXPresentOverlayAdapter.IsInitialized()) {
        g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
        if (!g_FFXPresentOverlayAdapter.InitDX12(dx12Device, callbackQueue, static_cast<int>(resourceDesc.Format))) {
            HookLogImportant(
                "DX12: FFX present callback failed to initialize overlay adapter (device=%p fmt=%d frameId=%llu)",
                dx12Device, static_cast<int>(resourceDesc.Format), static_cast<unsigned long long>(desc->frameID));
            return false;
        }

        const bool callbackOutputHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(resourceDesc.Format);
        g_FFXPresentOverlayAdapter.SetHDR(callbackOutputHDR, static_cast<int>(resourceDesc.Format));
        g_FFXPresentOverlayDevice = dx12Device;
        g_FFXPresentOverlayFormat = resourceDesc.Format;
        HookLogImportant("DX12: FFX present callback initialized overlay adapter for runtime-owned FSR (fmt=%d hdr=%d)",
                         static_cast<int>(resourceDesc.Format), callbackOutputHDR ? 1 : 0);
    } else {
        static std::atomic<int> s_ffxPresentOverlayReuseLogCount{0};
        const int reuseLogCount = s_ffxPresentOverlayReuseLogCount.fetch_add(1, std::memory_order_relaxed);
        if (reuseLogCount < 10 || (reuseLogCount % 300) == 0) {
            HookLog(
                "DX12: Reusing FFX present callback overlay adapter for runtime-owned FSR (device=%p fmt=%d "
                "frameId=%llu)",
                dx12Device, static_cast<int>(resourceDesc.Format), static_cast<unsigned long long>(desc->frameID));
        }
    }

    return true;
}

static bool RenderOverlayViaFFXPresentCallback(const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!ShouldBridgeOverlayViaFFXPresentCallback(desc)) {
        return false;
    }

    if (!EnsureOverlayAdapterReadyForFFXPresentCallback(desc)) {
        return false;
    }

    auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(desc->commandList);
    auto* outputResource = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    auto* dx12Device = static_cast<ID3D12Device*>(desc->device);
    if (!cmdList || !outputResource || !dx12Device) {
        return false;
    }

    auto renderOverlayToResource = [&](ID3D12Resource* targetResource, D3D12_RESOURCE_STATES targetState,
                                       const char* targetName) -> bool {
        if (!targetResource) {
            return false;
        }

        const D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {};
        std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);
        if (!g_FFXPresentRtvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            if (FAILED(dx12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_FFXPresentRtvHeap)))) {
                HookLogImportant("DX12: FFX present callback failed to create RTV heap");
                return false;
            }
        }

        rtvHandle = g_FFXPresentRtvHeap->GetCPUDescriptorHandleForHeapStart();
        dx12Device->CreateRenderTargetView(targetResource, nullptr, rtvHandle);

        TransitionResourceIfNeeded(cmdList, targetResource, targetState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        g_FFXPresentOverlayAdapter.SetIPCClient(g_IPC);
        g_FFXPresentOverlayAdapter.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            g_FFXPresentOverlayAdapter.SetMetrics(perf);
        }
        g_FFXPresentOverlayAdapter.SetGraphicsAPI("DX12");
        g_FFXPresentOverlayAdapter.SetDX12RenderTarget(cmdList, reinterpret_cast<void*>(rtvHandle.ptr));
        g_FFXPresentOverlayAdapter.RenderOverlay(static_cast<int>(targetDesc.Width),
                                                 static_cast<int>(targetDesc.Height));

        TransitionResourceIfNeeded(cmdList, targetResource, D3D12_RESOURCE_STATE_RENDER_TARGET, targetState);

        static std::atomic<int> s_ffxPresentTargetLogCount{0};
        const int targetLogCount = s_ffxPresentTargetLogCount.fetch_add(1, std::memory_order_relaxed);
        if (targetLogCount < 20 || (targetLogCount % 600) == 0) {
            HookLog("DX12: FFX present callback rendered overlay target=%s resource=%p frameId=%llu generated=%d",
                    targetName ? targetName : "unknown", targetResource, static_cast<unsigned long long>(desc->frameID),
                    desc->isGeneratedFrame ? 1 : 0);
        }
        return true;
    };

    const D3D12_RESOURCE_STATES outputState = GetDX12StateFromFFXResourceState(desc->outputSwapChainBuffer.state);
    const bool renderedOutput = renderOverlayToResource(outputResource, outputState, "output");
    if (!renderedOutput) {
        return false;
    }

    const bool fsrApiActive = g_FGCompat.IsFSRFGApiActive();
    const bool nativeFSRSuspended =
        g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) && !fsrApiActive;
    auto* currentResource = static_cast<ID3D12Resource*>(desc->currentBackBuffer.resource);
    const bool mirrorToCurrent = ce::dx12_overlay_policy::ShouldMirrorFFXPresentCallbackOverlayToCurrentBackBuffer(
        desc->isGeneratedFrame != 0, currentResource != nullptr, outputResource != nullptr,
        currentResource != nullptr && currentResource != outputResource, nativeFSRSuspended);
    bool renderedCurrent = false;
    if (mirrorToCurrent) {
        renderedCurrent = renderOverlayToResource(
            currentResource, GetDX12StateFromFFXResourceState(desc->currentBackBuffer.state), "current-backbuffer");
    }

    static std::atomic<int> s_ffxPresentOverlayLogCount{0};
    const int logCount = s_ffxPresentOverlayLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        HookLogImportant(
            "DX12: FFX present callback rendered overlay on runtime-owned FSR path (generated=%d frameId=%llu "
            "output=%p current=%p mirroredCurrent=%d fsrApiActive=%d suspended=%d runtime=%s)",
            desc->isGeneratedFrame ? 1 : 0, static_cast<unsigned long long>(desc->frameID), outputResource,
            currentResource, renderedCurrent ? 1 : 0, fsrApiActive ? 1 : 0, nativeFSRSuspended ? 1 : 0,
            ce::fg_runtime::GetRuntimeModeName(runtimeMode));
    }
    if (nativeFSRSuspended) {
        static std::atomic<int> s_ffxPresentSuspendedRenderLogCount{0};
        const int suspendedLogCount = s_ffxPresentSuspendedRenderLogCount.fetch_add(1, std::memory_order_relaxed);
        if (suspendedLogCount < 20 || (suspendedLogCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback rendered overlay during native-FSR suspension "
                "(generated=%d frameId=%llu output=%p current=%p mirroredCurrent=%d log=%d)",
                desc->isGeneratedFrame ? 1 : 0, static_cast<unsigned long long>(desc->frameID), outputResource,
                currentResource, renderedCurrent ? 1 : 0, suspendedLogCount + 1);
        }
    }

    return true;
}

void DX12_SetFFXPresentCallbackBridge(void* bridgeKey, ce::ffx_api::PresentCallback originalCallback,
                                      void* originalUserContext) {
    if (!bridgeKey) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
    auto it = g_FFXPresentCallbackBridges.find(bridgeKey);
    if (originalCallback == &DX12_RenderOverlayViaFFXPresentCallback) {
        static std::atomic<int> s_selfBridgeLogCount{0};
        const int logCount = s_selfBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Ignoring recursive FFX present-callback bridge original for key=%p "
                "(existing=%d originalUserCtx=%p log=%d)",
                bridgeKey, it != g_FFXPresentCallbackBridges.end() ? 1 : 0, originalUserContext, logCount + 1);
        }
        if (it == g_FFXPresentCallbackBridges.end()) {
            g_FFXPresentCallbackBridges[bridgeKey] = {
                .originalCallback = nullptr,
                .originalUserContext = nullptr,
                .installed = true,
            };
        }
        return;
    }

    g_FFXPresentCallbackBridges[bridgeKey] = {
        .originalCallback = originalCallback,
        .originalUserContext = originalUserContext,
        .installed = true,
    };
}

bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
    const auto it = g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != g_FFXPresentCallbackBridges.end() && it->second.installed;
}

// True only when an installed bridge ALSO has a non-null original callback to delegate to. This is the
// signal that CE's bridge can do a CORRECT composition by calling the app/default callback instead of
// the fallback self-CopyResource (which wedges AMD). Used to keep the bridge delegating across an
// enabled app-callback->null-callback toggle, where AMD retains CE's bridge and would otherwise call it
// with a cleared (null) original. See guardrails.md (FFX present-callback toggle wedge, 20260615_021242).
bool DX12_HasFFXPresentCallbackBridgeWithOriginal(void* bridgeKey) {
    if (!bridgeKey) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
    const auto it = g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != g_FFXPresentCallbackBridges.end() && it->second.installed && it->second.originalCallback != nullptr &&
           it->second.originalCallback != &DX12_RenderOverlayViaFFXPresentCallback;
}

bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback) {
    return callback == &DX12_RenderOverlayViaFFXPresentCallback;
}

void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
    g_FFXPresentCallbackBridges.erase(bridgeKey);
}

static void ResetFFXPresentCallbackOverlayBackend(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_OverlayMutex);
    if (g_FFXPresentOverlayAdapter.IsInitialized()) {
        HookLogImportant("%s — resetting native FSR present-callback overlay adapter", reason);
        g_FFXPresentOverlayAdapter.Shutdown();
    }
    g_FFXPresentOverlayDevice = nullptr;
    g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
    if (g_FFXPresentRtvHeap) {
        g_FFXPresentRtvHeap->Release();
        g_FFXPresentRtvHeap = nullptr;
    }
}

static void ForceClearNativeFSRInternalNoCallbackComposition(const char* reason) {
    if (g_NativeFSRInternalNoCallbackComposition.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared retained native FSR internal no-callback composition route (%s)", reason);
        // Re-arm the VEH breakpoint for the next FG-on transition (one-shot detection cycle reset).
        FFXHook_ResetVehDisarmAndRearm();
    }
}

void DX12_OnNativeFSRPresentCallbackRoutingConfigured(bool enabled, bool bridgeActive, bool appCallbackProvided) {
    const bool previousInternalNoCallbackComposition =
        g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
    const bool runtimeOwnsLivePresentPath = g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath();
    const bool retainedNoCallbackSuspension =
        ce::dx12_overlay_policy::ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(
            enabled, bridgeActive, appCallbackProvided, previousInternalNoCallbackComposition,
            runtimeOwnsLivePresentPath);
    const bool internalNoCallbackComposition =
        (enabled && !bridgeActive && !appCallbackProvided) || retainedNoCallbackSuspension;
    g_FFXPresentCallbackBridgeExpected.store(bridgeActive, std::memory_order_release);
    g_NativeFSRInternalNoCallbackComposition.store(internalNoCallbackComposition, std::memory_order_release);
    if (!bridgeActive) {
        g_LastFFXPresentCallbackTickMs.store(0, std::memory_order_release);
        ResetFFXPresentCallbackFirstStallDetection();
    }

    if (retainedNoCallbackSuspension) {
        static std::atomic<int> s_retainedNoCallbackSuspensionLogCount{0};
        const int retainLogCount = s_retainedNoCallbackSuspensionLogCount.fetch_add(1, std::memory_order_relaxed);
        if (retainLogCount < 20 || (retainLogCount % 300) == 0) {
            HookLogImportant(
                "DX12: Native FSR suspension retains internal no-callback composition route — normal overlay "
                "rendering stays allowed on the runtime-owned swapchain queue (runtime=%s scQueue=%p origGame=%p "
                "fgOwned=%d log=%d)",
                ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_SwapchainQueue, g_OriginalGameQueue,
                g_FGRuntimeOwnsSwapchain ? 1 : 0, retainLogCount + 1);
        }
    }

    static std::atomic<int> s_nativeFSRCallbackRoutingLogCount{0};
    const int logCount = s_nativeFSRCallbackRoutingLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 30 || (logCount % 300) == 0 || internalNoCallbackComposition) {
        HookLogImportant(
            "DX12: Native FSR present routing configured (enabled=%d bridgeActive=%d appCallback=%d "
            "internalNoCallback=%d retainedNoCallbackSuspension=%d runtime=%s scQueue=%p origGame=%p log=%d)",
            enabled ? 1 : 0, bridgeActive ? 1 : 0, appCallbackProvided ? 1 : 0, internalNoCallbackComposition ? 1 : 0,
            retainedNoCallbackSuspension ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()),
            g_SwapchainQueue, g_OriginalGameQueue, logCount + 1);
    }
}

void DX12_OnNativeFSRFrameGenerationContextsDestroyed() {
    ForceClearNativeFSRInternalNoCallbackComposition("native FSR contexts destroyed");
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "all native FSR contexts destroyed");
    // The destroyed contexts are the strong half of the "stronger off signal":
    // the next GAME-created swapchain ends the runtime-owned teardown.
    g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(true, std::memory_order_release);
    DX12_OnNativeFSRPresentCallbackRoutingConfigured(false, false, false);
    g_RenderWatchdog.SetRuntimePresentationMonitor(false);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    static std::atomic<int> s_nativeFSRContextsDestroyedLogCount{0};
    const int logCount = s_nativeFSRContextsDestroyedLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Native FSR contexts destroyed; cleared callback routing and runtime presentation monitor "
            "(runtime=%s scQueue=%p origGame=%p log=%d)",
            ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), g_SwapchainQueue, g_OriginalGameQueue,
            logCount + 1);
    }
}

void DX12_OnNativeFSRFrameGenerationConfigured(bool enabled, bool retainedPresentCallbackBridge) {
    static std::atomic<int> s_nativeFSROffRuntimeTeardownLogCount{0};

    if (enabled) {
        g_RenderWatchdog.SetRuntimePresentationMonitor(true);
        s_nativeFSROffRuntimeTeardownLogCount.store(0, std::memory_order_release);
        g_FGCompat.SetFSRFGSupportPresent(true);
        g_FGCompat.SetFSRFGMultiplier(2);
        g_FGCompat.SetFSRFGActive(true);
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        // A fresh enabled configure starts a new FSR session; stale
        // off/destroy recovery evidence must not leak into it.
        g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
        g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
        g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
        g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
        char deferredModulePath[MAX_PATH] = {};
        ID3D12CommandQueue* deferredQueue =
            ConsumeDeferredOfficialFFXTakeoverSideEffects(deferredModulePath, sizeof(deferredModulePath));
        const bool protectedOfficialStartup =
            g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel);
        if (protectedOfficialStartup) {
            ResetProtectedOfficialFFXStartupProgressCounters();
        }
        if (deferredQueue) {
            HookLogImportant(
                "DX12: Finalizing staged official FFX takeover after enabled ffxConfigure "
                "(queue=%p module=%s)",
                deferredQueue, deferredModulePath[0] ? deferredModulePath : "unknown");
            ApplyAuthoritativeFFXTakeoverSideEffects(
                deferredQueue, deferredModulePath[0] ? deferredModulePath : "official FFX runtime",
                "native FSR enabled configure");
            deferredQueue->Release();
        } else if (protectedOfficialStartup) {
            HookLogImportant(
                "DX12: Finalizing protected official FFX startup pass-through after enabled ffxConfigure "
                "(no queue captured before configure)");
            ApplyAuthoritativeFFXTakeoverSideEffects(nullptr, "official FFX runtime",
                                                     "native FSR enabled configure after protected pass-through");
        }
        SetNativeFSRStartupConfigureArmingPending(false, "native FSR enabled configure");
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kNativeFSRConfigureOn,
                                    "DX12_OnNativeFSRFrameGenerationConfigured", nullptr, nullptr,
                                    ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
        return;
    }

    const bool runtimeOwnedTeardownStillActive =
        ce::dx12_overlay_policy::ShouldPreserveRuntimeOwnedNativeFGPresentPathAfterDisabledConfigure(
            g_FGRuntimeOwnsSwapchain, HookHasRuntimeOwnedNativeFGPresentPath(), retainedPresentCallbackBridge,
            g_FGCompat.HasDirectFFXApiConfirmation());
    g_RenderWatchdog.SetRuntimePresentationMonitor(runtimeOwnedTeardownStillActive);
    SetNativeFSRStartupConfigureArmingPending(false, "native FSR disabled configure");
    ClearDeferredOfficialFFXTakeoverSideEffects("native FSR disabled configure");
    ClearProtectedOfficialFFXStartupSwapchainPending("native FSR disabled configure");
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("native FSR disabled configure");
    g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(runtimeOwnedTeardownStillActive, std::memory_order_release);
    if (runtimeOwnedTeardownStillActive) {
        const int logCount = s_nativeFSROffRuntimeTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Native FSR explicitly configured FG OFF while runtime-owned swapchain teardown is still active "
                "(scQueue=%p origGame=%p cmdQ=%p retainedBridge=%d directFFX=%d log=%d)",
                g_SwapchainQueue, g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                retainedPresentCallbackBridge ? 1 : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0, logCount + 1);
        }
    }
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kNativeFSRConfigureOff,
                                "DX12_OnNativeFSRFrameGenerationConfigured", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kOff, false, true);
}

uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* desc, void* userCtx) {
    g_LastFFXPresentCallbackTickMs.store(GetTickCount64(), std::memory_order_release);

    static thread_local int s_ffxPresentCallbackDepth = 0;
    if (s_ffxPresentCallbackDepth > 0) {
        static std::atomic<int> s_recursiveCallbackLogCount{0};
        const int logCount = s_recursiveCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Suppressing recursive FFX present-callback bridge entry "
                "(depth=%d frameId=%llu userCtx=%p log=%d)",
                s_ffxPresentCallbackDepth, desc ? static_cast<unsigned long long>(desc->frameID) : 0ULL, userCtx,
                logCount + 1);
        }
        return 0;
    }

    struct CallbackDepthGuard {
        int& depth;
        explicit CallbackDepthGuard(int& d) : depth(d) {
            ++depth;
        }
        ~CallbackDepthGuard() {
            --depth;
        }
    } depthGuard(s_ffxPresentCallbackDepth);

    ce::ffx_api::PresentCallback originalCallback = nullptr;
    void* originalUserContext = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_FFXPresentCallbackBridgeMutex);
        const auto it = g_FFXPresentCallbackBridges.find(userCtx);
        if (it != g_FFXPresentCallbackBridges.end()) {
            originalCallback = it->second.originalCallback;
            originalUserContext = it->second.originalUserContext;
        }
    }

    uint32_t result = 0;
    if (originalCallback) {
        if (originalCallback == &DX12_RenderOverlayViaFFXPresentCallback) {
            static std::atomic<int> s_selfOriginalCallbackLogCount{0};
            const int logCount = s_selfOriginalCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: FFX present-callback bridge original was CE bridge; skipping recursive original "
                    "(frameId=%llu userCtx=%p originalUserCtx=%p log=%d)",
                    desc ? static_cast<unsigned long long>(desc->frameID) : 0ULL, userCtx, originalUserContext,
                    logCount + 1);
            }
        } else {
            result = originalCallback(desc, originalUserContext);
        }
    }

    if (!desc) {
        return result;
    }

    if (result != 0) {
        static std::atomic<int> s_ffxPresentCallbackErrorLogCount{0};
        if (s_ffxPresentCallbackErrorLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipping overlay because runtime callback returned 0x%08X "
                "(frameId=%llu)",
                result, static_cast<unsigned long long>(desc->frameID));
        }
        return result;
    }

    static std::atomic<int> s_ffxPresentPremulLogCount{0};
    const bool usePremulAlpha = ce::ffx_api::ResolvePresentCallbackUsePremulAlpha(desc);
    const int premulLogCount = s_ffxPresentPremulLogCount.fetch_add(1, std::memory_order_relaxed);
    if (premulLogCount < 10) {
        HookLogImportant("DX12: FFX present callback composition contract (frameId=%llu premulAlpha=%d)",
                         static_cast<unsigned long long>(desc->frameID), usePremulAlpha ? 1 : 0);
    }

    const bool ffxCallbackHasCurrentBackBuffer = desc->currentBackBuffer.resource != nullptr;
    const bool ffxCallbackOutputDiffersFromCurrent =
        desc->currentBackBuffer.resource != desc->outputSwapChainBuffer.resource;
    const auto ffxCallbackRuntimeMode = g_FGCompat.GetRuntimeMode();
    const bool ffxRuntimeOwnsNativeFSRPresentation =
        g_FGCompat.IsFSRFGApiActive() || ce::fg_runtime::RuntimeModeUsesFSR(ffxCallbackRuntimeMode) ||
        (g_FGRuntimeOwnsSwapchain && g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire));
