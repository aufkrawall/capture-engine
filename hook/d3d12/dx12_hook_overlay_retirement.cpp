#include "dx12_hook_internal.h"
#include "overlay_resource_retirement.h"
#include "common/logging/log_meter.h"

namespace {
ce::dx12::OverlayResourceRetirement<IUnknown, ID3D12Fence> retired;
std::atomic<bool> pending{false};
}

void PrepareOverlayGpuRetirement() {
    std::lock_guard<std::recursive_mutex> resources(dx12_hook_g_OverlayMutex);
    const UINT64 value = dx12_hook_g_deferredSignalValue.exchange(0, std::memory_order_acq_rel);
    auto* queue = dx12_hook_g_deferredSignalQueue.exchange(nullptr, std::memory_order_acq_rel);
    dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    if (!value) return;
    auto& state = dx12_hook_g_State;
    if (queue && state.fence && SUCCEEDED(queue->Signal(state.fence, value)))
        state.currentFenceValue = std::max(state.currentFenceValue, value);
    else state.gpuCompletionUnprovable = true;
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
