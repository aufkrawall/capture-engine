#include "dx12_hook_internal.h"


bool ShouldUseConfirmedPostSLForOverlayIncludedWork(const OverlayConfig& cfg) {
return cfg.showOverlay && dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) &&
       dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
}


void CaptureRequestedDX12Screenshot(IDXGISwapChain3* sc3, SharedMemoryLayout* shm, uint64_t requestId, ID3D12CommandQueue* queueOverride) {
if (!sc3 || !shm || requestId == 0)
    return;

bool queued = false;
ID3D12Device* dx12Device = g_Device.load();
ID3D12CommandQueue* dx12Queue = queueOverride ? queueOverride : g_CommandQueue.load();
if (dx12Device && dx12Queue) {
    UINT bbIdx = sc3->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (SUCCEEDED(sc3->GetBuffer(bbIdx, IID_PPV_ARGS(&backBuffer)))) {
        const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
        const auto presentationEncoding = DXGIShared::ResolveSwapChainPresentationEncoding(
            static_cast<IDXGISwapChain*>(sc3), resourceDesc.Format);
        queued = SaveDX12TextureAsScreenshotRaw(dx12Device, dx12Queue, backBuffer, shm, requestId,
                                                presentationEncoding);
        backBuffer->Release();
    }
}
if (!queued)
    CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}


void PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx, UINT currentBackBufferIdx) {
if (!pSwapChain || !shm || !captureQueue)
    return;
if (shm->throttleCapture.load(std::memory_order_acquire))
    return;

DXGI_SWAP_CHAIN_DESC swapChainDesc{};
auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
    presentationEncoding =
        DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
}
shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));

std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
ID3D12Device* captureDevice = g_Device.load(std::memory_order_acquire);
if (!dx12_hook_g_SharedCaptureD3D12.IsInitializedFor(captureDevice, pSwapChain)) {
    if (!dx12_hook_g_SharedCaptureD3D12.Initialize(captureDevice, pSwapChain)) {
        return;
    }
    HookLogImportant("DX12: Shared capture initialized for swapchain generation sc=%p device=%p", pSwapChain,
                     captureDevice);
}

UINT bbIdx = 0;
if (hasCurrentBackBufferIdx) {
    bbIdx = currentBackBufferIdx;
} else {
    IDXGISwapChain3* sc3 = nullptr;
    pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    bbIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
    if (sc3)
        sc3->Release();
}

if (!dx12_hook_g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx))
    return;

SharedFrameDescriptor desc;
if (!dx12_hook_g_SharedCaptureD3D12.GetCurrentFrame(&desc))
    return;

for (UINT i = 0; i < SharedCaptureD3D12::kSharedTextureCount; ++i) {
    shm->SetSharedHandle(static_cast<int>(i), (uint64_t)dx12_hook_g_SharedCaptureD3D12.GetSharedHandle((int)i));
}
shm->SetFenceShareHandle((uint64_t)dx12_hook_g_SharedCaptureD3D12.GetFenceShareHandle());
shm->SetWidth(desc.width);
shm->SetHeight(desc.height);
shm->SetFormat(desc.format);

uint32_t wIdx = shm->frameRing.writeIndex.load(std::memory_order_acquire);
uint32_t rIdx = shm->frameRing.readIndex.load(std::memory_order_acquire);
if ((uint32_t)(wIdx - rIdx) < (uint32_t)FRAME_RING_SIZE) {
    const bool ringWasEmpty = wIdx == shm->frameRing.ingestIndex.load(std::memory_order_acquire);
    FrameSlot& slot = shm->frameRing.slots[wIdx % FRAME_RING_SIZE];
    slot.fenceValue = desc.fenceValue;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    slot.timestamp = desc.presentTime;
    slot.frameIndex = desc.frameNumber;
    slot.textureIndex = desc.textureIndex;
    slot.sourcePid = GetCurrentProcessId();
    std::atomic_thread_fence(std::memory_order_release);
    slot.valid.store(1, std::memory_order_release);
    shm->frameRing.writeIndex.store(wIdx + 1, std::memory_order_release);
    if (ringWasEmpty && g_IPC) {
        g_IPC->SignalInjectFrameReady();
    }
    DXGIShared::SetLatestSourceFrameIndex(desc.frameNumber);
    static uint64_t s_lastPublishLineageLogTick = 0;
    uint64_t nowTick = GetTickCount64();
    if (nowTick - s_lastPublishLineageLogTick >= 1000) {
        HookLog("DX12: Publish frame=%u ring=%u tex=%d fence=%llu ts=%llu bb=%u depth=%u", desc.frameNumber, wIdx,
                desc.textureIndex, static_cast<unsigned long long>(desc.fenceValue),
                static_cast<unsigned long long>(desc.presentTime), bbIdx, static_cast<unsigned>(wIdx - rIdx));
        s_lastPublishLineageLogTick = nowTick;
    }
} else {
    shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
    shm->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
}
}
