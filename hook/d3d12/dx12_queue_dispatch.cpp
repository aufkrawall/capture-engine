#include "dx12_queue_dispatch.h"

#include "dx12_overlay_policy/queue_dispatch_policy.h"
#include "execute_dispatch_registry.h"
#include "dx12_hook_internal.h"
#include "hook/hooking/inline_hook.h"
#include "common/logging/log_meter.h"

#include <map>
#include <mutex>
#include <vector>

namespace ce::dx12_queue_dispatch {
namespace {
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
ce::dx12::ExecuteDispatchRegistry<ExecuteCommandListsPtr> registry;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
ce::dx12::ExecuteDispatchRegistry<SignalPtr> signalRegistry;

template <typename Target>
class InlineMethodTable {
public:
    void Record(void** vtable, Target nativeMethod, Target trampoline) {
        if (!vtable || !nativeMethod || !trampoline)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        vtableNative_[vtable] = nativeMethod;
        for (auto& pair : pairs_) {
            if (pair.nativeMethod == nativeMethod) {
                pair.trampoline = trampoline;
                return;
            }
        }
        pairs_.push_back({nativeMethod, trampoline});
    }

    bool TryGetBoundNative(void** vtable, Target* outNative) const {
        if (!vtable)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = vtableNative_.find(vtable);
        if (it == vtableNative_.end())
            return false;
        if (outNative)
            *outNative = it->second;
        return true;
    }

    Target TrampolineForNative(Target nativeMethod) const {
        if (!nativeMethod)
            return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : pairs_) {
            if (pair.nativeMethod == nativeMethod)
                return pair.trampoline;
        }
        return nullptr;
    }

    Target NativeForCallable(Target callable) const {
        if (!callable)
            return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& pair : pairs_) {
            if (pair.trampoline == callable)
                return pair.nativeMethod;
        }
        return callable;
    }

    void ClearVTableBindings() {
        std::lock_guard<std::mutex> lock(mutex_);
        // Keep pairs_ for process lifetime: installed inline hooks remain live in
        // D3D12Core.dll, so Resolve() and CallableMethodForNative() must still
        // recover their exact trampolines after Reset().
        vtableNative_.clear();
    }

private:
    struct Pair {
        Target nativeMethod = nullptr;
        Target trampoline = nullptr;
    };
    mutable std::mutex mutex_;
    std::vector<Pair> pairs_;
    std::map<void**, Target> vtableNative_;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
InlineMethodTable<ExecuteCommandListsPtr> inlineEclTable;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - process-lifetime synchronization must initialize before hooks are callable.
InlineMethodTable<SignalPtr> inlineSignalTable;

ExecuteCommandListsPtr ReadSlot(void** vtable) {
    return reinterpret_cast<ExecuteCommandListsPtr>(std::atomic_ref<void*>(vtable[10]).load(std::memory_order_acquire));
}

SignalPtr ReadSignalSlot(void** vtable) {
    return reinterpret_cast<SignalPtr>(std::atomic_ref<void*>(vtable[14]).load(std::memory_order_acquire));
}

bool ShouldUseInlineHook(void** vtable, const void* methodAddress) {
    char vtablePath[MAX_PATH] = {};
    char methodPath[MAX_PATH] = {};
    const bool vtableResolved =
        TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(vtable), vtablePath, sizeof(vtablePath));
    const bool methodResolved = TryGetModulePathFromCodeAddress(methodAddress, methodPath, sizeof(methodPath));
    return ce::dx12_overlay_policy::ShouldInlineHookCommandQueueMethod(vtableResolved, vtablePath, methodResolved,
                                                                       methodPath);
}

template <typename Target>
void PublishTrampolineToOriginal(void* trampoline, void* context) {
    auto* targetSlot = static_cast<Target*>(context);
    if (!targetSlot)
        return;
    std::atomic_ref<Target>(*targetSlot).store(reinterpret_cast<Target>(trampoline), std::memory_order_release);
}

