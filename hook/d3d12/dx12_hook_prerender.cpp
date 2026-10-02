#include "dx12_hook_internal.h"


void ApplyPrerenderLimitDX12(float limit, bool frameGenerationPresentationActive) {
if (limit < 0.0f)
    return;
// FG runtimes can replace the observed ECL queue with their own internal
// presentation queue. Keep the limiter on the retained application queue
// so its fence stream cannot become part of a runtime Present dependency.
bool usesOriginalGameQueue = false;
ID3D12CommandQueue* currentQueueSnapshot = nullptr;
DX12Context ctx = GetDX12PrerenderContext(frameGenerationPresentationActive, &usesOriginalGameQueue,
                                          &currentQueueSnapshot);
if (!ctx.IsValid())
    return;

std::lock_guard<std::mutex> lock(dx12_hook_g_PrerenderMutex);

if (dx12_hook_g_PrerenderDevice != ctx.device || dx12_hook_g_PrerenderQueue != ctx.queue) {
    for (auto* fence : dx12_hook_g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    dx12_hook_g_PrerenderFences.clear();
    for (HANDLE event : dx12_hook_g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    dx12_hook_g_PrerenderEvents.clear();
    dx12_hook_g_PrerenderFrameIndex = 0;
    if (dx12_hook_g_PrerenderDevice)
        dx12_hook_g_PrerenderDevice->Release();
    if (dx12_hook_g_PrerenderQueue)
        dx12_hook_g_PrerenderQueue->Release();
    dx12_hook_g_PrerenderDevice = ctx.device;
    dx12_hook_g_PrerenderQueue = ctx.queue;
    dx12_hook_g_PrerenderDevice->AddRef();
    dx12_hook_g_PrerenderQueue->AddRef();
    HookLogImportant(
        "DX12: Prerender fence stream rebound device=%p queue=%p role=%s currentQueue=%p",
        ctx.device, ctx.queue, usesOriginalGameQueue ? "original-game" : "current-fallback",
        currentQueueSnapshot);
}

// Initialize fence ring buffer if needed
if (dx12_hook_g_PrerenderFences.empty()) {
    for (int i = 0; i < 16; i++) {
        ID3D12Fence* fence = nullptr;
        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(ctx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            dx12_hook_g_PrerenderFences.push_back(fence);
            dx12_hook_g_PrerenderEvents.push_back(event);
        } else if (event) {
            CloseHandle(event);
        }
    }
    HookLog("DX12: Created prerender limit fence ring buffer (size: %d)", (int)dx12_hook_g_PrerenderFences.size());
}

if (dx12_hook_g_PrerenderFences.empty())
    return;

static std::atomic<int> s_prerenderWarnLogs{0};
auto waitForFence = [&](ID3D12Fence* fenceToWait, HANDLE waitEvent, uint64_t waitValue) -> bool {
    if (!fenceToWait || !waitEvent)
        return false;
    if (fenceToWait->GetCompletedValue() >= waitValue)
        return true;

    HRESULT setHr = fenceToWait->SetEventOnCompletion(waitValue, waitEvent);
    if (FAILED(setHr)) {
        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender SetEventOnCompletion failed hr=0x%08X value=%llu", setHr, waitValue);
        }
        return false;
    }

    DWORD waitResult = WaitForSingleObject(waitEvent, INFINITE);
    if (waitResult == WAIT_OBJECT_0)
        return true;

    if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender wait failed result=%lu error=%lu value=%llu", waitResult, GetLastError(),
                waitValue);
    }
    return false;
};

size_t idx = dx12_hook_g_PrerenderFrameIndex % dx12_hook_g_PrerenderFences.size();
ID3D12Fence* fence = dx12_hook_g_PrerenderFences[idx];
HANDLE event = dx12_hook_g_PrerenderEvents[idx];

if (limit == 0.0f) {
    // Strict Serial: Signal and immediately wait
    uint64_t value = dx12_hook_g_PrerenderFrameIndex + 1;
    HRESULT signalHr = ctx.queue->Signal(fence, value);
    if (SUCCEEDED(signalHr)) {
        waitForFence(fence, event, value);
    } else if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, value);
    }
} else {
    const int lookback = std::clamp(static_cast<int>(limit), 1, 6);

    // Signal current frame
    uint64_t signalValue = dx12_hook_g_PrerenderFrameIndex + 1;
    HRESULT signalHr = ctx.queue->Signal(fence, signalValue);
    if (FAILED(signalHr)) {
        if (s_prerenderWarnLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLog("DX12: Prerender signal failed hr=0x%08X value=%llu", signalHr, signalValue);
        }
        dx12_hook_g_PrerenderFrameIndex++;
        return;
    }

    // Wait on N frames ago
    if (dx12_hook_g_PrerenderFrameIndex >= (uint64_t)lookback) {
        size_t waitIdx = (dx12_hook_g_PrerenderFrameIndex - lookback) % dx12_hook_g_PrerenderFences.size();
        ID3D12Fence* waitFence = dx12_hook_g_PrerenderFences[waitIdx];
        HANDLE waitEvent = dx12_hook_g_PrerenderEvents[waitIdx];
        uint64_t waitValue = (dx12_hook_g_PrerenderFrameIndex - lookback) + 1;

        if (waitFence->GetCompletedValue() < waitValue) {
            waitForFence(waitFence, waitEvent, waitValue);
        }
    }
}

dx12_hook_g_PrerenderFrameIndex++;
}
