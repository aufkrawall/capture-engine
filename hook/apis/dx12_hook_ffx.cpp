#include "dx12_hook_internal.h"

static bool KnownDLSSFGModuleLoaded() {
    if (dx12_hook_g_KnownDLSSFGModuleSeen.load(std::memory_order_acquire)) {
        return true;
    }

    constexpr const wchar_t* kKnownDLSSFGModules[] = {
        L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_g.dll", L"nvngx_dlssg.dll", L"nvngx_dlss.dll",
    };

    for (const wchar_t* moduleName : kKnownDLSSFGModules) {
        if (GetModuleHandleW(moduleName)) {
            dx12_hook_g_KnownDLSSFGModuleSeen.store(true, std::memory_order_release);
            return true;
        }
    }

    return false;
}

static bool ShouldBridgeOverlayViaFFXPresentCallback(const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!desc) {
        return false;
    }

    const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
    const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
    const bool directFFXConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (!ce::dx12_overlay_policy::ShouldBridgeOverlayViaFFXPresentCallback(
            runtimeOwnedNativeFGPresentPath, authoritativeFSRActive, directFFXConfirmation, runtimeMode)) {
        static std::atomic<int> s_ffxPresentBridgeSkippedNoEvidenceLogCount{0};
        const int logCount = s_ffxPresentBridgeSkippedNoEvidenceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because no authoritative native FSR evidence is "
                "active (runtimeOwnedNativePath=%d apiFSR=%d directFFX=%d runtime=%s frameId=%llu log=%d)",
                runtimeOwnedNativeFGPresentPath ? 1 : 0, authoritativeFSRActive ? 1 : 0, directFFXConfirmation ? 1 : 0,
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), static_cast<unsigned long long>(desc->frameID),
                logCount + 1);
        }
        return false;
    }

    if (!desc->device || !desc->commandList || !desc->outputSwapChainBuffer.resource) {
        static std::atomic<int> s_ffxPresentBridgeInvalidDescLogCount{0};
        const int logCount = s_ffxPresentBridgeInvalidDescLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because callback resources are incomplete "
                "(device=%p cmdList=%p output=%p frameId=%llu log=%d)",
                desc->device, desc->commandList, desc->outputSwapChainBuffer.resource,
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    if (!g_IPC) {
        static std::atomic<int> s_ffxPresentBridgeNoIPCLogCount{0};
        const int logCount = s_ffxPresentBridgeNoIPCLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because IPC is not connected "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
    if (!shm) {
        static std::atomic<int> s_ffxPresentBridgeNoSharedMemLogCount{0};
        const int logCount = s_ffxPresentBridgeNoSharedMemLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge skipped overlay because shared memory is unavailable "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    const OverlayConfig cfg = GetActiveDX12OverlayConfig(shm);
    if (!cfg.showOverlay) {
        static std::atomic<int> s_ffxPresentBridgeOverlayHiddenLogCount{0};
        const int logCount = s_ffxPresentBridgeOverlayHiddenLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLog(
                "DX12: FFX present callback bridge skipped overlay because overlay is hidden "
                "(frameId=%llu log=%d)",
                static_cast<unsigned long long>(desc->frameID), logCount + 1);
        }
        return false;
    }

    return true;
}

static D3D12_RESOURCE_STATES GetDX12StateFromFFXResourceState(uint32_t state) {
    D3D12_RESOURCE_STATES dx12State = static_cast<D3D12_RESOURCE_STATES>(0);

    if (state & ce::ffx_api::kResourceStateUnorderedAccess) {
        dx12State |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

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
    const bool cachedHDRStateValid = dx12_hook_g_LastKnownSwapchainHDRStateValid.load(std::memory_order_acquire);
    const bool cachedHDRState = dx12_hook_g_LastKnownSwapchainIsHDR.load(std::memory_order_acquire);
    const int cachedColorSpace = dx12_hook_g_LastKnownSwapchainColorSpace.load(std::memory_order_acquire);
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
        const uint32_t prevW = dx12_hook_g_NoCallbackBackbufferWidth.exchange(desc.BufferDesc.Width, std::memory_order_acq_rel);
        const uint32_t prevH = dx12_hook_g_NoCallbackBackbufferHeight.exchange(desc.BufferDesc.Height, std::memory_order_acq_rel);
        dx12_hook_g_NoCallbackBackbufferFormat.store(static_cast<uint32_t>(desc.BufferDesc.Format), std::memory_order_release);
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
        callbackQueue = dx12_hook_g_SwapchainQueue ? dx12_hook_g_SwapchainQueue : g_CommandQueue.load(std::memory_order_acquire);
        if (!callbackQueue) {
            callbackQueue = dx12_hook_g_OriginalGameQueue;
        }
    }
    if (!callbackQueue) {
        HookLogImportant(
            "DX12: FFX present callback has no live queue for overlay backend init (device=%p frameId=%llu)",
            dx12Device, static_cast<unsigned long long>(desc->frameID));
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

    dx12_hook_g_State.cachedWidth = static_cast<int>(resourceDesc.Width);
    dx12_hook_g_State.cachedHeight = static_cast<int>(resourceDesc.Height);
    dx12_hook_g_State.format = resourceDesc.Format;

    if (ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(
            dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized(), dx12_hook_g_FFXPresentOverlayDevice != dx12Device,
            dx12_hook_g_FFXPresentOverlayFormat != resourceDesc.Format)) {
        HookLogImportant(
            "DX12: Resetting FFX present callback overlay backend before runtime-owned FSR render "
            "(oldDevice=%p newDevice=%p oldFmt=%d newFmt=%d)",
            dx12_hook_g_FFXPresentOverlayDevice, dx12Device, static_cast<int>(dx12_hook_g_FFXPresentOverlayFormat),
            static_cast<int>(resourceDesc.Format));
        dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
    }

    if (!dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
        dx12_hook_g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
        if (!dx12_hook_g_FFXPresentOverlayAdapter.InitDX12(dx12Device, callbackQueue, static_cast<int>(resourceDesc.Format))) {
            HookLogImportant(
                "DX12: FFX present callback failed to initialize overlay adapter (device=%p fmt=%d frameId=%llu)",
                dx12Device, static_cast<int>(resourceDesc.Format), static_cast<unsigned long long>(desc->frameID));
            return false;
        }

        const bool callbackOutputHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(resourceDesc.Format);
        dx12_hook_g_FFXPresentOverlayAdapter.SetHDR(callbackOutputHDR, static_cast<int>(resourceDesc.Format));
        dx12_hook_g_FFXPresentOverlayDevice = dx12Device;
        dx12_hook_g_FFXPresentOverlayFormat = resourceDesc.Format;
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
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);
        if (!dx12_hook_g_FFXPresentRtvHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            if (FAILED(dx12Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&dx12_hook_g_FFXPresentRtvHeap)))) {
                HookLogImportant("DX12: FFX present callback failed to create RTV heap");
                return false;
            }
        }

        rtvHandle = dx12_hook_g_FFXPresentRtvHeap->GetCPUDescriptorHandleForHeapStart();
        dx12Device->CreateRenderTargetView(targetResource, nullptr, rtvHandle);

        TransitionResourceIfNeeded(cmdList, targetResource, targetState, D3D12_RESOURCE_STATE_RENDER_TARGET);

        dx12_hook_g_FFXPresentOverlayAdapter.SetIPCClient(g_IPC);
        dx12_hook_g_FFXPresentOverlayAdapter.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            dx12_hook_g_FFXPresentOverlayAdapter.SetMetrics(perf);
        }
        dx12_hook_g_FFXPresentOverlayAdapter.SetGraphicsAPI("DX12");
        dx12_hook_g_FFXPresentOverlayAdapter.SetDX12RenderTarget(cmdList, reinterpret_cast<void*>(rtvHandle.ptr));
        dx12_hook_g_FFXPresentOverlayAdapter.RenderOverlay(static_cast<int>(targetDesc.Width),
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
        dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) && !fsrApiActive;
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

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    if (originalCallback == &DX12_RenderOverlayViaFFXPresentCallback) {
        static std::atomic<int> s_selfBridgeLogCount{0};
        const int logCount = s_selfBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Ignoring recursive FFX present-callback bridge original for key=%p "
                "(existing=%d originalUserCtx=%p log=%d)",
                bridgeKey, it != dx12_hook_g_FFXPresentCallbackBridges.end() ? 1 : 0, originalUserContext, logCount + 1);
        }
        if (it == dx12_hook_g_FFXPresentCallbackBridges.end()) {
            dx12_hook_g_FFXPresentCallbackBridges[bridgeKey] = {
                .originalCallback = nullptr,
                .originalUserContext = nullptr,
                .installed = true,
            };
        }
        return;
    }

    dx12_hook_g_FFXPresentCallbackBridges[bridgeKey] = {
        .originalCallback = originalCallback,
        .originalUserContext = originalUserContext,
        .installed = true,
    };
}

bool DX12_HasFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return false;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != dx12_hook_g_FFXPresentCallbackBridges.end() && it->second.installed;
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

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(bridgeKey);
    return it != dx12_hook_g_FFXPresentCallbackBridges.end() && it->second.installed && it->second.originalCallback != nullptr &&
           it->second.originalCallback != &DX12_RenderOverlayViaFFXPresentCallback;
}

bool DX12_IsFFXPresentCallbackBridgeCallback(ce::ffx_api::PresentCallback callback) {
    return callback == &DX12_RenderOverlayViaFFXPresentCallback;
}

void DX12_ClearFFXPresentCallbackBridge(void* bridgeKey) {
    if (!bridgeKey) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
    dx12_hook_g_FFXPresentCallbackBridges.erase(bridgeKey);
}