template <typename Target>
bool TryInstallInlineQueueHook(void** vtable, Target current, Target detour, Target* original,
                               InlineMethodTable<Target>& inlineTable, const char* methodName, size_t slotIndex) {
    void* trampoline = nullptr;
    if (InlineHook::TryGetInstalledTrampoline(reinterpret_cast<void*>(current), reinterpret_cast<void*>(detour),
                                              &trampoline) &&
        trampoline) {
        *original = reinterpret_cast<Target>(trampoline);
        inlineTable.Record(vtable, current, *original);
        return true;
    }
    if (InlineHook::InstallPublished(reinterpret_cast<void*>(current), reinterpret_cast<void*>(detour), &trampoline,
                                     &PublishTrampolineToOriginal<Target>, original) &&
        trampoline) {
        *original = reinterpret_cast<Target>(trampoline);
        inlineTable.Record(vtable, current, *original);
        static ce::log_meter::StreamChangeGate<8> s_inlineLogGate;
        if (const auto verdict = s_inlineLogGate.Observe(ce::log_meter::FieldKey(methodName, current),
                                                         ce::log_meter::FieldKey(trampoline, vtable))) {
            HookLogImportant(
                "DX12 %s: inline-hooked native D3D12 method at %p (trampoline=%p, vtable=%p) - vtable[%zu] preserved "
                "untouched%s",
                methodName, reinterpret_cast<void*>(current), trampoline, static_cast<void*>(vtable), slotIndex,
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return true;
    }
    static ce::log_meter::StreamChangeGate<4> s_inlineFallbackGate;
    if (const auto verdict = s_inlineFallbackGate.Observe(ce::log_meter::FieldKey(methodName, current),
                                                          ce::log_meter::FieldKey(vtable))) {
        HookLogImportant(
            "DX12 %s: inline hook on native D3D12 method %p unavailable (vtable=%p) - falling back to vtable[%zu] "
            "hook%s",
            methodName, reinterpret_cast<void*>(current), static_cast<void*>(vtable), slotIndex,
            ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return false;
}
}  // namespace

Capture CaptureVTable(void** vtable) {
    if (!vtable)
        return {CaptureResult::kFailed};
    const ExecuteCommandListsPtr current = ReadSlot(vtable);
    ExecuteCommandListsPtr boundNative = nullptr;
    if (registry.HasBinding(vtable) && inlineEclTable.TryGetBoundNative(vtable, &boundNative)) {
        if (current == boundNative)
            return {CaptureResult::kKnown, boundNative};
        return {CaptureResult::kFollower, boundNative};
    }

    const bool preferInline = ShouldUseInlineHook(vtable, reinterpret_cast<const void*>(current));
    const auto result =
        registry.Install(vtable, current, &DetourExecuteCommandLists, [&](ExecuteCommandListsPtr* original) {
            if (preferInline && TryInstallInlineQueueHook(vtable, current, &DetourExecuteCommandLists, original,
                                                          inlineEclTable, "ECL", 10)) {
                return true;
            }
            return VTableHook::Create(static_cast<void*>(&vtable[10]), reinterpret_cast<void*>(&DetourExecuteCommandLists),
                                      reinterpret_cast<void**>(original)) == VTableHook::Success;
        });
    using Result = decltype(registry)::InstallResult;
    const ExecuteCommandListsPtr reportedOriginal = inlineEclTable.NativeForCallable(result.original);
    switch (result.result) {
        case Result::kCaptured:
            return {CaptureResult::kCaptured, reportedOriginal};
        case Result::kKnown:
            return {CaptureResult::kKnown, reportedOriginal};
        case Result::kFollower:
            return {CaptureResult::kFollower, reportedOriginal};
        case Result::kRetired:
            return {CaptureResult::kRetired, reportedOriginal};
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
            VTableHook::GetOriginal(static_cast<void*>(&vtable[10]), reinterpret_cast<void*>(&DetourExecuteCommandLists),
                                    &original);
            if (original)
                return reinterpret_cast<ExecuteCommandListsPtr>(original);
            const ExecuteCommandListsPtr live = ReadSlot(vtable);
            if (const ExecuteCommandListsPtr tramp = inlineEclTable.TrampolineForNative(live))
                return tramp;
            void* installedTramp = nullptr;
            if (live && InlineHook::TryGetInstalledTrampoline(reinterpret_cast<void*>(live),
                                                              reinterpret_cast<void*>(&DetourExecuteCommandLists),
                                                              &installedTramp)) {
                return reinterpret_cast<ExecuteCommandListsPtr>(installedTramp);
            }
            return static_cast<ExecuteCommandListsPtr>(nullptr);
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
    const SignalPtr current = ReadSignalSlot(vtable);
    SignalPtr boundNative = nullptr;
    if (signalRegistry.HasBinding(vtable) && inlineSignalTable.TryGetBoundNative(vtable, &boundNative)) {
        if (current == boundNative)
            return {CaptureResult::kKnown, boundNative};
        return {CaptureResult::kFollower, boundNative};
    }

    const bool preferInline = ShouldUseInlineHook(vtable, reinterpret_cast<const void*>(current));
    const auto result =
        signalRegistry.Install(vtable, current, &DetourTraceCommandQueueSignal, [&](SignalPtr* original) {
            if (preferInline && TryInstallInlineQueueHook(vtable, current, &DetourTraceCommandQueueSignal, original,
                                                          inlineSignalTable, "Signal", 14)) {
                return true;
            }
            return VTableHook::Create(static_cast<void*>(&vtable[14]),
                                      reinterpret_cast<void*>(&DetourTraceCommandQueueSignal),
                                      reinterpret_cast<void**>(original)) == VTableHook::Success;
        });
    using Result = decltype(signalRegistry)::InstallResult;
    const SignalPtr reportedOriginal = inlineSignalTable.NativeForCallable(result.original);
    switch (result.result) {
        case Result::kCaptured:
            return {CaptureResult::kCaptured, reportedOriginal};
        case Result::kKnown:
            return {CaptureResult::kKnown, reportedOriginal};
        case Result::kFollower:
            return {CaptureResult::kFollower, reportedOriginal};
        case Result::kRetired:
            return {CaptureResult::kRetired, reportedOriginal};
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
                VTableHook::GetOriginal(static_cast<void*>(&vtable[14]),
                                        reinterpret_cast<void*>(&DetourTraceCommandQueueSignal), &original);
                if (original)
                    return reinterpret_cast<SignalPtr>(original);
                const SignalPtr live = ReadSignalSlot(vtable);
                if (const SignalPtr tramp = inlineSignalTable.TrampolineForNative(live))
                    return tramp;
                void* installedTramp = nullptr;
                if (live && InlineHook::TryGetInstalledTrampoline(reinterpret_cast<void*>(live),
                                                                  reinterpret_cast<void*>(&DetourTraceCommandQueueSignal),
                                                                  &installedTramp)) {
                    return reinterpret_cast<SignalPtr>(installedTramp);
                }
                return static_cast<SignalPtr>(nullptr);
            },
            [&] { return ReadSignalSlot(vtable); });
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

ExecuteCommandListsPtr CallableMethodForNative(ExecuteCommandListsPtr nativeMethod) {
    if (!nativeMethod)
        return nullptr;
    if (const ExecuteCommandListsPtr tramp = inlineEclTable.TrampolineForNative(nativeMethod))
        return tramp;
    return nativeMethod;
}

SignalPtr CallableSignalForNative(SignalPtr nativeMethod) {
    if (!nativeMethod)
        return nullptr;
    if (const SignalPtr tramp = inlineSignalTable.TrampolineForNative(nativeMethod))
        return tramp;
    return nativeMethod;
}

ExecuteCommandListsPtr NativeMethodForCallable(ExecuteCommandListsPtr callable) {
    return inlineEclTable.NativeForCallable(callable);
}

bool MethodsShareImplementation(ExecuteCommandListsPtr a, ExecuteCommandListsPtr b) {
    if (!a || !b)
        return false;
    return a == b || NativeMethodForCallable(a) == NativeMethodForCallable(b);
}

bool HasBinding(ID3D12CommandQueue* queue) {
    return queue && registry.HasBinding(*reinterpret_cast<void***>(queue));
}

std::vector<Binding> Snapshot() {
    std::vector<Binding> result;
    for (const auto& entry : registry.Snapshot())
        result.push_back({entry.vtable, inlineEclTable.NativeForCallable(entry.original)});
    return result;
}
uint64_t Generation() {
    return registry.Generation();
}
void Reset() {
    registry.Reset();
    signalRegistry.Reset();
    inlineEclTable.ClearVTableBindings();
    inlineSignalTable.ClearVTableBindings();
}

}  // namespace ce::dx12_queue_dispatch
