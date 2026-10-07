#include "dx12_queue_dispatch.h"

#include "execute_dispatch_registry.h"
#include "dx12_hook_internal.h"
#include "common/logging/log_meter.h"

namespace ce::dx12_queue_dispatch {
namespace {
ce::dx12::ExecuteDispatchRegistry<ExecuteCommandListsPtr> registry;
ce::dx12::ExecuteDispatchRegistry<SignalPtr> signalRegistry;
ExecuteCommandListsPtr ReadSlot(void** vtable) {
    return reinterpret_cast<ExecuteCommandListsPtr>(std::atomic_ref<void*>(vtable[10]).load(std::memory_order_acquire));
}
}  // namespace

Capture CaptureVTable(void** vtable) {
    if (!vtable)
        return {CaptureResult::kFailed};
    const auto result =
        registry.Install(vtable, ReadSlot(vtable), &DetourExecuteCommandLists, [&](ExecuteCommandListsPtr* original) {
            return VTableHook::Create(&vtable[10], reinterpret_cast<void*>(&DetourExecuteCommandLists),
                                      reinterpret_cast<void**>(original)) == VTableHook::Success;
        });
    using Result = decltype(registry)::InstallResult;
    switch (result.result) {
        case Result::kCaptured:
            return {CaptureResult::kCaptured, result.original};
        case Result::kKnown:
            return {CaptureResult::kKnown, result.original};
        case Result::kFollower:
            return {CaptureResult::kFollower, result.original};
        case Result::kRetired:
            return {CaptureResult::kRetired, result.original};
        case Result::kFailed:
            return {CaptureResult::kFailed};
    }
    return {CaptureResult::kFailed};
}

ExecuteCommandListsPtr Resolve(ID3D12CommandQueue* queue) {
    void** vtable = queue ? *reinterpret_cast<void***>(queue) : nullptr;
    if (!vtable)
        return nullptr;
    auto target = registry.ResolveInterception(
        vtable, &DetourExecuteCommandLists,
        [&] {
            void* original = nullptr;
            VTableHook::GetOriginal(&vtable[10], reinterpret_cast<void*>(&DetourExecuteCommandLists), &original);
            return reinterpret_cast<ExecuteCommandListsPtr>(original);
        },
        [&] { return ReadSlot(vtable); });
    if (!target) {
        static ce::log_meter::ChangeGate missing;
        const auto verdict = missing.Observe(ce::log_meter::FieldKey(vtable));
        if (verdict) {
            HookLogImportant("DX12 ECL: no exact predecessor for intercepted queue=%p vtable=%p%s", queue, vtable,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return target;
}

SignalCapture CaptureSignalVTable(void** vtable) {
    if (!vtable)
        return {CaptureResult::kFailed};
    const auto current = reinterpret_cast<SignalPtr>(std::atomic_ref<void*>(vtable[14]).load(std::memory_order_acquire));
    const auto result = signalRegistry.Install(vtable, current, &DetourTraceCommandQueueSignal, [&](SignalPtr* original) {
        return VTableHook::Create(&vtable[14], reinterpret_cast<void*>(&DetourTraceCommandQueueSignal),
                                  reinterpret_cast<void**>(original)) == VTableHook::Success;
    });
    using Result = decltype(signalRegistry)::InstallResult;
    switch (result.result) {
        case Result::kCaptured:
            return {CaptureResult::kCaptured, result.original};
        case Result::kKnown:
            return {CaptureResult::kKnown, result.original};
        case Result::kFollower:
            return {CaptureResult::kFollower, result.original};
        case Result::kRetired:
            return {CaptureResult::kRetired, result.original};
        case Result::kFailed:
            return {CaptureResult::kFailed};
    }
    return {CaptureResult::kFailed};
}

SignalPtr ResolveSignal(ID3D12CommandQueue* queue) {
    void** vtable = queue ? *reinterpret_cast<void***>(queue) : nullptr;
    SignalPtr target = nullptr;
    if (vtable) {
        target = signalRegistry.ResolveInterception(
            vtable, &DetourTraceCommandQueueSignal,
            [&] {
                void* original = nullptr;
                VTableHook::GetOriginal(&vtable[14], reinterpret_cast<void*>(&DetourTraceCommandQueueSignal),
                                        &original);
                return reinterpret_cast<SignalPtr>(original);
            },
            [&] {
                return reinterpret_cast<SignalPtr>(std::atomic_ref<void*>(vtable[14]).load(std::memory_order_acquire));
            });
    }
    if (!target) {
        static ce::log_meter::ChangeGate missing;
        const auto verdict = missing.Observe(ce::log_meter::FieldKey(queue, vtable));
        if (verdict) {
            HookLogImportant("DX12 Signal: no exact predecessor for queue=%p vtable=%p%s", queue, vtable,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return target;
}

bool HasBinding(ID3D12CommandQueue* queue) {
    return queue && registry.HasBinding(*reinterpret_cast<void***>(queue));
}

std::vector<Binding> Snapshot() {
    std::vector<Binding> result;
    for (const auto& entry : registry.Snapshot())
        result.push_back({entry.vtable, entry.original});
    return result;
}
uint64_t Generation() {
    return registry.Generation();
}
void Reset() {
    registry.Reset();
    signalRegistry.Reset();
}

}  // namespace ce::dx12_queue_dispatch