void DX12_OnNativeFSRPresentCallbackRoutingConfigured(bool enabled, bool bridgeActive, bool appCallbackProvided) {
    const bool previousInternalNoCallbackComposition =
        dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
    const bool runtimeOwnsLivePresentPath = dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath();
    const bool retainedNoCallbackSuspension =
        ce::dx12_overlay_policy::ShouldRetainNativeFSRInternalNoCallbackCompositionForDisabledConfigure(
            enabled, bridgeActive, appCallbackProvided, previousInternalNoCallbackComposition,
            runtimeOwnsLivePresentPath);
    const bool internalNoCallbackComposition =
        (enabled && !bridgeActive && !appCallbackProvided) || retainedNoCallbackSuspension;
    dx12_hook_g_FFXPresentCallbackBridgeExpected.store(bridgeActive, std::memory_order_release);
    dx12_hook_g_NativeFSRInternalNoCallbackComposition.store(internalNoCallbackComposition, std::memory_order_release);
    if (!bridgeActive) {
        dx12_hook_g_LastFFXPresentCallbackTickMs.store(0, std::memory_order_release);
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
                ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
                dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, retainLogCount + 1);
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
            dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, logCount + 1);
    }
}

void DX12_OnNativeFSRFrameGenerationContextsDestroyed() {
    ForceClearNativeFSRInternalNoCallbackComposition("native FSR contexts destroyed");
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "all native FSR contexts destroyed");
    // The destroyed contexts are the strong half of the "stronger off signal":
    // the next GAME-created swapchain ends the runtime-owned teardown.
    dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(true, std::memory_order_release);
    DX12_OnNativeFSRPresentCallbackRoutingConfigured(false, false, false);
    g_RenderWatchdog.SetRuntimePresentationMonitor(false);
    ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
    static std::atomic<int> s_nativeFSRContextsDestroyedLogCount{0};
    const int logCount = s_nativeFSRContextsDestroyedLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Native FSR contexts destroyed; cleared callback routing and runtime presentation monitor "
            "(runtime=%s scQueue=%p origGame=%p log=%d)",
            ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue,
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
        dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
        dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
        dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
        char deferredModulePath[MAX_PATH] = {};
        ID3D12CommandQueue* deferredQueue =
            ConsumeDeferredOfficialFFXTakeoverSideEffects(deferredModulePath, sizeof(deferredModulePath));
        const bool protectedOfficialStartup =
            dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel);
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
            dx12_hook_g_FGRuntimeOwnsSwapchain, HookHasRuntimeOwnedNativeFGPresentPath(), retainedPresentCallbackBridge,
            g_FGCompat.HasDirectFFXApiConfirmation());
    g_RenderWatchdog.SetRuntimePresentationMonitor(runtimeOwnedTeardownStillActive);
    SetNativeFSRStartupConfigureArmingPending(false, "native FSR disabled configure");
    ClearDeferredOfficialFFXTakeoverSideEffects("native FSR disabled configure");
    ClearProtectedOfficialFFXStartupSwapchainPending("native FSR disabled configure");
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("native FSR disabled configure");
    dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(runtimeOwnedTeardownStillActive, std::memory_order_release);
    if (runtimeOwnedTeardownStillActive) {
        const int logCount = s_nativeFSROffRuntimeTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Native FSR explicitly configured FG OFF while runtime-owned swapchain teardown is still active "
                "(scQueue=%p origGame=%p cmdQ=%p retainedBridge=%d directFFX=%d log=%d)",
                dx12_hook_g_SwapchainQueue, dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                retainedPresentCallbackBridge ? 1 : 0, g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0, logCount + 1);
        }
    }
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kNativeFSRConfigureOff,
                                "DX12_OnNativeFSRFrameGenerationConfigured", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kOff, false, true);
}

uint32_t DX12_RenderOverlayViaFFXPresentCallback(ce::ffx_api::CallbackDescFrameGenerationPresent* desc, void* userCtx) {
    dx12_hook_g_LastFFXPresentCallbackTickMs.store(GetTickCount64(), std::memory_order_release);

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
        std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
        const auto it = dx12_hook_g_FFXPresentCallbackBridges.find(userCtx);
        if (it != dx12_hook_g_FFXPresentCallbackBridges.end()) {
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
        (dx12_hook_g_FGRuntimeOwnsSwapchain && dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire));

    const bool shouldComposeCurrentToOutput = ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(
        originalCallback != nullptr, ffxCallbackHasCurrentBackBuffer, ffxCallbackOutputDiffersFromCurrent);
    if (shouldComposeCurrentToOutput) {
        static std::atomic<int> s_ffxPresentComposeCopyLogCount{0};
        const int composeLogCount = s_ffxPresentComposeCopyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (composeLogCount < 10 || (composeLogCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge composing current backbuffer into output because no "
                "app/default composition callback is available (frameId=%llu generated=%d runtimeOwnedNativeFSR=%d "
                "runtime=%s log=%d)",
                static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                ffxRuntimeOwnsNativeFSRPresentation ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(ffxCallbackRuntimeMode),
                composeLogCount + 1);
        }
        // WEDGE PRECURSOR DIAGNOSTIC: self-composing on AMD's command list while AMD owns the native-FSR
        // presentation is the documented ffxQuery-wedge path (session 20260615_021242). With the
        // app->null-callback toggle fix this should no longer be reached (CE's bridge keeps a delegate),
        // so if it fires for a runtime-owned FSR it is the high-risk case — log the resource states once
        // so the exact desc encoding (native vs FFX) is attributable from the log alone.
        if (ffxRuntimeOwnsNativeFSRPresentation) {
            static std::atomic<int> s_ffxComposeWedgeRiskLogCount{0};
            const int wedgeLogCount = s_ffxComposeWedgeRiskLogCount.fetch_add(1, std::memory_order_relaxed);
            if (wedgeLogCount < 20 || (wedgeLogCount % 120) == 0) {
                HookLogImportant(
                    "DX12: WARNING FFX bridge self-composing on AMD's command list for runtime-owned FSR — "
                    "ffxQuery-wedge risk (frameId=%llu generated=%d rawCurrentState=0x%X rawOutputState=0x%X "
                    "outputDiffersFromCurrent=%d log=%d)",
                    static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                    (unsigned)desc->currentBackBuffer.state, (unsigned)desc->outputSwapChainBuffer.state,
                    ffxCallbackOutputDiffersFromCurrent ? 1 : 0, wedgeLogCount + 1);
            }
        }
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(desc->commandList);
        // GPU-breadcrumb the no-app-callback self-compose path (recorded into AMD's command list, which AMD
        // executes after this callback returns). On freeze: start=reached the callback, rt=self-compose copy
        // executed, draw=overlay executed. If all reach the latest seq but ffxQuery still wedges, even AMD's
        // correct-state path can't host CE work; if they stop, that op is where AMD's GPU hangs.
        BeginOverlayGpuBreadcrumbFrame(static_cast<ID3D12Device*>(desc->device));
        WriteOverlayGpuBreadcrumb(cmdList, kOverlayBcStart);
        CopyFFXPresentSourceToOutput(cmdList, desc);
        WriteOverlayGpuBreadcrumb(cmdList, kOverlayBcAfterRTBarrier);
    } else if (originalCallback && ffxCallbackHasCurrentBackBuffer && ffxCallbackOutputDiffersFromCurrent &&
               (desc->isGeneratedFrame || ffxRuntimeOwnsNativeFSRPresentation)) {
        static std::atomic<int> s_ffxPresentAppCompositionLogCount{0};
        const int logCount = s_ffxPresentAppCompositionLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge preserving app/default-composed runtime output "
                "(frameId=%llu generated=%d runtimeOwnedNativeFSR=%d runtime=%s log=%d)",
                static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                ffxRuntimeOwnsNativeFSRPresentation ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(ffxCallbackRuntimeMode),
                logCount + 1);
        }
    }

    if (RenderOverlayViaFFXPresentCallback(desc)) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }
    WriteOverlayGpuBreadcrumb(static_cast<ID3D12GraphicsCommandList*>(desc->commandList), kOverlayBcAfterDraw);
    WriteOverlayGpuBreadcrumb(static_cast<ID3D12GraphicsCommandList*>(desc->commandList), kOverlayBcBeforeClose);
    HookUpdatePreferredOverlayFGPublicationState(g_FGCompat.IsFGActive(), g_FGCompat.GetRuntimeMode(),
                                                 "DX12_RenderOverlayViaFFXPresentCallback");
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
        ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, g_FGCompat.GetOutputFPS(), g_FGCompat.GetBaseFPS(),
                                                     g_FGCompat.GetFGMultiplier(),
                                                     "DX12_RenderOverlayViaFFXPresentCallback");
        static std::atomic<int> s_ffxCallbackFGPublishLogCount{0};
        if (s_ffxCallbackFGPublishLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("FG publication: source=DX12_RenderOverlayViaFFXPresentCallback runtime=%s multiplier=%d",
                             ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode), g_FGCompat.GetFGMultiplier());
        }
    }
    return result;
}

static ID3D12Device* g_FFXUiCompositeDevice = nullptr;

static ID3D12DescriptorHeap* g_FFXUiCompositeRtvHeap = nullptr;

static constexpr int kFFXUiCompositeSlotCount =
    3;  // 3-slot rotation recycled by fence value (signaled on CE's own queue).

static ID3D12CommandAllocator* g_FFXUiCompositeAlloc[kFFXUiCompositeSlotCount] = {};

static ID3D12GraphicsCommandList* g_FFXUiCompositeList = nullptr;

static HANDLE g_FFXUiCompositeFenceEvent = nullptr;

static UINT64 g_FFXUiCompositeAllocFenceVal[kFFXUiCompositeSlotCount] = {};

static DXGI_FORMAT g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;

static std::atomic<bool> g_FFXUiResourceCompositionActive{false};

static std::atomic<uint64_t> g_FFXUiCompositeLastTickMs{0};

// --- Step 3: Bundle overlay into the game's existing ECL (no extra ECL call) ---------------------------
// FSR's fence tracking counts ECL *calls* (ExecuteCommandLists invocations), not command lists within an
// ECL. Appending CE's overlay command list to the game's existing ECL (NumCommandLists + 1, same ECL call)
// adds zero extra ECL calls and zero extra Signals → no fence tracking corruption. The UI texture is
// written on the game queue (as part of the game's ECL) → no foreign-queue write. No mode switch → no
// callback wedge. The cached UI texture comes from the previous frame's ffxConfigure (GTA registers the
// same texture every frame).
static std::atomic<ID3D12Resource*> g_CachedFFXUiTexture{nullptr};

