#include "dx12_hook_internal.h"
#include "overlay_resource_retirement.h"
#include "common/logging/log_meter.h"

namespace {
ce::dx12::OverlayResourceRetirement<IUnknown, ID3D12Fence> retired;
std::atomic<bool> pending{false};

// The swapchain can release its submit queue before the next Present or
// retirement. Keep the exact submitting queue alive with the pending value.
struct DeferredOverlaySignal {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    UINT64 value = 0;
    int allocatorIndex = -1;
};
DeferredOverlaySignal deferred;
}

void StageDeferredOverlaySignal(ID3D12CommandQueue* queue, UINT64 value, int allocatorIndex) {
    // The draw transaction owns the overlay mutex.
    deferred = {Microsoft::WRL::ComPtr<ID3D12CommandQueue>(queue), value, allocatorIndex};
}

void FlushDeferredOverlaySignalBeforeDraw() {
    // The draw transaction owns the overlay mutex. A runtime can present
    // without taking CE's post-Present flush, so resolve that work before
    // selecting an allocator or computing a new fence value.
    if (deferred.value) {
        DX12_FlushDeferredSignalWithInfo(nullptr);
    }
}

extern "C" __declspec(dllexport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo) {
    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
    auto& state = dx12_hook_g_State;
    if (outInfo) {
        *outInfo = {};
        outInfo->hadDeferredSignal = deferred.value != 0;
        outInfo->hasFence = state.fence != nullptr;
        outInfo->hasFenceEvent = state.fenceEvent != nullptr;
        outInfo->fence = state.fence;
        outInfo->fenceEvent = state.fenceEvent;
        outInfo->fenceValue = deferred.value;
        outInfo->completedValue = state.fence ? state.fence->GetCompletedValue() : 0;
    }
    if (!deferred.value) {
        return false;
    }

    auto signal = std::exchange(deferred, {});
    Microsoft::WRL::ComPtr<ID3D12Fence> pinnedFence = state.fence;
    const HRESULT hr = signal.queue && pinnedFence ? signal.queue->Signal(pinnedFence.Get(), signal.value) : E_POINTER;
    if (outInfo) {
        outInfo->queue = signal.queue.Get();
        outInfo->signalHr = hr;
        outInfo->signalSucceeded = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        if (ce::dx12_overlay_policy::ShouldCommitDeferredOverlayFenceSignalValue(true, signal.value,
                                                                                 state.currentFenceValue)) {
            state.currentFenceValue = signal.value;
        }
        if (signal.allocatorIndex >= 0 && static_cast<size_t>(signal.allocatorIndex) < state.fenceValues.size()) {
            state.fenceValues[signal.allocatorIndex] = signal.value;
        }
        if (outInfo) {
            outInfo->completedValue = pinnedFence->GetCompletedValue();
        }
    } else {
        state.gpuCompletionUnprovable = true;
    }
    static std::atomic<uint32_t> flushCount{0};
    static ce::log_meter::ChangeGate failureGate;
    const uint32_t call = flushCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(call, 12, 2048) ||
        (FAILED(hr) && failureGate.Observe(ce::log_meter::FieldKey(hr, signal.queue.Get())))) {
        HookLogImportant("DX12: Flushed deferred overlay fence signal (fence=%p value=%llu queue=%p hr=0x%08X)",
                         pinnedFence.Get(), static_cast<unsigned long long>(signal.value), signal.queue.Get(),
                         static_cast<unsigned>(hr));
    }
    return SUCCEEDED(hr);
}

void PrepareOverlayGpuRetirement() {
    DX12_FlushDeferredSignalWithInfo(nullptr);
}

void CollectRetiredOverlayGpuResources() {
    if (!pending.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> resources(dx12_hook_g_OverlayMutex);
    retired.Collect();
    pending.store(retired.Pending(), std::memory_order_release);
}

void RetireOverlayGpuResources(bool synchronization, bool views) {
    std::lock_guard<std::recursive_mutex> resources(dx12_hook_g_OverlayMutex);
    PrepareOverlayGpuRetirement();
    retired.Collect();
    auto& state = dx12_hook_g_State;
    auto* fence = state.fence;
    const UINT64 value = state.currentFenceValue;
    const bool unprovable = state.gpuCompletionUnprovable;
    auto* completionFence = dx12_hook_g_OverlayCompletionFence.load(std::memory_order_acquire);
    const UINT64 completionValue = state.overlayCompletionValue;
    std::vector<IUnknown*> objects;
    std::vector<HANDLE> events;
    objects.reserve(state.allocators.size() + 12);
    events.reserve(2);
    auto take = [&](auto*& object) { if (object) objects.push_back(std::exchange(object, nullptr)); };
    if (synchronization) {
        if (!views && state.viewsSubmitted) {
            retired.RememberViewWork(fence, value, unprovable);
            if (completionValue) retired.RememberViewWork(completionFence, completionValue, unprovable);
        }
        for (auto*& allocator : state.allocators) take(allocator);
        state.allocators.clear();
        state.fenceValues.clear();
        take(state.cmdList); take(state.fence); take(state.overlayQueue); take(state.crossQueueFence);
        if (state.fenceEvent) events.push_back(std::exchange(state.fenceEvent, nullptr));
        if (state.crossQueueFenceEvent) events.push_back(std::exchange(state.crossQueueFenceEvent, nullptr));
        state.currentFenceValue = state.crossQueueFenceValue = 0;
        state.overlayCompletionValue = 0;
        state.gpuCompletionUnprovable = false;
        state.allocIndex = 0;
        state.slotPicker.Reset();
    }
    if (views) {
        take(state.rtvDescHeap); take(state.srvDescHeap); take(state.offscreenRT);
        take(state.offscreenRtvHeap); take(state.postFSRProbeScratch); take(g_DummyBackBuffer);
        state.viewsSubmitted = false;
    }
    retired.Retire(std::move(objects), std::move(events), fence, value, unprovable, views,
                   completionFence, completionValue);
    pending.store(retired.Pending(), std::memory_order_release);
    if (retired.Pending()) {
        const size_t retainedCount = retired.RetainedCount();
        const size_t unprovableCount = retired.UnprovableCount();
        static ce::log_meter::ChangeGate deferred;
        const auto verdict =
            deferred.Observe(ce::log_meter::FieldKey(fence, value, unprovable, retainedCount, unprovableCount));
        if (verdict)
            HookLogImportant(
                "[OverlayRetirement] generation=retained fence=%p value=%llu unprovable=%d retained=%zu "
                "unprovableTotal=%zu escalated=%d%s",
                fence, static_cast<unsigned long long>(value), unprovable ? 1 : 0, retainedCount, unprovableCount,
                (unprovableCount > 0 || retainedCount > 1) ? 1 : 0,
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}
