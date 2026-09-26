#include "dx12_hook_internal.h"

namespace {

bool SameComIdentity(IUnknown* left, IUnknown* right) {
    if (!left || !right)
        return false;
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
           SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) &&
           leftIdentity.Get() == rightIdentity.Get();
}

void LogDX12ScreenshotQueueFailure(const char* stage, HRESULT hr, IDXGISwapChain* swapChain,
                                   ID3D12CommandQueue* queue) {
    static std::atomic<uint64_t> s_failureCount{0};
    const uint64_t count = s_failureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 20 || (count % 200) == 0) {
        HookLogImportant("DX12: Screenshot producer failed at %s hr=0x%08X (sc=%p queue=%p failure=%llu)",
                         stage ? stage : "unknown", static_cast<unsigned>(hr), swapChain, queue,
                         static_cast<unsigned long long>(count));
    }
}

}  // namespace

// True while the PostSL callback draws the overlay for this Present. That
// callback runs before ProcessFrame in the same Present, so it owns the ordering
// for both screenshot variants: the overlay-free copy has to be submitted ahead
// of the overlay list there, and the overlay-included copy after it.
bool PostSLOwnsThisFramesOverlayDraw(const OverlayConfig& cfg) {
    return ce::dx12_overlay_policy::ShouldPostSLOwnScreenshotOrdering(
        cfg.showOverlay, dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire),
        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire),
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
        DXGIShared::IsPostSLFinalOutputPresentCallback(), DXGIShared::WasPostSLOffKeepAlivePrePresentDrawn());
}

void CaptureRequestedDX12Screenshot(IDXGISwapChain* swapChain, SharedMemoryLayout* shm, uint64_t requestId,
                                    ID3D12CommandQueue* queueOverride) {
    if (!shm || requestId == 0 || GetPendingScreenshotRequestId(shm) != requestId)
        return;

    ComPtr<ID3D12CommandQueue> queue;
    if (queueOverride) {
        queue = queueOverride;
    } else {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        queue = g_CommandQueue.load(std::memory_order_acquire);
    }

    bool queued = false;
    const char* failureStage = "missing swapchain or queue";
    HRESULT failureHr = E_POINTER;
    ComPtr<IDXGISwapChain3> swapChain3;
    if (swapChain && queue) {
        failureStage = "IDXGISwapChain3 query";
        failureHr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    }
    ComPtr<ID3D12Resource> backBuffer;
    UINT backBufferIndex = 0;
    if (SUCCEEDED(failureHr) && swapChain3) {
        backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
        failureStage = "backbuffer query";
        failureHr = swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    }
    ComPtr<ID3D12Device> backBufferDevice;
    if (SUCCEEDED(failureHr) && backBuffer) {
        failureStage = "backbuffer device query";
        failureHr = backBuffer->GetDevice(IID_PPV_ARGS(&backBufferDevice));
    }
    ComPtr<ID3D12Device> queueDevice;
    if (SUCCEEDED(failureHr) && backBufferDevice) {
        failureStage = "queue device query";
        failureHr = queue->GetDevice(IID_PPV_ARGS(&queueDevice));
    }
    if (SUCCEEDED(failureHr) && !SameComIdentity(backBufferDevice.Get(), queueDevice.Get())) {
        failureStage = "swapchain/queue device identity";
        failureHr = E_INVALIDARG;
    }
    if (SUCCEEDED(failureHr)) {
        const D3D12_RESOURCE_DESC resourceDesc = backBuffer->GetDesc();
        const auto presentationEncoding =
            DXGIShared::ResolveSwapChainPresentationEncoding(swapChain, resourceDesc.Format);
        failureStage = "asynchronous readback submission";
        queued = SaveDX12TextureAsScreenshotRaw(backBufferDevice.Get(), queue.Get(), backBuffer.Get(), shm,
                                                requestId, presentationEncoding);
        if (!queued)
            failureHr = E_FAIL;
    }

    if (!queued && GetPendingScreenshotRequestId(shm) == requestId) {
        LogDX12ScreenshotQueueFailure(failureStage, failureHr, swapChain, queue.Get());
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
    }
}