static std::atomic<uint32_t> g_CachedFFXUiState{0};

static std::atomic<uint32_t> g_CachedFFXUiFlags{0};

// When the bundle target is CE's OWN substituted texture (the game registered a degenerate placeholder, e.g.
// GTA's 1x1), the texture starts empty and must be cleared to transparent each frame so only the overlay
// composites over the game frame. When the target is the game's own usable UI texture, we must NOT clear (the
// overlay blends on top of the game's HUD already present in that texture).
static std::atomic<bool> g_BundleTargetNeedsTransparentClear{false};

// (g_NoCallbackBackbufferWidth/Height/Format are declared earlier, near g_FFXPresentOverlayFormat, because
// DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain writes them before this point in the file.)
// CE-owned, backbuffer-sized UI texture substituted into RegisterUiResource when the game's UI texture is
// degenerate. AMD composites THIS texture post-interpolation; the game-ECL bundle draws the overlay onto it
// every frame on the game queue. CE owns it (persists across frames), so the cached bundle-target pointer
// stays valid. Released on teardown / device change (ReleaseFFXUiCompositeInfra).
static ID3D12Resource* g_CEUiSubstituteTexture = nullptr;

static uint32_t g_CEUiSubstituteWidth = 0;

static uint32_t g_CEUiSubstituteHeight = 0;

static DXGI_FORMAT g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;

static D3D12_RESOURCE_STATES g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;

static std::atomic<uint64_t> g_FFXUiPreparationSequence{0};

static uint64_t g_FFXUiCommittedPreparationSequence = 0;  // guarded by g_FFXUiCompositeMutex

// The presenter-thread compatibility driver can observe both real and generated output Presents for one
// registered UI input. Composite at most once per accepted RegisterUiResource sequence or alpha blending is
// applied repeatedly to the same texture and the two outputs visibly alternate in intensity.
static uint64_t g_FFXUiPresenterFallbackLastSequence = 0;  // guarded by g_FFXUiCompositeMutex

static FFXUiCompositeTimelineEntry g_FFXUiCompositeTimeline[dx12_hook_kFFXUiCompositeTimelineSize];

static std::atomic<uint32_t> g_FFXUiCompositeTimelineIdx{0};

// QPC of the most recent ffxConfigure forward call (set in Hooked_ffxConfigure, read by the next timeline entry).
static std::atomic<uint64_t> g_LastFfxConfigureForwardQpc{0};

// Frame counter for ffxConfigure calls (separate from g_FFXUiCompositeFrame to correlate configure vs composite).
static std::atomic<uint64_t> g_FfxConfigureFrame{0};

static void RecordFFXUiCompositeTimelineEntry(const FFXUiCompositeTimelineEntry& entry) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.fetch_add(1, std::memory_order_relaxed);
    g_FFXUiCompositeTimeline[idx % dx12_hook_kFFXUiCompositeTimelineSize] = entry;
}

static void DumpFFXUiCompositeTimeline(const char* reason) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.load(std::memory_order_relaxed);
    const int count = (idx < static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? static_cast<int>(idx)
                                                                                 : dx12_hook_kFFXUiCompositeTimelineSize;
    if (count == 0) {
        HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — no composite calls recorded yet", reason ?: "freeze");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqMs = static_cast<double>(freq.QuadPart) / 1000.0;
    const uint32_t startIdx =
        (idx >= static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? (idx % dx12_hook_kFFXUiCompositeTimelineSize) : 0;
    HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — %d entries (total=%u)", reason ?: "freeze", count, idx);
    for (int i = 0; i < count; ++i) {
        const uint32_t slotIdx = (startIdx + i) % dx12_hook_kFFXUiCompositeTimelineSize;
        const auto& e = g_FFXUiCompositeTimeline[slotIdx];
        const double submitToReturnMs =
            (e.returnQpc && e.submitQpc && freqMs > 0) ? static_cast<double>(e.returnQpc - e.submitQpc) / freqMs : -1.0;
        HookLogImportant(
            "  [timeline %d/%d] frame=%llu slot=%u fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u "
            "gameEcl=%u submitQpc=%llu returnQpc=%llu submitToReturnMs=%.3f uiTex=%p ffxState=0x%X queue=%p",
            i + 1, count, static_cast<unsigned long long>(e.frame), e.slot, static_cast<unsigned long long>(e.fenceVal),
            static_cast<unsigned long long>(e.fenceCompleted), e.waitTimedOut, e.gameEclCount,
            static_cast<unsigned long long>(e.submitQpc), static_cast<unsigned long long>(e.returnQpc),
            submitToReturnMs, e.uiTexture, e.ffxState, e.queue);
    }
    const uint64_t lastForwardQpc = g_LastFfxConfigureForwardQpc.load(std::memory_order_relaxed);
    const uint64_t cfgFrame = g_FfxConfigureFrame.load(std::memory_order_relaxed);
    HookLogImportant("  [timeline] lastFfxConfigureForwardQpc=%llu ffxConfigureFrame=%llu",
                     static_cast<unsigned long long>(lastForwardQpc), static_cast<unsigned long long>(cfgFrame));
}

// Called from DX12_LogOverlayGpuBreadcrumbs (via the freeze watchdog) to log the CE composite fence
// completion state + dump the timeline ring buffer. This is the key diagnostic that distinguishes
// "CE's Signal completed → AMD wedged after the Signal" from "CE's Signal never completed → wedge
// is at/before the Signal" — the fork between the Signal-wedge and ECL-wedge hypotheses.
void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason) {
    const uint64_t fenceVal = dx12_hook_g_FFXUiCompositeFenceVal;
    const uint64_t fenceCompleted = dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0;
    const int frame = dx12_hook_g_FFXUiCompositeFrame;
    const bool compositionActive = g_FFXUiResourceCompositionActive.load(std::memory_order_acquire);
    const uint64_t lastTickMs = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    HookLogImportant(
        "DX12: [ffx-ui-composite-freeze-diag] %s — frame=%d fenceVal=%llu fenceCompleted=%llu "
        "fenceMatch=%d compositionActive=%d lastTickMs=%llu nowMs=%llu",
        reason ? reason : "freeze", frame, static_cast<unsigned long long>(fenceVal),
        static_cast<unsigned long long>(fenceCompleted), fenceVal == fenceCompleted ? 1 : 0, compositionActive ? 1 : 0,
        static_cast<unsigned long long>(lastTickMs), static_cast<unsigned long long>(GetTickCount64()));
    DumpFFXUiCompositeTimeline(reason);
    // Proxy-present driver + re-assert bracket state (defined later in this file / ffx_hook.cpp): shows in
    // one freeze dump whether the game-thread driver was live and whether a substitute re-assert was
    // in-flight (the historical presenter-thread deadlock signature).
    DX12_LogFFXProxyPresentHookFreezeDiagnostics(reason);
    FFXHook_LogSubstituteReRegFreezeDiagnostics(reason);
}

bool DX12_IsFFXUiResourceCompositionActive() {
    if (!g_FFXUiResourceCompositionActive.load(std::memory_order_acquire)) {
        return false;
    }
    // Recency-gated so it auto-disables once FG turns off and the per-frame UI-resource configures stop.
    const uint64_t last = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    return last != 0 && (GetTickCount64() - last) < 500;
}

// Cache the UI texture from RegisterUiResource for the per-present composite. The composite
// (DX12_CompositeOverlayOntoCachedFFXUiResource, driven from DetourPresent's no-callback FSR FG branch) draws
// CE's overlay onto the cached/CE-substituted UI texture on CE's OWN fenced queue. Gated to no-callback FSR
// FG only. Note: the call site in Hooked_ffxConfigure also caches during the VEH detection phase (before the
// no-callback flag is set) so the cache is populated before the VEH disarms.
bool DX12_ShouldCacheFFXUiResourceForBundle() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

// True if the UI texture has been cached from a RegisterUiResource call (for the VEH disarm condition).
bool DX12_IsFFXUiResourceCachedForBundle() {
    return g_CachedFFXUiTexture.load(std::memory_order_acquire) != nullptr;
}

// Direct read of the no-callback composition flag (for the VEH one-shot disarm logic in ffx_hook.cpp).
// Unlike DX12_IsFFXUiResourceCompositionActive (which is recency-gated), this returns the raw latched flag.
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

// True when the LIVE swapchain queue is the game's own original queue. During ACTIVE no-callback FSR FG the
// game presents on AMD's SEPARATE FfxFrameInterpolationSwapchain queue (g_SwapchainQueue != origGame); once
// FSR turns off and the game recreates a NATIVE swapchain it presents on its own queue again
// (g_SwapchainQueue == origGame). DetourPresent uses this as the reliable real-time signal that AMD's FG
// swapchain is gone — so a still-set no-callback latch is STALE (the off-signal was missed: ffxDestroyContext
// bypass / one-shot ffxConfigure VEH disarmed / preserved ownership), AMD is no longer compositing the UI
// texture, and the overlay must fall back to the (now-safe) backbuffer route. It is safe because the crash
// boundary is submitting on AMD's separate FG queue, which by definition != the original game queue.
bool DX12_IsLiveSwapchainQueueOriginalGameQueue() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_OriginalGameQueue != nullptr && dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue;
}

// True when native FSR FG has been explicitly DISABLED (ffxConfigure frameGenerationEnabled=0) while AMD's
// runtime-owned swapchain is still the live present path — i.e. a no-callback SUSPENSION (menu/loading), not
// full off. AMD keeps the swapchain but is NOT interpolating, so the ffxQuery interpolation-pacing wedge is
// inactive and separate overlay GPU work on the backbuffer is safe again (the documented suspension behavior).
// DetourPresent uses this to relax the crash-boundary skip during a suspension so the overlay is never blank
// if the bundle has a coverage gap. Cleared on the next enabled ffxConfigure (resume). This is an explicit
// configure-driven latch, not a heuristic runtime-mode read, so it does not flicker under active interpolation.
bool DX12_IsNativeFSRFGSuspendedDisablePending() {
    return dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
}

