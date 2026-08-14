#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"

namespace {

bool EnsureOverlayAdapterReadyForFFXPresentTarget(
    ID3D12Device* device, ID3D12CommandQueue* callbackQueue, const D3D12_RESOURCE_DESC& resourceDesc,
    const char* initializationSource, bool logReuse, uint64_t frameId) {
    if (!device || !callbackQueue) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);
    dx12_hook_g_State.cachedWidth = static_cast<int>(resourceDesc.Width);
    dx12_hook_g_State.cachedHeight = static_cast<int>(resourceDesc.Height);
    dx12_hook_g_State.format = resourceDesc.Format;

    if (ce::dx12_overlay_policy::ShouldResetFFXPresentCallbackOverlayBackend(
            dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized(), dx12_hook_g_FFXPresentOverlayDevice != device,
            dx12_hook_g_FFXPresentOverlayFormat != resourceDesc.Format)) {
        HookLogImportant(
            "DX12: Resetting FFX present callback overlay backend before runtime-owned FSR use "
            "(source=%s oldDevice=%p newDevice=%p oldFmt=%d newFmt=%d)",
            initializationSource ? initializationSource : "unknown", dx12_hook_g_FFXPresentOverlayDevice, device,
            static_cast<int>(dx12_hook_g_FFXPresentOverlayFormat), static_cast<int>(resourceDesc.Format));
        dx12_hook_g_FFXPresentOverlayAdapter.Shutdown();
    }

    if (!dx12_hook_g_FFXPresentOverlayAdapter.IsInitialized()) {
        dx12_hook_g_FFXPresentOverlayAdapter.SetHwnd(nullptr);
        if (!dx12_hook_g_FFXPresentOverlayAdapter.InitDX12(
                device, callbackQueue, static_cast<int>(resourceDesc.Format))) {
            HookLogImportant(
                "DX12: FFX present callback failed to initialize overlay adapter "
                "(source=%s device=%p queue=%p fmt=%d)",
                initializationSource ? initializationSource : "unknown", device, callbackQueue,
                static_cast<int>(resourceDesc.Format));
            return false;
        }

        const bool outputHDR = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(resourceDesc.Format);
        dx12_hook_g_FFXPresentOverlayAdapter.SetHDR(outputHDR, static_cast<int>(resourceDesc.Format));
        dx12_hook_g_FFXPresentOverlayDevice = device;
        dx12_hook_g_FFXPresentOverlayFormat = resourceDesc.Format;
        HookLogImportant(
            "DX12: FFX present callback initialized overlay adapter for runtime-owned FSR "
            "(source=%s queue=%p fmt=%d hdr=%d %llux%u)",
            initializationSource ? initializationSource : "unknown", callbackQueue,
            static_cast<int>(resourceDesc.Format), outputHDR ? 1 : 0,
            static_cast<unsigned long long>(resourceDesc.Width), resourceDesc.Height);
    } else if (logReuse) {
        static std::atomic<int> s_reuseLogCount{0};
        const int logCount = s_reuseLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLog(
                "DX12: Reusing FFX present callback overlay adapter for runtime-owned FSR "
                "(device=%p fmt=%d frameId=%llu)",
                device, static_cast<int>(resourceDesc.Format), static_cast<unsigned long long>(frameId));
        }
    }

    return true;
}

}  // namespace

bool DX12_EnsureOverlayAdapterReadyForFFXPresentCallback(
    const ce::ffx_api::CallbackDescFrameGenerationPresent* desc) {
    if (!desc || !desc->device || !desc->outputSwapChainBuffer.resource) {
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(desc->device);
    auto* outputResource = static_cast<ID3D12Resource*>(desc->outputSwapChainBuffer.resource);
    ID3D12CommandQueue* callbackQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> queueLock(g_CommandQueueMutex);
        callbackQueue =
            dx12_hook_g_SwapchainQueue ? dx12_hook_g_SwapchainQueue : g_CommandQueue.load(std::memory_order_acquire);
        if (!callbackQueue) {
            callbackQueue = dx12_hook_g_OriginalGameQueue;
        }
    }
    if (!callbackQueue) {
        HookLogImportant(
            "DX12: FFX present callback has no live queue for overlay backend init (device=%p frameId=%llu)",
            device, static_cast<unsigned long long>(desc->frameID));
        return false;
    }

    return EnsureOverlayAdapterReadyForFFXPresentTarget(
        device, callbackQueue, outputResource->GetDesc(), "live app-callback", true, desc->frameID);
}

bool DX12_PrewarmFFXPresentCallbackOverlayAdapter(IDXGISwapChain* presentedSwapChain,
                                                   ID3D12CommandQueue* presentationQueue) {
    if (!presentedSwapChain || !presentationQueue) {
        return false;
    }

    IDXGISwapChain3* swapChain3 = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    ID3D12Device* device = nullptr;
    if (FAILED(presentedSwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3) {
        return false;
    }
    swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();
    if (!backBuffer) {
        return false;
    }
    backBuffer->GetDevice(IID_PPV_ARGS(&device));
    const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
    backBuffer->Release();
    if (!device) {
        return false;
    }

    const bool ready = EnsureOverlayAdapterReadyForFFXPresentTarget(
        device, presentationQueue, resourceDesc, "no-callback topmost prewarm", false, 0);
    device->Release();
    return ready;
}
