#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"


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

AcquiredNativeFSROwnerQueue AcquireNativeFSRSwapchainPresentationQueue(IDXGISwapChain* proxy,
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

bool SubmitNativeFSROwnerQueueOverlayCommandList(ID3D12CommandQueue* queue, ID3D12CommandList* commandList) {
    if (!queue || !commandList) {
        return false;
    }
    ID3D12CommandList* lists[] = {commandList};
    ScopedCEOverlayECLSubmission ceOverlayECLGuard("ffx-owner-queue");
    queue->ExecuteCommandLists(1, lists);
    return true;
}

HRESULT SignalNativeFSROwnerQueueOverlayFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64 value) {
    return queue && fence ? queue->Signal(fence, value) : E_INVALIDARG;
}

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

bool DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR(IDXGISwapChain* presentedSwapChain,
                                                              ID3D12CommandQueue* submitQueue) {
    if (!presentedSwapChain || !submitQueue) {
        return false;
    }

    // The presented swapchain at CE's deep body hook is the swapchain whose backbuffer the foreign
    // overlay just composited into. Resolve its exact current buffer per present instead of relying
    // on the normal overlay backend's cached RTV heap: during active FSR FG that heap is preserved
    // across the FFX swapchain change and can still reference the pre-FG buffers, which is exactly
    // the stale-target shape the 0.1.5972 device removal was traced to.
    IDXGISwapChain3* swapChain3 = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(presentedSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3) {
        return false;
    }
    swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();
    if (!backBuffer) {
        static std::atomic<int> s_getBufferRefusedLog{0};
        const int logCount = s_getBufferRefusedLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Below-foreign-chain FSR deep draw REFUSED — GetBuffer failed (sc=%p queue=%p log=%d)",
                (void*)presentedSwapChain, (void*)submitQueue, logCount + 1);
        }
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    bool hdr = false;
    if (SUCCEEDED(presentedSwapChain->GetDesc(&desc))) {
        // No log prefix: this runs on the steady-state present path and the renderer itself logs
        // the HDR contract on change.
        hdr = ResolveSwapchainOutputHDRState(presentedSwapChain, desc.BufferDesc.Format, nullptr);
    }

    ce::dx12_ffx_suspend_overlay::RenderRequest request = {};
    request.proxySwapChain = presentedSwapChain;
    request.presentationQueue = submitQueue;
    request.targetResource = backBuffer;
    request.targetState = D3D12_RESOURCE_STATE_PRESENT;
    request.clearTransparent = false;
    request.routeName = "below-foreign-chain-fsr";
    request.submitCommandList = &SubmitNativeFSROwnerQueueOverlayCommandList;
    request.signalFence = &SignalNativeFSROwnerQueueOverlayFence;
    request.hdr = hdr;
    const bool rendered = ce::dx12_ffx_suspend_overlay::Render(request);
    backBuffer->Release();

    static std::atomic<int> s_renderedLog{0};
    static std::atomic<int> s_refusedLog{0};
    if (rendered) {
        const int logCount = s_renderedLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5 || (logCount % 300) == 0) {
            HookLogImportant(
                "[OVERLAY LAYER] CE composites BELOW the foreign Present chain on the runtime-owned FSR "
                "swapchain (site=deep-body-below-foreign-chain-runtime-owned-fsr "
                "source=DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR sc=%p queue=%p log=%d) — "
                "the foreign overlays drew on this queue before CE, so CE's overlay is the topmost layer",
                (void*)presentedSwapChain, (void*)submitQueue, logCount + 1);
        }
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kBelowForeignChainRuntimeOwnedFSR);
    } else {
        const int logCount = s_refusedLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Below-foreign-chain FSR deep draw REFUSED by the owner-queue renderer (sc=%p queue=%p "
                "log=%d) — the FFX present-callback draw remains the overlay transport",
                (void*)presentedSwapChain, (void*)submitQueue, logCount + 1);
        }
    }
    return rendered;
}