// RTV-compatible format helper for the FFX UI texture; defined below ReleaseFFXUiCompositeInfra and used by
// PrepareCEUiSubstituteTexture / DX12_PrepareFFXUiOverlayTarget / DX12_CompositeOverlayOntoFFXUiResource.
static DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);  // forward decl — defined below

// Set the texture the per-present composite draws the overlay onto next frame (either the game's own usable UI
// texture, or CE's substitute). needsTransparentClear is true only for CE's substitute (it is CE-owned and
// otherwise empty, so it must be cleared each frame); false for the game's own UI texture (blend on top of the
// HUD already present there). Diagnostic surfaces whether the registered texture is stable or rotates per
// frame (the post-VEH-disarm cache-freeze question).
static void SetBundleTargetTexture(ID3D12Resource* targetTexture, uint32_t ffxState, uint32_t flags,
                                   bool needsTransparentClear, const char* targetKind) {
    if (!targetTexture) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    // The game may rotate or release its registered UI resource immediately after ffxConfigure returns. Keep a
    // real cache reference until the next target replaces it; the prior raw pointer was a deterministic UAF.
    targetTexture->AddRef();
    ID3D12Resource* prev = g_CachedFFXUiTexture.exchange(targetTexture, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(ffxState, std::memory_order_release);
    g_CachedFFXUiFlags.store(flags, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(needsTransparentClear, std::memory_order_release);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);
    static std::atomic<uint64_t> s_uiCacheUpdateCount{0};
    const uint64_t n = s_uiCacheUpdateCount.fetch_add(1, std::memory_order_relaxed);
    const bool changed = prev != targetTexture;
    if (n < 20 || changed || (n % 600) == 0) {
        HookLogImportant(
            "DX12: FFX UI-composite target %s=%p (prev=%p changed=%d state=0x%X flags=0x%X clear=%d update=%llu)",
            targetKind ? targetKind : "tex", (void*)targetTexture, (void*)prev, changed ? 1 : 0, ffxState, flags,
            needsTransparentClear ? 1 : 0, (unsigned long long)(n + 1));
    }
    if (prev) {
        prev->Release();
    }
}

static bool IsResourceOwnedByDevice(ID3D12Resource* resource, ID3D12Device* expectedDevice) {
    if (!resource || !expectedDevice) {
        return false;
    }
    ID3D12Device* resourceDevice = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || !resourceDevice) {
        return false;
    }
    IUnknown* resourceIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    const bool same = SUCCEEDED(resourceDevice->QueryInterface(IID_PPV_ARGS(&resourceIdentity))) &&
                      SUCCEEDED(expectedDevice->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
                      resourceIdentity == expectedIdentity;
    if (resourceIdentity) {
        resourceIdentity->Release();
    }
    if (expectedIdentity) {
        expectedIdentity->Release();
    }
    resourceDevice->Release();
    return same;
}

// Create/resize CE's own backbuffer-sized UI texture (substituted into RegisterUiResource when the game's UI
// texture is degenerate). Created with ALLOW_RENDER_TARGET (the bundle draws the overlay onto it) in the same
// resource state AMD will read it in (mirrors the game's registered ffxState). Returns the texture or nullptr.
// Caller holds g_FFXUiCompositeMutex.
static ID3D12Resource* PrepareCEUiSubstituteTexture(ID3D12Device* device, uint32_t width, uint32_t height,
                                                    DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState) {
    if (!device || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }
    if (g_CEUiSubstituteTexture && IsResourceOwnedByDevice(g_CEUiSubstituteTexture, device) &&
        g_CEUiSubstituteWidth == width && g_CEUiSubstituteHeight == height && g_CEUiSubstituteFormat == format &&
        g_CEUiSubstituteInitialState == initialState) {
        g_CEUiSubstituteTexture->AddRef();
        return g_CEUiSubstituteTexture;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = FFXUiCompositeRtvFormat(format);  // typed RTV format (the bundle clears with this)
    ID3D12Resource* tex = nullptr;
    const HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &td, initialState, &clearValue,
                                                       IID_PPV_ARGS(&tex));
    if (FAILED(hr) || !tex) {
        HookLogImportant("DX12: FFX UI substitute texture creation FAILED (%ux%u fmt=%d hr=0x%08X)", width, height,
                         static_cast<int>(format), static_cast<unsigned>(hr));
        return nullptr;
    }
    tex->SetName(L"CE_FFXUiSubstituteTexture");
    HookLogImportant(
        "DX12: Prepared CE substitute UI texture %ux%u fmt=%d initState=0x%X; publication waits for successful "
        "FFX RegisterUiResource",
        width, height, static_cast<int>(format), static_cast<unsigned>(initialState));
    return tex;
}

bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags,
                                    ce::ffx_api::Resource* ceSubstitute,
                                    DX12FFXUiOverlayTargetPreparation* preparation) {
    auto* gameTex = static_cast<ID3D12Resource*>(gameUi.resource);
    if (!gameTex || !preparation) {
        return false;
    }
    *preparation = {};
    preparation->state = gameUi.state;
    preparation->flags = flags;
    preparation->sequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto stageGameTexture = [&]() {
        gameTex->AddRef();
        preparation->target = gameTex;
        preparation->substitute = false;
        preparation->clearTransparent = false;
    };

    // Authoritative geometry/format from the actual D3D12 resource (the FFX description can be partially filled);
    // fall back to the FFX description dims if the resource is not a usable 2D texture.
    uint32_t gameW = gameUi.description.width;
    uint32_t gameH = gameUi.description.height;
    DXGI_FORMAT gameFmt = DXGI_FORMAT_UNKNOWN;
    {
        const D3D12_RESOURCE_DESC gd = gameTex->GetDesc();
        if (gd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && gd.Width != 0 && gd.Height != 0) {
            gameW = static_cast<uint32_t>(gd.Width);
            gameH = gd.Height;
            gameFmt = gd.Format;
        }
    }
    const uint32_t bbW = dx12_hook_g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
    const uint32_t bbH = dx12_hook_g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);

    const auto target = ce::dx12_overlay_policy::ChooseFFXUiOverlayTarget(gameW, gameH, bbW, bbH);
    if (target == ce::dx12_overlay_policy::FFXUiOverlayTarget::kCompositeOntoGameTexture) {
        stageGameTexture();
        return false;
    }

    // Degenerate game UI texture (GTA's 1x1): substitute CE's own backbuffer-sized texture so AMD composites
    // the overlay. Requires known backbuffer geometry + a valid device; otherwise fall back to caching the game
    // texture (the bundle's degenerate guard then safely skips the draw — never a 1x1 garbage submit / crash).
    ID3D12Device* device = nullptr;
    gameTex->GetDevice(IID_PPV_ARGS(&device));
    const DXGI_FORMAT substituteFmt =
        (gameFmt != DXGI_FORMAT_UNKNOWN)
            ? gameFmt
            : static_cast<DXGI_FORMAT>(dx12_hook_g_NoCallbackBackbufferFormat.load(std::memory_order_acquire));
    if (!device || bbW == 0 || bbH == 0 || substituteFmt == DXGI_FORMAT_UNKNOWN || !ceSubstitute) {
        stageGameTexture();
        static std::atomic<int> s_substFallbackLog{0};
        const int n = s_substFallbackLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "DX12: FFX UI substitute UNAVAILABLE (device=%p bb=%ux%u fmt=%d) — degenerate game UI texture %ux%u "
                "kept as bundle target; bundle skips the draw rather than draw onto a placeholder (log=%d)",
                (void*)device, bbW, bbH, static_cast<int>(substituteFmt), gameW, gameH, n + 1);
        }
        if (device) {
            device->Release();
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    const D3D12_RESOURCE_STATES initialState = GetDX12StateFromFFXResourceState(gameUi.state);
    // COMMON/PRESENT is legitimately numeric zero. Never truth-test D3D12_RESOURCE_STATES: creating in a
    // different fallback state while forwarding/caching COMMON would make the first owner-queue barrier's
    // StateBefore false and can remove the device.
    ID3D12Resource* ceTex = PrepareCEUiSubstituteTexture(device, bbW, bbH, substituteFmt, initialState);
    device->Release();
    if (!ceTex) {
        stageGameTexture();
        return false;
    }

    // Mirror the game's description/state so AMD treats CE's texture identically; override only geometry + ptr.
    *ceSubstitute = gameUi;
    ceSubstitute->resource = ceTex;
    ceSubstitute->description.width = bbW;
    ceSubstitute->description.height = bbH;

    preparation->target = ceTex;
    preparation->substitute = true;
    preparation->clearTransparent = true;
    preparation->width = bbW;
    preparation->height = bbH;
    preparation->format = substituteFmt;
    preparation->initialState = initialState;
    return true;
}

void DX12_DiscardFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation) {
        return;
    }
    if (preparation->target) {
        preparation->target->Release();
    }
    *preparation = {};
}

void DX12_CommitFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation || !preparation->target) {
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
        if (preparation->sequence < g_FFXUiCommittedPreparationSequence) {
            HookLogImportant(
                "DX12: Discarding stale successful FFX UI registration commit (sequence=%llu committed=%llu)",
                static_cast<unsigned long long>(preparation->sequence),
                static_cast<unsigned long long>(g_FFXUiCommittedPreparationSequence));
        } else {
            g_FFXUiCommittedPreparationSequence = preparation->sequence;
            if (preparation->substitute && g_CEUiSubstituteTexture != preparation->target) {
                preparation->target->AddRef();
                ID3D12Resource* oldSubstitute = g_CEUiSubstituteTexture;
                g_CEUiSubstituteTexture = preparation->target;
                g_CEUiSubstituteWidth = preparation->width;
                g_CEUiSubstituteHeight = preparation->height;
                g_CEUiSubstituteFormat = preparation->format;
                g_CEUiSubstituteInitialState = preparation->initialState;
                if (oldSubstitute) {
                    oldSubstitute->Release();
                }
            }

            SetBundleTargetTexture(preparation->target, preparation->state, preparation->flags,
                                   preparation->clearTransparent,
                                   preparation->substitute ? "ce-substitute-tex" : "game-tex");
            static std::atomic<int> s_commitLog{0};
            const int n = s_commitLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 300) == 0) {
                HookLogImportant(
                    "DX12: Committed accepted FFX UI target %p (substitute=%d %ux%u fmt=%d state=0x%X "
                    "sequence=%llu log=%d)",
                    preparation->target, preparation->substitute ? 1 : 0, preparation->width, preparation->height,
                    static_cast<int>(preparation->format), preparation->state,
                    static_cast<unsigned long long>(preparation->sequence), n + 1);
            }
        }
    }
    DX12_DiscardFFXUiOverlayTarget(preparation);
}