bool PublishDX12CapturedFrame(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm,
                             ID3D12CommandQueue* captureQueue, bool hasCurrentBackBufferIdx,
                             UINT currentBackBufferIdx, const FrameCaptureMetadata* metadata,
                             ExecuteCommandListsPtr executeCommandLists) {
if (!pSwapChain || !shm || !captureQueue)
    return false;
if (shm->throttleCapture.load(std::memory_order_acquire))
    return false;

DXGI_SWAP_CHAIN_DESC swapChainDesc{};
auto presentationEncoding = ce::presentation_color::Encoding::Unsupported;
if (SUCCEEDED(pSwapChain->GetDesc(&swapChainDesc))) {
    presentationEncoding =
        DXGIShared::ResolveSwapChainPresentationEncoding(pSwapChain, swapChainDesc.BufferDesc.Format);
}
shm->SetIsHDR(ce::presentation_color::IsHDR(presentationEncoding));

std::unique_lock<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex, std::try_to_lock);
if (!capLock.owns_lock()) {
    shm->runtimeState.injectProducerCaptureLockDrops.fetch_add(1, std::memory_order_relaxed);
    return false;
}
ID3D12Device* captureDevice = g_Device.load(std::memory_order_acquire);
// Initialize() closes the previous generation's shared handles right before
// creating new ones, so the new handles can carry the old numeric values.
// Guarded by dx12_hook_g_DX12CaptureMutex like the publication below.
static uint32_t s_transportGeneration = 0;
static SharedMemoryLayout* s_transportGenerationShm = nullptr;
// A replacement host's mapping (possibly at the same address) starts at 0; the
// handles below are republished into it every frame, under a generation of ours.
bool newTransportGeneration = s_transportGenerationShm != shm ||
                              static_cast<uint32_t>(shm->GetTransportGeneration()) != s_transportGeneration;
if (!dx12_hook_g_SharedCaptureD3D12.IsInitializedFor(captureDevice, pSwapChain)) {
    if (!dx12_hook_g_SharedCaptureD3D12.Initialize(captureDevice, pSwapChain)) {
        return false;
    }
    newTransportGeneration = true;
}
if (newTransportGeneration) {
    s_transportGeneration = static_cast<uint32_t>(shm->BeginTransportGeneration());
    s_transportGenerationShm = shm;
    HookLogImportant("DX12: Shared capture initialized for swapchain generation sc=%p device=%p transport=%u",
                     pSwapChain, captureDevice, s_transportGeneration);
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

const int64_t timestampQpc = metadata ? metadata->timestampQpc : 0;
ScopedCEOverlayECLSubmission captureECLGuard("shared capture command list");
if (!dx12_hook_g_SharedCaptureD3D12.CaptureFrame(captureQueue, bbIdx, timestampQpc, executeCommandLists))
    return false;

SharedFrameDescriptor desc;
if (!dx12_hook_g_SharedCaptureD3D12.GetCurrentFrame(&desc))
    return false;

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
    slot.displayTimingSequence = metadata ? metadata->displayTimingSequence : 0;
    slot.frameIndex = desc.frameNumber;
    slot.textureIndex = desc.textureIndex;
    slot.sourcePid = GetCurrentProcessId();
    slot.captureFlags = metadata ? metadata->captureFlags : SHARED_FRAME_CAPTURE_NONE;
    slot.displayTimingGeneration = metadata ? metadata->displayTimingGeneration : 0;
    slot.transportGeneration = s_transportGeneration;
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
        HookLog("DX12: Publish frame=%u ring=%u tex=%d fence=%llu ts=%llu bb=%u depth=%u flags=0x%X "
                "displaySequence=%llu/%u", desc.frameNumber, wIdx,
                desc.textureIndex, static_cast<unsigned long long>(desc.fenceValue),
                static_cast<unsigned long long>(desc.presentTime), bbIdx, static_cast<unsigned>(wIdx - rIdx),
                slot.captureFlags, static_cast<unsigned long long>(slot.displayTimingSequence),
                slot.displayTimingGeneration);
        s_lastPublishLineageLogTick = nowTick;
    }
    return true;
} else {
    shm->frameRing.droppedFrames.fetch_add(1, std::memory_order_relaxed);
    shm->runtimeState.injectProducerMetadataFullDrops.fetch_add(1, std::memory_order_relaxed);
}
return false;
}
