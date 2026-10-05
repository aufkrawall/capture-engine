#include "dx12_hook_internal.h"
#include "hook/runtime/hook_clock.h"
#include "common/logging/log_meter.h"


void WaitForInFlightPostSLCallbacks(const char* reason) {
for (int spin = 0; spin < 200; ++spin) {
    uint32_t inFlight = g_PostSLLifecycle.CallbacksInFlight();
    if (inFlight == 0) {
        return;
    }

    if (spin == 0 || spin == 10 || spin == 50) {
        HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
    }
    Sleep(1);
}

uint32_t remaining = g_PostSLLifecycle.CallbacksInFlight();
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


namespace {
void ReleaseRetiredPostSLQueue(ID3D12CommandQueue* queue, const char* role, const char* reason) {
    HookLogImportant("[PostSLLifecycle] queue=release role=%s queue=%p epoch=%u reason=%s", role, queue,
                     g_PostSLLifecycle.Epoch(), reason);
    queue->Release();
}
void RecordPostSLRetirementWork() {
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);
    g_PostSLQueues.RecordRetirementWork(dx12_hook_g_State.fence, dx12_hook_g_State.currentFenceValue);
}
}  // namespace

void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
    auto retired = g_PostSLQueues.DetachPinnedWrapper();
    if (retired.Borrow()) {
        HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, retired.Borrow());
        retired.Release();
    }
}

void ClearPostSLQueues(const char* reason) {
    const bool released = g_PostSLQueues.ClearSelection([&](ID3D12CommandQueue* queue, const char* role) {
        ReleaseRetiredPostSLQueue(queue, role, reason);
    });
    if (!released) {
        static ce::log_meter::ChangeGate retirementGate;
        const auto epoch = g_PostSLLifecycle.Epoch();
        const auto verdict = retirementGate.Observe(ce::log_meter::FieldKey(epoch));
        if (verdict) {
            HookLogImportant("[PostSLLifecycle] queues=deferred epoch=%u reason=gpu-incomplete transition=%s%s",
                epoch, reason, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
}

void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
    const bool runtimeActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const uint32_t callbacks = g_PostSLLifecycle.CallbacksInFlight();
    if (g_PostSLQueues.RetirementPending() && !runtimeActive && callbacks == 0) {
        // Refresh the final value after a callback that outlasted the bounded drain.
        RecordPostSLRetirementWork();
    }
    const bool retiredSelection = g_PostSLQueues.CleanupDeferred(runtimeActive, callbacks,
        [&](ID3D12CommandQueue* queue, const char* role) { ReleaseRetiredPostSLQueue(queue, role, reason); });
    if (auto* deferredCommandQueue = dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
        deferredCommandQueue->Release();
    }
    if (retiredSelection) RealignInactiveCommandQueueToSwapchainQueue(reason);
}


void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
if (!queue) {
    return;
}

constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
dx12_hook_g_PostSLRecentTeardownActivityUntilMs.store(ce::hook_clock::TickCount64() + kPostSLRecentTeardownActivityMs,
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
g_PostSLLifecycle.InvalidateGeneration();
g_PostSLLifecycle.ResetStartupEvidence();
dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);

if (deferQueueReleaseUntilCallbacksDrain) {
    SetPostSLCallbackInstalled(false, reason);
    WaitForInFlightPostSLCallbacks(reason);
    RecordPostSLRetirementWork();
    WaitForOverlayGpuIdle(reason);
    g_PostSLQueues.DeferSelectionRetirement();
} else {
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