void DX12_NoteFfxConfigureForward(uint64_t configureType) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_LastFfxConfigureForwardQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_relaxed);
    const uint64_t frame = g_FfxConfigureFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log RegisterUiResource (type=0x30002) calls with frame context so the FG-configure (0x20002) and
    // UI-register (0x30002) streams are correlatable per frame in the freeze dump.
    if (configureType == ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12) {
        static std::atomic<int> s_registerUiResLogCount{0};
        const int n = s_registerUiResLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant("FFX Hook: RegisterUiResource forwarded (type=0x30002 cfgFrame=%llu qpc=%llu log=%d)",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(qpc.QuadPart),
                             n + 1);
        }
    }
}

static void ReleaseFFXUiCompositeInfra() {
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
    // Invalidate any RegisterUiResource preparation that entered before teardown and has not returned from
    // AMD yet. Its later commit must not resurrect device-bound resources into the cleared generation.
    g_FFXUiCommittedPreparationSequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_FFXUiPresenterFallbackLastSequence = 0;
    if (g_FFXUiCompositeList) {
        g_FFXUiCompositeList->Release();
        g_FFXUiCompositeList = nullptr;
    }
    for (auto& a : g_FFXUiCompositeAlloc) {
        if (a) {
            a->Release();
            a = nullptr;
        }
    }
    if (g_FFXUiCompositeRtvHeap) {
        g_FFXUiCompositeRtvHeap->Release();
        g_FFXUiCompositeRtvHeap = nullptr;
    }
    if (dx12_hook_g_FFXUiCompositeQueue) {
        dx12_hook_g_FFXUiCompositeQueue->Release();
        dx12_hook_g_FFXUiCompositeQueue = nullptr;
    }
    if (dx12_hook_g_FFXUiCompositeFence) {
        dx12_hook_g_FFXUiCompositeFence->Release();
        dx12_hook_g_FFXUiCompositeFence = nullptr;
    }
    dx12_hook_g_FFXUiCompositeFenceVal = 0;
    for (int i = 0; i < kFFXUiCompositeSlotCount; ++i) {
        g_FFXUiCompositeAllocFenceVal[i] = 0;
    }
    dx12_hook_g_FFXUiCompositeFrame = 0;
    g_FFXUiCompositeTimelineIdx.store(0, std::memory_order_relaxed);
    g_LastFfxConfigureForwardQpc.store(0, std::memory_order_relaxed);
    g_FfxConfigureFrame.store(0, std::memory_order_relaxed);
    ID3D12Resource* cachedTexture = g_CachedFFXUiTexture.exchange(nullptr, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(0, std::memory_order_release);
    g_CachedFFXUiFlags.store(0, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(false, std::memory_order_release);
    if (cachedTexture) {
        cachedTexture->Release();
    }
    // Stop the per-present substitute re-registration: the stored desc's resource pointer (CE's substitute)
    // is about to dangle. ffx_hook re-stores it on the next RegisterUiResource substitution.
    FFXHook_ClearSubstituteUiReRegistration();
    // Release CE's own substitute UI texture (degenerate-game-texture path). Released here on teardown / device
    // change only — never from the per-frame composite path (that would blank the overlay every frame).
    if (g_CEUiSubstituteTexture) {
        g_CEUiSubstituteTexture->Release();
        g_CEUiSubstituteTexture = nullptr;
    }
    g_CEUiSubstituteWidth = 0;
    g_CEUiSubstituteHeight = 0;
    g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;
    g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;
}

// Pick an RTV-compatible (non-TYPELESS) format for the UI texture so CreateRenderTargetView succeeds.
// Keep sRGB views as-is (the overlay then writes through the game's own gamma expectation).
static DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat) {
    switch (texFormat) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return texFormat;
    }
}

// Draw the inject overlay onto the game's registered FFX UI texture. Submits on CE's OWN dedicated queue
// (g_FFXUiCompositeQueue, NOT the game queue or AMD's runtime present queue) so AMD's presenter pacing is
// never perturbed. Signals the fence on CE's own queue and CPU-waits for completion before returning, so
// the caller (Hooked_ffxConfigure) forwards RegisterUiResource only after CE's overlay write is GPU-complete.
// Returns true if recorded.
bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr, uint32_t ffxState, uint32_t flags) {
    if (!uiResourcePtr) {
        return false;
    }
    auto* uiTexture = static_cast<ID3D12Resource*>(uiResourcePtr);

    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> qlock(g_CommandQueueMutex);
        gameQueue = dx12_hook_g_OriginalGameQueue;  // used for node-mask + overlay-adapter init, NOT for submit
    }
    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    if (!gameQueue || !device) {

        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);

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
        const uint32_t bbW = dx12_hook_g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
        const uint32_t bbH = dx12_hook_g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);
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
        if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
            dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
        }
        dx12_hook_g_FFXPresentOverlayDevice = nullptr;
        dx12_hook_g_FFXPresentOverlayFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;
        g_FFXUiCompositeDevice = device;
        return false;
    }

    // Lazily build the dedicated CE-queue submission infra (separate from the present-thread overlay and
    // separate from g_State.overlayQueue whose lifecycle is tied to FG state transitions).
    if (!dx12_hook_g_FFXUiCompositeFence) {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12_hook_g_FFXUiCompositeFence)))) {
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
    if (!dx12_hook_g_FFXUiCompositeQueue) {
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (gameQueue) {
            qDesc.NodeMask = gameQueue->GetDesc().NodeMask;
        }
        if (FAILED(device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&dx12_hook_g_FFXUiCompositeQueue)))) {
            HookLogImportant(
                "DX12: FFX UI-composite FAILED to create dedicated CE queue — refusing the known-wedging "
                "game/runtime-queue fallback");
            dx12_hook_g_FFXUiCompositeQueue = nullptr;
            return false;
        } else {
            dx12_hook_g_FFXUiCompositeQueue->SetName(L"CE_FFXUiCompositeQueue");
            HookLogImportant("DX12: FFX UI-composite dedicated CE queue created (ptr=%p gameQueue=%p)",
                             dx12_hook_g_FFXUiCompositeQueue, gameQueue);
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
        std::lock_guard<std::recursive_mutex> olock(dx12_hook_g_OverlayMutex);
        if (!dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized() || g_FFXUiCompositeAdapterFormat != rtvFormat) {
            if (dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
                dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
            }
            dx12_hook_g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
            if (!dx12_hook_g_FFXPresentOverlayAdapter.InitDX12(device, gameQueue, static_cast<int>(rtvFormat))) {
                HookLogImportant("DX12: FFX UI-composite failed to init overlay adapter (fmt=%d)",
                                 static_cast<int>(rtvFormat));
                return false;
            }
            const bool uiTargetHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(rtvFormat);
            dx12_hook_g_FFXPresentOverlayAdapter.SetHDR(uiTargetHDR, static_cast<int>(rtvFormat));
            dx12_hook_g_FFXPresentOverlayDevice = device;
            dx12_hook_g_FFXPresentOverlayFormat = rtvFormat;
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
    ID3D12CommandQueue* submitQueue = dx12_hook_g_FFXUiCompositeQueue;
    if (!submitQueue) {
        return false;
    }
    const int slot = dx12_hook_g_FFXUiCompositeFrame % kFFXUiCompositeSlotCount;
    // Recycle the allocator slot by fence value (signaled on CE's own queue). Fast path: already complete.
    if (dx12_hook_g_FFXUiCompositeFence && dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() < g_FFXUiCompositeAllocFenceVal[slot]) {
        HookLogImportant(
            "DX12: FFX UI-composite refused in-flight allocator reuse (slot=%d guard=%llu completed=%llu) — "
            "prior completion proof is missing; no wait/overwrite",
            slot, static_cast<unsigned long long>(g_FFXUiCompositeAllocFenceVal[slot]),
            static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence->GetCompletedValue()));
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
                slot, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned>(allocResetHr), static_cast<unsigned>(listResetHr));
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
        std::lock_guard<std::recursive_mutex> olock(dx12_hook_g_OverlayMutex);
        dx12_hook_g_FFXPresentOverlayAdapter.SetIPCClient(g_IPC);
        dx12_hook_g_FFXPresentOverlayAdapter.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            dx12_hook_g_FFXPresentOverlayAdapter.SetMetrics(perf);
        }
        dx12_hook_g_FFXPresentOverlayAdapter.SetGraphicsAPI("DX12");
        dx12_hook_g_FFXPresentOverlayAdapter.SetDX12RenderTarget(g_FFXUiCompositeList, reinterpret_cast<void*>(rtv.ptr));
        dx12_hook_g_FFXPresentOverlayAdapter.RenderOverlay(width, height);
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
    ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
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
    ++dx12_hook_g_FFXUiCompositeFenceVal;
    const HRESULT signalHr = submitQueue->Signal(dx12_hook_g_FFXUiCompositeFence, dx12_hook_g_FFXUiCompositeFenceVal);
    if (FAILED(signalHr)) {
        g_FFXUiCompositeAllocFenceVal[slot] = UINT64_MAX;
        HookLogImportant(
            "DX12: FFX UI-composite completion Signal FAILED (queue=%p frame=%d value=%llu hr=0x%08X) — "
            "overlay not published and allocator permanently quarantined",
            submitQueue, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
            static_cast<unsigned>(signalHr));
        return false;
    }
    g_FFXUiCompositeAllocFenceVal[slot] = dx12_hook_g_FFXUiCompositeFenceVal;
    ++dx12_hook_g_FFXUiCompositeFrame;
    // Step 2 revised: CPU-wait for CE's overlay write to complete on CE's own queue before returning, so
    // the caller (Hooked_ffxConfigure) forwards RegisterUiResource only after the overlay is GPU-complete.
    // This is a deterministic fence completion wait (not a sleep/poll), on the ffxConfigure thread (which is
    // already blocked calling ffxConfigure), not the game's ECL thread. Wait duration = overlay draw time (<1ms).
    uint32_t waitTimedOut = 0;
    if (g_FFXUiCompositeFenceEvent && dx12_hook_g_FFXUiCompositeFence &&
        dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() < dx12_hook_g_FFXUiCompositeFenceVal) {
        const HRESULT eventHr =
            dx12_hook_g_FFXUiCompositeFence->SetEventOnCompletion(dx12_hook_g_FFXUiCompositeFenceVal, g_FFXUiCompositeFenceEvent);
        const DWORD waitResult =
            SUCCEEDED(eventHr) ? WaitForSingleObject(g_FFXUiCompositeFenceEvent, INFINITE) : WAIT_FAILED;
        if (waitResult != WAIT_OBJECT_0) {
            waitTimedOut = 1;
            HookLogImportant(
                "DX12: FFX UI-composite completion proof FAILED (frame=%d fenceVal=%llu completed=%llu "
                "setEventHr=0x%08X wait=%lu) — overlay not published/re-registered",
                dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
                static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence->GetCompletedValue()),
                static_cast<unsigned>(eventHr), waitResult);
            return false;
        }
    }
    LARGE_INTEGER returnQpc;
    QueryPerformanceCounter(&returnQpc);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);

    // Record this composite call in the timeline ring buffer for freeze diagnosis.
    const int gameEclCount = dx12_hook_g_CommandListsExecutedThisFrame.load(std::memory_order_relaxed);
    RecordFFXUiCompositeTimelineEntry(
        {static_cast<uint64_t>(dx12_hook_g_FFXUiCompositeFrame), dx12_hook_g_FFXUiCompositeFenceVal,
         dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0,
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
            submitQueue, uiTexture, dx12_hook_g_FFXUiCompositeFrame, static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFenceVal),
            static_cast<unsigned long long>(dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0),
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
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
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

    }
    if (descriptorDevice) {
        descriptorDevice->Release();
    }
    if (underlyingDevice) {
        underlyingDevice->Release();
    }
    return {selectedQueue, route};
}

