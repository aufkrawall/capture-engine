#include "dx12_hook_internal.h"


void WaitForInFlightPostSLCallbacks(const char* reason) {
for (int spin = 0; spin < 200; ++spin) {
    uint32_t inFlight = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
    if (inFlight == 0) {
        return;
    }

    if (spin == 0 || spin == 10 || spin == 50) {
        HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
    }
    Sleep(1);
}

uint32_t remaining = dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire);
if (remaining != 0) {
    HookLogImportant("%s — timed out waiting for %u in-flight PostSL callback(s)", reason, remaining);
}
}


void WaitForOverlayGpuIdle(const char* reason) {
if (!dx12_hook_g_State.fence || dx12_hook_g_State.currentFenceValue == 0) {
    return;
}

const UINT64 lastVal = dx12_hook_g_State.currentFenceValue;
HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
if (!drainEvent) {
    return;
}

HRESULT drainHr = dx12_hook_g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
if (SUCCEEDED(drainHr)) {
    DWORD waitResult = WaitForSingleObject(drainEvent, 200);
    HookLogImportant("%s — drained overlay GPU work (fenceVal=%llu wait=%u)", reason, (unsigned long long)lastVal,
                     waitResult);
} else {
    HookLogImportant("%s — fence drain failed hr=0x%08X", reason, drainHr);
}
CloseHandle(drainEvent);
}


void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
ID3D12CommandQueue* oldPinnedWrapperQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    oldPinnedWrapperQueue = dx12_hook_g_PostSLPinnedSLWrapperQueue;
    dx12_hook_g_PostSLPinnedSLWrapperQueue = nullptr;
}

if (oldPinnedWrapperQueue) {
    HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, oldPinnedWrapperQueue);
    oldPinnedWrapperQueue->Release();
}
}


void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut) {
if (lockedQueueOut) {
    *lockedQueueOut = nullptr;
}
if (dedicatedQueueOut) {
    *dedicatedQueueOut = nullptr;
}

std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
if (lockedQueueOut) {
    *lockedQueueOut = dx12_hook_g_PostSLLockedQueue;
}
if (dedicatedQueueOut) {
    *dedicatedQueueOut = dx12_hook_g_PostSLDedicatedQueue;
}
dx12_hook_g_PostSLLockedQueue = nullptr;
dx12_hook_g_PostSLDedicatedQueue = nullptr;
}


void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue, ID3D12CommandQueue* dedicatedQueue) {
if (lockedQueue) {
    HookLogImportant("%s — releasing PostSL locked queue %p", reason, lockedQueue);
    lockedQueue->Release();
}

if (dedicatedQueue) {
    HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, dedicatedQueue);
    dedicatedQueue->Release();
}
}


void ClearPostSLQueues(const char* reason) {
ID3D12CommandQueue* oldLockedQueue = nullptr;
ID3D12CommandQueue* oldDedicatedQueue = nullptr;
DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);
ReleaseDetachedPostSLQueues(reason, oldLockedQueue, oldDedicatedQueue);
}


void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
ID3D12CommandQueue* deferredLockedQueue =
    dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
if (deferredLockedQueue) {
    HookLogImportant("%s - releasing deferred PostSL locked queue %p", reason, deferredLockedQueue);
    deferredLockedQueue->Release();
}

ID3D12CommandQueue* deferredCommandQueue =
    dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
if (deferredCommandQueue) {
    HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
    deferredCommandQueue->Release();
}

if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.load(std::memory_order_acquire)) {
    return;
}

if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
    return;
}

if (dx12_hook_g_PostSLCallbackInFlight.load(std::memory_order_acquire) != 0) {
    return;
}

if (!dx12_hook_g_PostSLDeferredQueueCleanupPending.exchange(false, std::memory_order_acq_rel)) {
    return;
}

ID3D12CommandQueue* oldLockedQueue = nullptr;
ID3D12CommandQueue* oldDedicatedQueue = nullptr;
DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);

