#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"

static void DX12_RemoveFFXProxyPresentHookLocked(const char* reason);  // defined below


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

D3D12_RESOURCE_STATES GetDX12StateFromFFXResourceState(uint32_t state) {
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

void TransitionResourceIfNeeded(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource,
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

void CopyFFXPresentSourceToOutput(ID3D12GraphicsCommandList* cmdList,
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

bool RenderOverlayViaFFXPresentCallback(const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
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