static bool SubmitNativeFSROwnerQueueOverlayCommandList(ID3D12CommandQueue* queue, ID3D12CommandList* commandList) {
    if (!queue || !commandList) {
        return false;
    }
    ID3D12CommandList* lists[] = {commandList};
    ScopedCEOverlayECLSubmission ceOverlayECLGuard("ffx-owner-queue");
    queue->ExecuteCommandLists(1, lists);
    return true;
}

static HRESULT SignalNativeFSROwnerQueueOverlayFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    return queue && fence ? queue->Signal(fence, value) : E_INVALIDARG;
}

// During a runtime-owned no-callback FSR suspension or disabled protected startup arming, AMD presents the
// replacement backbuffer 1:1 and does not composite the registered UI resource. Draw directly onto that buffer
// immediately before the proxy Present, on the target-compatible owner queue resolved from the FFX descriptor.
// The SDK orders that queue into its internal present queue, so no foreign queue, cross-queue race, copy, or
// per-frame CPU wait exists.
bool DX12_CompositeOverlayOntoSuspendBackbuffer(IDXGISwapChain* proxy, const char* routeName) {
    IDXGISwapChain3* swapChain3 = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    if (proxy && SUCCEEDED(proxy->QueryInterface(IID_PPV_ARGS(&swapChain3))) && swapChain3) {
        swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
        swapChain3->Release();
    }
    const AcquiredNativeFSROwnerQueue ownerQueue = AcquireNativeFSRSwapchainPresentationQueue(proxy, backBuffer);
    if (!ownerQueue.queue || !backBuffer) {
        static std::atomic<int> s_missingBindingLogCount{0};
        const int logCount = s_missingBindingLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR proxy-backbuffer overlay refused work because proxy %p has no target-compatible "
                "FFX owner queue/backbuffer (route=%s queue=%p backbuffer=%p log=%d)",
                proxy, routeName && routeName[0] ? routeName : "unknown", ownerQueue.queue, backBuffer, logCount + 1);
        }
        if (ownerQueue.queue) {
            ownerQueue.queue->Release();
        }
        if (backBuffer) {
            backBuffer->Release();
        }
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    bool hdr = false;
    if (SUCCEEDED(proxy->GetDesc(&desc))) {
        hdr =
            ResolveSwapchainOutputHDRState(proxy, desc.BufferDesc.Format, "DX12: FSR proxy-backbuffer owner-queue HDR");
    }

    ce::dx12_ffx_suspend_overlay::RenderRequest request = {};
    request.proxySwapChain = proxy;
    request.presentationQueue = ownerQueue.queue;
    request.targetResource = backBuffer;
    request.routeName = routeName && routeName[0] ? routeName : "proxy-backbuffer";
    request.submitCommandList = &SubmitNativeFSROwnerQueueOverlayCommandList;
    request.signalFence = &SignalNativeFSROwnerQueueOverlayFence;
    request.hdr = hdr;
    const bool rendered = ce::dx12_ffx_suspend_overlay::Render(request);
    backBuffer->Release();
    ownerQueue.queue->Release();

    if (rendered) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }
    return rendered;
}

// Active no-callback FSR FG consumes the registered UI resource from the same FFX swapchain pipeline. Record
// CE's UI-resource draw on the target-compatible owner queue immediately before proxy Present. That gives AMD
// an explicit queue-order dependency without a foreign queue, a cross-queue resource race, or a per-frame CPU
// completion wait. A retained local reference plus the renderer slot pins rotating resources through submission.
static bool DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(IDXGISwapChain* proxy) {
    ID3D12Resource* uiTexture = nullptr;
    uint32_t ffxState = 0;
    uint32_t flags = 0;
    bool isSubstitute = false;
    bool needsTransparentClear = false;
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
        uiTexture = g_CachedFFXUiTexture.load(std::memory_order_acquire);
        if (uiTexture) {
            uiTexture->AddRef();
            ffxState = g_CachedFFXUiState.load(std::memory_order_acquire);
            flags = g_CachedFFXUiFlags.load(std::memory_order_acquire);
            isSubstitute = g_CEUiSubstituteTexture && uiTexture == g_CEUiSubstituteTexture;
            needsTransparentClear = g_BundleTargetNeedsTransparentClear.load(std::memory_order_acquire);
        }
    }
    if (!uiTexture) {
        return false;
    }

    const AcquiredNativeFSROwnerQueue ownerQueue = AcquireNativeFSRSwapchainPresentationQueue(proxy, uiTexture);
    if (!ownerQueue.queue) {
        static std::atomic<int> s_missingBindingLogCount{0};
        const int logCount = s_missingBindingLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR active UI-resource overlay has no target-compatible proxy owner queue "
                "(proxy=%p target=%p substitute=%d log=%d); owner-queue route unavailable",
                proxy, uiTexture, isSubstitute ? 1 : 0, logCount + 1);
        }
        // A CE-owned substitute has no incoming game-queue writer, so the legacy completion-waited route
        // remains a safe compatibility fallback for an unknown FFX descriptor revision. A game-owned UI
        // texture cannot use this fallback here because a foreign-queue write would race its producer.
        const bool rendered = isSubstitute && DX12_CompositeOverlayOntoFFXUiResource(uiTexture, ffxState, flags);
        uiTexture->Release();
        return rendered;
    }

    ce::dx12_ffx_suspend_overlay::RenderRequest request = {};
    request.proxySwapChain = proxy;
    request.presentationQueue = ownerQueue.queue;
    request.targetResource = uiTexture;
    request.targetState = GetDX12StateFromFFXResourceState(ffxState);
    request.clearTransparent = needsTransparentClear;
    request.routeName =
        ownerQueue.route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue
            ? "active-ui-resource-streamline-unwrapped"
            : "active-ui-resource";
    request.submitCommandList = &SubmitNativeFSROwnerQueueOverlayCommandList;
    request.signalFence = &SignalNativeFSROwnerQueueOverlayFence;
    request.hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(uiTexture->GetDesc().Format);
    const bool rendered = ce::dx12_ffx_suspend_overlay::Render(request);
    ownerQueue.queue->Release();
    uiTexture->Release();

    return rendered;
}

static std::mutex g_FFXProxyPresentHookMutex;

static void* g_FFXProxySwapchain = nullptr;             // game-facing proxy object (identity/diagnostics only)

static void** g_FFXProxyPresentVtableEntry = nullptr;   // &vtable[8] patched (class vtable in the FFX module)

static void** g_FFXProxyPresent1VtableEntry = nullptr;  // &vtable[22] patched (nullptr if slot unavailable)

static std::atomic<PFN_FFXProxyPresent> g_FFXProxyPresentOriginal{nullptr};

static std::atomic<PFN_FFXProxyPresent1> g_FFXProxyPresent1Original{nullptr};

static std::atomic<bool> g_FFXProxyPresentHookInstalled{false};

static std::atomic<uint64_t> g_FFXProxyPresentHookInstallQpc{0};

static std::atomic<uint64_t> g_FFXProxyPreworkCount{0};

static std::atomic<uint64_t> g_FFXProxyPreworkLastQpc{0};

static std::atomic<uint32_t> g_FFXProxyPreworkLastTid{0};

// Detours can already have been fetched from the class vtable when it is restored. Quiescing prevents any
// late entrant from touching CE renderer state, while the epoch counter lets teardown drain callbacks that
// entered before quiescing. Forward targets remain published until process exit.
static std::atomic<bool> g_FFXProxyPresentQuiescing{true};

static std::atomic<uint32_t> g_FFXProxyPresentDetoursInFlight{0};