if (oldLockedQueue) {
    ID3D12CommandQueue* previouslyDeferred =
        dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(oldLockedQueue, std::memory_order_acq_rel);
    if (previouslyDeferred) {
        HookLogImportant("%s - releasing superseded deferred PostSL locked queue %p", reason, previouslyDeferred);
        previouslyDeferred->Release();
    }
    HookLogImportant("%s - deferred PostSL locked queue release %p", reason, oldLockedQueue);
}
if (oldDedicatedQueue) {
    HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, oldDedicatedQueue);
    oldDedicatedQueue->Release();
}

RealignInactiveCommandQueueToSwapchainQueue(reason);
}


void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
if (!queue) {
    return;
}

constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
dx12_hook_g_PostSLRecentTeardownActivityUntilMs.store(GetTickCount64() + kPostSLRecentTeardownActivityMs,
                                            std::memory_order_release);
static std::atomic<int> s_postSLRecentTeardownLogCount{0};
const int logCount = s_postSLRecentTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount < 10 || (logCount % 128) == 0) {
    HookLogImportant("%s - marking PostSL queue %p as recently active during Streamline teardown (%llums)", reason,
                     queue, (unsigned long long)kPostSLRecentTeardownActivityMs);
}
}


void InvalidateAllOverlayCachedFrames() {
g_OverlayAdapter.InvalidateCachedFrame();
dx12_hook_g_D3D11On12Adapter.InvalidateCachedFrame();
dx12_hook_g_SLFGAdapter.InvalidateCachedFrame();
}


void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper, bool deferQueueReleaseUntilCallbacksDrain) {
dx12_hook_g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);

if (deferQueueReleaseUntilCallbacksDrain) {
    SetPostSLCallbackInstalled(false, reason);
    WaitForInFlightPostSLCallbacks(reason);
    WaitForOverlayGpuIdle(reason);
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(true, std::memory_order_release);
} else {
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues(reason);
}

ClearPostSLPinnedSLWrapperQueue(reason);

if (clearRealQueueBehindSLWrapper) {
    ID3D12CommandQueue* oldRealQueue = dx12_hook_g_RealQueueBehindSLWrapper.exchange(nullptr, std::memory_order_acq_rel);
    if (oldRealQueue) {
        HookLogImportant("%s — cleared cached real queue behind SL wrapper %p", reason, oldRealQueue);
    }
}
}


void RealignInactiveCommandQueueToSwapchainQueue(const char* reason) {
ID3D12CommandQueue* oldCommandQueue = nullptr;
ID3D12CommandQueue* swapchainQueue = nullptr;
ID3D12CommandQueue* originalGameQueue = nullptr;
bool realignedCommandQueue = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    originalGameQueue = dx12_hook_g_OriginalGameQueue;
    ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
    bool actualFGActive = IsActualFrameGenerationActive();
    bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    if (ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
            actualFGActive, streamlineFGRunning, swapchainQueue != nullptr, originalGameQueue != nullptr,
            currentCommandQueue != nullptr, currentCommandQueue == swapchainQueue,
            currentCommandQueue == originalGameQueue,
            currentCommandQueue == dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire))) {
        oldCommandQueue = currentCommandQueue;
        g_CommandQueue.store(swapchainQueue, std::memory_order_release);
        swapchainQueue->AddRef();
        realignedCommandQueue = true;
    }
}

if (realignedCommandQueue) {
    HookLogImportant("%s - realigned stale command queue %p -> swapchain queue %p (origGame=%p)", reason,
                     oldCommandQueue, swapchainQueue, originalGameQueue);
    if (oldCommandQueue) {
        ID3D12CommandQueue* previouslyDeferred =
            dx12_hook_g_DeferredCommandQueueRelease.exchange(oldCommandQueue, std::memory_order_acq_rel);
        if (previouslyDeferred) {
            HookLogImportant("%s - releasing superseded deferred stale command queue %p", reason,
                             previouslyDeferred);
            previouslyDeferred->Release();
        }
    }
}
}


void MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled() {
if (dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth <= 0) {
    return;
}
dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled = true;
}
