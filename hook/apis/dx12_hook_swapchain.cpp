#include "dx12_hook_internal.h"

// REQUIRED EXPORTS
void DX12_AdjustWrapperResizeDepth(int delta) {
    if (delta > 0)
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_add(delta);
    else
        DXGIShared::g_SharedState.wrapperResizeDepth.fetch_sub(-delta);
}

void DX12_InvalidateSwapchain() {
    DXGIShared::g_SharedState.swapchainInvalid.store(true, std::memory_order_release);
    HookLog("DX12: Swapchain marked INVALID (FSR/FG transition detected)");
    ReleaseStreamlineStartupActivationSwapchain("DX12_InvalidateSwapchain");
    // The make-before-break keep-alive is bound to the live proxy swapchain;
    // a swapchain invalidation ends it.
    dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kSwapchainInvalidation, "DX12_InvalidateSwapchain");
    // Log current state for debugging
    HookLog("DX12: Invalidating - overlayInit=%d, syncInit=%d, device=%p, queue=%p", dx12_hook_g_State.overlayInit,
            dx12_hook_g_State.syncInit, g_Device.load(), g_CommandQueue.load());

    // Only invalidate swapchain-level state, not device-level sync resources
    // This allows swapchain changes without full reinitialization
    if (dx12_hook_g_State.overlayInit) {
        HookLog(
            "DX12: Invalidating swapchain resources (device-level resources "
            "preserved)");
        dx12_hook_g_State.overlayInit = false;
        CleanupRTVs();
    }
}

void DX12_SignalFSR4SwapchainRecreated() {
    DXGIShared::g_SharedState.fsr4RecreationPending.store(true, std::memory_order_release);
    HookLog("DX12: FSR4 swapchain recreation signaled");
}

void DX12_AdjustWrapperResizeDepth_C(int delta) {
    DX12_AdjustWrapperResizeDepth(delta);
}

// Queue-aware wrapper fallback for frame classification.
// The wrapper path is only used when the real queue has not been registered yet;
// once registration succeeds, the vtable ECL detour becomes the authoritative
// source of command-list counts.
__attribute__((noinline)) void DX12_NotifyCommandListsForQueue(ID3D12CommandQueue* pQueue, UINT numCommandLists) {
    if (!pQueue || numCommandLists == 0) {
        return;
    }

    auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblPtr) {
        return;
    }

    D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return;
    }

    ID3D12CommandQueue* classificationQueue = GetFrameClassificationQueue();
    if (!classificationQueue || pQueue != classificationQueue) {
        static std::atomic<int> s_skippedWrapperNotifyLogCount{0};
        int skipCount = s_skippedWrapperNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (skipCount < 10 || (skipCount % 2048) == 2047) {
            HookLog(

                "DX12: Ignoring wrapper queue notify for non-classification queue "
                "%p (class=%p, primary=%p, orig=%p, num=%u)",
                pQueue, classificationQueue, dx12_hook_g_PrimaryGameQueue.load(std::memory_order_relaxed), dx12_hook_g_OriginalGameQueue,
                numCommandLists);
        }
        return;
    }

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    dx12_hook_g_CommandListsExecutedThisFrame.fetch_add(numCommandLists, std::memory_order_relaxed);
}

// Legacy queue-less wrapper notify. Ignore it so stale helper traffic cannot
// mark auxiliary command queue work as a real frame.
void DX12_NotifyCommandLists(UINT numCommandLists) {
    static std::atomic<int> s_legacyNotifyLogCount{0};
    int logCount = s_legacyNotifyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 5) {
        HookLog(
            "DX12: Ignoring legacy queue-less DX12_NotifyCommandLists(%u) to avoid "
            "false real-frame classification",
            numCommandLists);
    }
}

void RemoveGlobalVTableHooks() {
    // Remove deep hook first (patches function body past external JMP)
    if (dx12_hook_s_realCreateSCForHwndAddr) {
        if (InlineHook::RemoveDeepHook(dx12_hook_s_realCreateSCForHwndAddr)) {
            dx12_hook_s_realCreateSCForHwndAddr = nullptr;
            dx12_hook_s_deepHookTrampoline = nullptr;
        } else {
            HookLogImportant(
                "DX12: Retaining deep CreateSwapChain chain pointers because CE could not prove sole ownership");
        }
    }

    // RemoveAll owns the inline patch transaction. Keep its trampoline pointer
    // resident because a foreign follower can retain the CE detour as its next
    // chain link even after the export entry changes.
    if (dx12_hook_s_oCreateSCForHwndInline) {
        HookLog("DX12: Retaining inline CreateSwapChainForHwnd trampoline for saved-chain safety");
    }

    // Clear tracked swapchains (no Release needed — we don't AddRef tracked SCs)
    {
        std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
        for (const auto& entry : dx12_hook_s_hwndSwapchainMap) {
            for (IDXGISwapChain* swapchain : entry.second) {
                DXGIShared::DX12_UnregisterThirdPartyOverlaySwapchain(swapchain);
            }
        }
        dx12_hook_s_hwndSwapchainMap.clear();
    }

    if (!dx12_hook_oCreateSwapChainGlobal && !dx12_hook_oCreateSwapChainForHwndGlobal) {
        return;
    }

    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        HookLog("DX12: DXGI module not loaded, skipping vtable hook removal");
        return;
    }

    typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateFactory) {
        HookLog("DX12: CreateDXGIFactory1 not found for vtable hook removal");
        return;
    }

    IDXGIFactory2* pFactory = nullptr;
    HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
    if (FAILED(hr) || !pFactory) {
        HookLog("DX12: Failed to create factory for vtable hook removal");
        return;
    }

    void** vtable = *(void***)pFactory;

    if (dx12_hook_oCreateSwapChainGlobal) {
        VTableHook::Remove(reinterpret_cast<void*>(&vtable[10]), (void*)dx12_hook_oCreateSwapChainGlobal);
        HookLog("DX12: Removed CreateSwapChain vtable hook");
        dx12_hook_oCreateSwapChainGlobal = nullptr;
    }

    if (dx12_hook_oCreateSwapChainForHwndGlobal) {
        VTableHook::Remove(reinterpret_cast<void*>(&vtable[15]), (void*)dx12_hook_oCreateSwapChainForHwndGlobal);
        HookLog("DX12: Removed CreateSwapChainForHwnd vtable hook");
        dx12_hook_oCreateSwapChainForHwndGlobal = nullptr;
    }

    pFactory->Release();
    HookLog("DX12: Global factory vtable hooks removed");
}

// Install Present vtable hooks for pre-existing swapchains (late injection)
// DISABLED: Global Present vtable hooks cause shutdown crashes
// Factory wrapping is now the primary mechanism for intercepting swapchains
void DX12_InstallPresentHooksForSwapchain(IDXGISwapChain* pSwapChain) {
    // DISABLED: Present vtable hooks are disabled to prevent crashes
    // Pre-existing swapchains (created before injection) won't have overlay

    (void)pSwapChain;
}