static std::mutex g_FFXProxyPresentDrainMutex;

static std::condition_variable g_FFXProxyPresentDrainCV;

// Set while the current thread is inside the proxy-present prework — the ONLY context allowed to call the
// substitute re-assert (FFXHook_ReRegisterSubstituteUiResource hard-refuses without it; deadlock boundary).
static thread_local bool t_InsideFFXProxyPresentPrework = false;

// Present1 in FFX 3.1 delegates to Present. Keep the depth set across the original call so that nested
// vtable dispatch forwards normally but does not composite the same replacement buffer twice.
static thread_local uint32_t t_FFXProxyPresentDetourDepth = 0;

bool DX12_IsFFXProxyPresentHookInstalled() {
    return g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire);
}

bool DX12_IsCurrentThreadInsideFFXProxyPresentPrework() {
    return t_InsideFFXProxyPresentPrework;
}

// True while the proxy-present prework is the LIVE composite driver: hook installed AND the game is
// actually presenting through the hooked proxy (prework observed recently, or the hook was installed
// moments ago and the first proxy present is still on its way). DetourPresent's kSkipBundleCovers arm
// consults this so the presenter-thread fallback composite resumes — visibly, loudly — if the proxy hook
// ever goes quiet (wrong object hooked / game bypasses the proxy), instead of silently blanking the overlay.
bool DX12_IsFFXProxyPresentHookDriving() {
    if (!g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        return false;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t lastPrework = g_FFXProxyPreworkLastQpc.load(std::memory_order_acquire);
    if (!lastPrework || freq.QuadPart <= 0) {
        // Until the detour has actually performed prework, keep the real-present fallback alive. Treating a
        // freshly patched-but-never-entered proxy as the live driver creates a deterministic first-frame gap.
        return false;
    }
    const double ageMs = static_cast<double>(now.QuadPart - lastPrework) * 1000.0 / static_cast<double>(freq.QuadPart);
    return ageMs < 1000.0;
}

// Per-present prework on the GAME thread, before AMD's proxy Present runs. Mirrors DetourPresent's
// composite-route decision so both drivers can never disagree about WHEN the composite should run — only
// WHERE it runs differs.
static void DX12_RunFFXProxyPrePresentWork(IDXGISwapChain* proxy, const char* entryPoint) {
    const bool nativeNoCallbackCompositionActive = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
    const bool protectedStartupBackbufferRoute =
        ce::dx12_overlay_policy::ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(
            dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
            HasResolvedOfficialFFXStartupPath(), DX12_IsFFXProxyPresentHookInstalled());
    if (!nativeNoCallbackCompositionActive && !protectedStartupBackbufferRoute) {
        return;
    }
    if (!protectedStartupBackbufferRoute) {
        const bool runtimeOwnsSwapchain =
            DXGIShared::DoesFGRuntimeOwnSwapchain() || HookHasRuntimeOwnedNativeFGPresentPath();
        const auto route = ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute(
            runtimeOwnsSwapchain, DX12_IsLiveSwapchainQueueOriginalGameQueue(),
            DX12_IsNativeFSRFGSuspendedDisablePending(), DX12_IsFFXUiResourceCachedForBundle(),
            /*bundleOverlayActivelyFiring=*/false);
        if (route != ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute::kSkipBundleCovers) {
            return;
        }
    }

    t_InsideFFXProxyPresentPrework = true;
    auto preworkScope = ce::make_scope_guard([]() { t_InsideFFXProxyPresentPrework = false; });
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_FFXProxyPreworkLastTid.store(GetCurrentThreadId(), std::memory_order_release);
    const uint64_t preworkNum = g_FFXProxyPreworkCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Active FG composites onto the cached/substituted UI texture before re-asserting its registration.
    // Both are game-thread-safe here: AMD's criticalSection is NOT held by this thread yet (Present enters it
    // after we forward), so the re-assert follows the exact lock order of the game's own per-frame register.
    //
    // BACKBUFFER EXCEPTIONS: disabled configure suspension and protected startup arming both present the
    // proxy backbuffer 1:1 without consuming the registered UI resource. Draw directly on that backbuffer via
    // the target-compatible FFX owner queue. Queue order guarantees game draw -> CE overlay -> AMD's internal
    // gameFence handoff -> Present, without the staged internal present queue or a CPU wait. The substitute
    // re-assert stays skipped because AMD is not consuming the UI resource in either passthrough state.
    bool composited;
    const bool suspendBackbufferRoute = !protectedStartupBackbufferRoute && DX12_IsNativeFSRFGSuspendedDisablePending();
    const bool proxyBackbufferRoute = protectedStartupBackbufferRoute || suspendBackbufferRoute;
    if (proxyBackbufferRoute) {
        composited = DX12_CompositeOverlayOntoSuspendBackbuffer(
            proxy, protectedStartupBackbufferRoute ? "protected-startup-backbuffer" : "suspend-backbuffer");
    } else {
        composited = DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(proxy);
        if (composited) {
            const auto reRegistration = FFXHook_ReRegisterSubstituteUiResource();
            if (reRegistration == FFXSubstituteUiReRegistrationResult::kFailed) {
                composited = false;
            }
        } else {
            static std::atomic<int> s_reassertSuppressedLogCount{0};
            const int logCount = s_reassertSuppressedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: FFX proxy prework did not re-register the substitute UI resource because the "
                    "overlay composite lacked owner-queue submission proof (log=%d)",
                    logCount + 1);
            }
        }
    }
    static std::atomic<void*> s_lastPreworkRouteProxy{nullptr};
    static std::atomic<int> s_lastPreworkRoute{-1};
    const int currentPreworkRoute = protectedStartupBackbufferRoute ? 2 : (suspendBackbufferRoute ? 1 : 0);
    void* previousPreworkProxy = s_lastPreworkRouteProxy.exchange(proxy, std::memory_order_acq_rel);
    const int previousPreworkRoute = s_lastPreworkRoute.exchange(currentPreworkRoute, std::memory_order_acq_rel);
    if (previousPreworkProxy != proxy || previousPreworkRoute != currentPreworkRoute) {
        HookLogImportant(
            "DX12: FFX proxy overlay route transition %s -> %s at prework #%llu (proxy=%p composited=%d) — "
            "the first present after the configure transition selected the new target",
            previousPreworkProxy != proxy || previousPreworkRoute < 0
                ? "uninitialized"
                : (previousPreworkRoute == 2
                       ? "protected-startup-backbuffer"
                       : (previousPreworkRoute == 1 ? "suspend-backbuffer" : "active-ui-resource")),
            protectedStartupBackbufferRoute ? "protected-startup-backbuffer"
                                            : (suspendBackbufferRoute ? "suspend-backbuffer" : "active-ui-resource"),
            static_cast<unsigned long long>(preworkNum), (void*)proxy, composited ? 1 : 0);
    }
    // A merely-entered detour is not coverage. Publish the live-driver heartbeat only after the command list
    // was submitted; otherwise immediately reactivate the real-present fallback for this same transition.
    g_FFXProxyPreworkLastQpc.store(composited ? static_cast<uint64_t>(qpc.QuadPart) : 0, std::memory_order_release);
    if (composited && !proxyBackbufferRoute) {
        g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
        g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }

    static std::atomic<int> s_preworkLog{0};
    const int n = s_preworkLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 600) == 0) {
        HookLogImportant(
            "DX12: FFX proxy-present prework #%llu via %s (proxy=%p tid=0x%04X composited=%d route=%s) — composite "
            "on the GAME thread before AMD's Present (log=%d)",
            static_cast<unsigned long long>(preworkNum), entryPoint ? entryPoint : "Present", (void*)proxy,
            GetCurrentThreadId(), composited ? 1 : 0,
            protectedStartupBackbufferRoute ? "protected-startup-backbuffer"
                                            : (suspendBackbufferRoute ? "suspend-backbuffer" : "ui-resource"),
            n + 1);
    }
}

static HRESULT STDMETHODCALLTYPE DX12_FFXProxyDetourPresent(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    g_FFXProxyPresentDetoursInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard = ce::make_scope_guard([]() {
        if (g_FFXProxyPresentDetoursInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            g_FFXProxyPresentDrainCV.notify_all();
        }
    });
    PFN_FFXProxyPresent original = g_FFXProxyPresentOriginal.load(std::memory_order_acquire);
    if (!original) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const bool outermost = t_FFXProxyPresentDetourDepth++ == 0;
    auto depthGuard = ce::make_scope_guard([&]() { --t_FFXProxyPresentDetourDepth; });
    if (outermost && !g_FFXProxyPresentQuiescing.load(std::memory_order_acquire)) {
        DX12_RunFFXProxyPrePresentWork(self, "Present");
    }
    return original(self, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE DX12_FFXProxyDetourPresent1(IDXGISwapChain* self, UINT SyncInterval, UINT Flags,
                                                             const DXGI_PRESENT_PARAMETERS* pParams) {
    g_FFXProxyPresentDetoursInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard = ce::make_scope_guard([]() {
        if (g_FFXProxyPresentDetoursInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            g_FFXProxyPresentDrainCV.notify_all();
        }
    });
    PFN_FFXProxyPresent1 original = g_FFXProxyPresent1Original.load(std::memory_order_acquire);
    if (!original) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const bool outermost = t_FFXProxyPresentDetourDepth++ == 0;
    auto depthGuard = ce::make_scope_guard([&]() { --t_FFXProxyPresentDetourDepth; });
    if (outermost && !g_FFXProxyPresentQuiescing.load(std::memory_order_acquire)) {
        DX12_RunFFXProxyPrePresentWork(self, "Present1");
    }
    return original(self, SyncInterval, Flags, pParams);
}

// Resolve the module owning an address (no refcount change). Returns nullptr when unresolvable.
static HMODULE ModuleFromAddress(const void* address) {
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

static void DX12_RemoveFFXProxyPresentHookLocked(const char* reason);  // defined below

bool DX12_TryInstallFFXProxyPresentHook(void* swapChain, void* ffxRuntimeAnchor, const char* source) {
    if (!swapChain || !ffxRuntimeAnchor) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_FFXProxyPresentHookMutex);
    if (!IsReadableSwapchainPointer(swapChain) || !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)swapChain))) {
        static std::atomic<int> s_unreadableLog{0};
        if (s_unreadableLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant("DX12: FFX proxy-present hook skipped — unreadable swapchain %p (source=%s)", swapChain,
                             source ? source : "?");
        }
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!IsReadableSwapchainPointer(reinterpret_cast<const void*>(&vtable[8]))) {
        return false;
    }
    void* presentEntry = vtable[8];
    const HMODULE ffxModule = ModuleFromAddress(ffxRuntimeAnchor);
    const HMODULE presentModule = ModuleFromAddress(presentEntry);
    const HMODULE ceModule = ModuleFromAddress(reinterpret_cast<const void*>(&DX12_TryInstallFFXProxyPresentHook));
    const bool presentEntryInFFXRuntimeModule = ffxModule != nullptr && presentModule == ffxModule;
    const bool presentEntryIsCEDetour = presentModule != nullptr && presentModule == ceModule;
    // "Already installed" requires the tracked ENTRY ADDRESS *and* the live entry VALUE to match CE's
    // detour: an FFX module unload+reload at the same base leaves the address equal but the fresh vtable
    // unpatched — that must be treated as a new install, not silently trusted.
    const bool alreadyInstalledOnThisVtableEntry = g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire) &&
                                                   g_FFXProxyPresentVtableEntry == &vtable[8] &&
                                                   presentEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent);
    if (alreadyInstalledOnThisVtableEntry) {
        // Same class vtable already routed to CE (e.g. FG re-enable with a fresh proxy of the same class):
        // just refresh the tracked object identity for diagnostics.
        if (g_FFXProxySwapchain != swapChain) {
            HookLogImportant(
                "DX12: FFX proxy-present hook retained across proxy object change (old=%p new=%p source=%s)",
                g_FFXProxySwapchain, swapChain, source ? source : "?");
            g_FFXProxySwapchain = swapChain;
            // Do not let a successful heartbeat from the old object suppress the real-present fallback before
            // the replacement proxy has delivered its own first covered Present.
            g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
        }
        return true;
    }
    if (!ce::dx12_overlay_policy::ShouldInstallFFXProxyPresentHook(presentEntryInFFXRuntimeModule,
                                                                   presentEntryIsCEDetour, false)) {
        static std::atomic<int> s_rejectLog{0};
        const int n = s_rejectLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "DX12: FFX proxy-present hook NOT installed — Present entry %p not in the FFX runtime module "
                "(sc=%p presentModule=%p ffxModule=%p ceDetour=%d source=%s log=%d); composite stays on the "
                "real-present fallback driver (no substitute re-assert there — deadlock boundary)",
                presentEntry, swapChain, (void*)presentModule, (void*)ffxModule, presentEntryIsCEDetour ? 1 : 0,
                source ? source : "?", n + 1);
        }
        return false;
    }
    // Keep the presenter-thread fallback enabled throughout a class-vtable handoff. Late entrants from the
    // previous detour only forward while quiescing is set; the new detour becomes authoritative after its
    // first observed prework.
    g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
    g_FFXProxyPresentQuiescing.store(true, std::memory_order_release);
    if (g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        // Different class vtable (new FFX runtime module / different proxy class): unhook the old entries
        // first so exactly one proxy vtable is ever patched.
        DX12_RemoveFFXProxyPresentHookLocked("new proxy class vtable");
    }
    // Publish the validated original before patching the shared class vtable. Another proxy instance can call
    // Present immediately after the slot changes; it must never observe CE's detour with a null forward target.
    g_FFXProxyPresentOriginal.store(reinterpret_cast<PFN_FFXProxyPresent>(presentEntry), std::memory_order_release);
    void* originalPresent = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[8]), reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent), &originalPresent);
    if (status != VTableHook::Success || !originalPresent) {
        HookLogImportant("DX12: FFX proxy-present vtable hook FAILED (%s) for sc=%p entry=%p source=%s",
                         VTableHook::StatusToString(status), swapChain, (void*)&vtable[8], source ? source : "?");
        return false;
    }
    g_FFXProxyPresentOriginal.store(reinterpret_cast<PFN_FFXProxyPresent>(originalPresent), std::memory_order_release);
    g_FFXProxyPresentVtableEntry = &vtable[8];
    // Present1 (IDXGISwapChain1 slot 22) — hook when the slot exists and also resolves into the FFX module.
    g_FFXProxyPresent1VtableEntry = nullptr;
    if (IsReadableSwapchainPointer(reinterpret_cast<const void*>(&vtable[22])) && ModuleFromAddress(vtable[22]) == ffxModule) {
        g_FFXProxyPresent1Original.store(reinterpret_cast<PFN_FFXProxyPresent1>(vtable[22]), std::memory_order_release);
        void* originalPresent1 = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[22]), reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent1), &originalPresent1) ==
                VTableHook::Success &&
            originalPresent1) {
            g_FFXProxyPresent1Original.store(reinterpret_cast<PFN_FFXProxyPresent1>(originalPresent1),
                                             std::memory_order_release);
            g_FFXProxyPresent1VtableEntry = &vtable[22];
        }
    }
    g_FFXProxySwapchain = swapChain;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_FFXProxyPresentHookInstallQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_release);
    g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
    g_FFXProxyPreworkCount.store(0, std::memory_order_relaxed);
    g_FFXProxyPresentHookInstalled.store(true, std::memory_order_release);
    g_FFXProxyPresentQuiescing.store(false, std::memory_order_release);
    HookLogImportant(
        "DX12: FFX proxy-present hook INSTALLED (proxy=%p vtable[8]=%p->%p present1Hooked=%d ffxModule=%p "
        "source=%s) — composite + substitute re-assert now run on the GAME thread before AMD's Present; "
        "AMD's presenter thread stays untouched",
        swapChain, (void*)&vtable[8], originalPresent, g_FFXProxyPresent1VtableEntry ? 1 : 0, (void*)ffxModule,
        source ? source : "?");
    return true;
}

// Caller holds g_FFXProxyPresentHookMutex. Restores the patched class-vtable entries when they are still
// readable and still point at CE's detours (the FFX module may already be unloaded at teardown — then the
// vtable memory is gone with it and there is nothing to restore).
static void DX12_RemoveFFXProxyPresentHookLocked(const char* reason) {
    if (!g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        return;
    }
    g_FFXProxyPresentHookInstalled.store(false, std::memory_order_release);
    if (g_FFXProxyPresentVtableEntry &&
        IsReadableSwapchainPointer(reinterpret_cast<const void*>(g_FFXProxyPresentVtableEntry)) &&
        *g_FFXProxyPresentVtableEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent)) {
        VTableHook::Remove(reinterpret_cast<void*>(g_FFXProxyPresentVtableEntry),
                           reinterpret_cast<void*>(g_FFXProxyPresentOriginal.load(std::memory_order_acquire)));
    }
    if (g_FFXProxyPresent1VtableEntry &&
        IsReadableSwapchainPointer(reinterpret_cast<const void*>(g_FFXProxyPresent1VtableEntry)) &&
        *g_FFXProxyPresent1VtableEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent1)) {
        VTableHook::Remove(reinterpret_cast<void*>(g_FFXProxyPresent1VtableEntry),
                           reinterpret_cast<void*>(g_FFXProxyPresent1Original.load(std::memory_order_acquire)));
    }
    HookLogImportant("DX12: FFX proxy-present hook removed (%s) (proxy=%p preworks=%llu)", reason ? reason : "?",
                     g_FFXProxySwapchain, static_cast<unsigned long long>(g_FFXProxyPreworkCount.load()));
    g_FFXProxySwapchain = nullptr;
    g_FFXProxyPresentVtableEntry = nullptr;
    g_FFXProxyPresent1VtableEntry = nullptr;
    // Keep the immutable forward targets published after unpatching. A detour already in flight owns a local
    // copy; clearing here creates an avoidable null-forward race. A later install replaces them atomically.
}

void DX12_RemoveFFXProxyPresentHook(const char* reason) {
    g_FFXProxyPresentQuiescing.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_FFXProxyPresentHookMutex);
        DX12_RemoveFFXProxyPresentHookLocked(reason);
    }

    if (t_FFXProxyPresentDetourDepth != 0) {
        // No current call site removes the hook from inside its detour. Keep this explicit diagnostic rather
        // than self-deadlocking if a future provider unexpectedly does so.
        HookLogImportant(
            "DX12: FFX proxy-present hook removal requested from inside its own detour; drain deferred "
            "(reason=%s inFlight=%u)",
            reason ? reason : "?", g_FFXProxyPresentDetoursInFlight.load(std::memory_order_acquire));
        return;
    }

    std::unique_lock<std::mutex> drainLock(g_FFXProxyPresentDrainMutex);
    g_FFXProxyPresentDrainCV.wait(
        drainLock, []() { return g_FFXProxyPresentDetoursInFlight.load(std::memory_order_acquire) == 0; });
    HookLogImportant("DX12: FFX proxy-present detours drained (%s)", reason ? reason : "removed");
}

// Freeze-dump snapshot of the proxy-present driver state (extends the ffx-ui-composite freeze diag).
void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason) {
    HookLogImportant(
        "DX12: [ffx-proxy-present-freeze-diag] %s — installed=%d driving=%d proxy=%p preworks=%llu "
        "lastPreworkQpc=%llu lastPreworkTid=0x%04X installQpc=%llu",
        reason ? reason : "freeze", g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
        DX12_IsFFXProxyPresentHookDriving() ? 1 : 0, g_FFXProxySwapchain,
        static_cast<unsigned long long>(g_FFXProxyPreworkCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_FFXProxyPreworkLastQpc.load(std::memory_order_acquire)),
        g_FFXProxyPreworkLastTid.load(std::memory_order_acquire),
        static_cast<unsigned long long>(g_FFXProxyPresentHookInstallQpc.load(std::memory_order_acquire)));
}
