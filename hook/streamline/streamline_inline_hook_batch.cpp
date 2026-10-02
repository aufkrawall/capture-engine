#include "streamline_inline_hook_batch.h"

#include "streamline_hook_internal.h"

namespace {

struct StreamlineBatchPublication {
    void* volatile* destination = nullptr;
    void* fallback = nullptr;
};

void PublishStreamlineBatchTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<StreamlineBatchPublication*>(context);
    InterlockedExchangePointer(publication->destination, trampoline ? trampoline : publication->fallback);
}

void LogInstallFailure(const char* hookName, void* target) {
    static std::atomic<uint32_t> s_installFailureCount{0};
    const uint32_t failureCount = s_installFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(failureCount, 10, 300)) {
        HookLogImportant("Streamline Hook: Failed to inline hook %s at %p (attempt=%u)", hookName, target,
                         failureCount);
    }
}

}  // namespace

bool StreamlineInlineHookBatch::QueueRaw(void* target, void* detour, void* volatile* original,
                                         std::atomic<bool>& installedFlag, std::atomic<void*>& targetSlot,
                                         const char* hookName) {
    if (!target || !original) {
        return false;
    }
    if (target == detour) {
        InterlockedExchangePointer(original, nullptr);
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    const bool slotInstalled = installedFlag.load(std::memory_order_acquire);
    if (slotInstalled && installedTarget == target) {
        return false;
    }
    if (!ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(
            slotInstalled, installedTarget, target,
            installedTarget != nullptr && DoesAddressBelongToLoadedModule(
                                              const_cast<void*>(installedTarget), nullptr, nullptr, 0, nullptr))) {
        static std::atomic<uint32_t> s_refusedRetargetCount{0};
        const uint32_t refusedCount = s_refusedRetargetCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(refusedCount, 10, 300)) {
            HookLogImportant(
                "Streamline Hook: Refusing to retarget %s from %p to %p — the installed target is still mapped, "
                "so a second live instance would take over CE's single forward pointer (count=%u)",
                hookName, installedTarget, target, refusedCount);
        }
        return false;
    }

    void* retainedTrampoline = nullptr;
    if (InlineHook::TryGetInstalledTrampoline(target, detour, &retainedTrampoline)) {
        InterlockedExchangePointer(original, retainedTrampoline);
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        HookLogImportant(
            "Streamline Hook: Reconciled rediscovered %s at %p with CE's retained live hook (trampoline=%p)",
            hookName, target, retainedTrampoline);
        return true;
    }

    if (entryCount_ == entries_.size()) {
        return InstallOverflowEntry(target, detour, original, installedFlag, targetSlot, hookName);
    }
    Entry& entry = entries_[entryCount_++];
    entry.target = target;
    entry.detour = detour;
    entry.original = original;
    entry.fallback = InterlockedCompareExchangePointer(original, nullptr, nullptr);
    entry.installedFlag = &installedFlag;
    entry.targetSlot = &targetSlot;
    entry.hookName = hookName;
    return false;
}

bool StreamlineInlineHookBatch::InstallOverflowEntry(void* target, void* detour, void* volatile* original,
                                                      std::atomic<bool>& installedFlag,
                                                      std::atomic<void*>& targetSlot, const char* hookName) {
    StreamlineBatchPublication publication{original, InterlockedCompareExchangePointer(original, nullptr, nullptr)};
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishStreamlineBatchTrampoline,
                                      &publication)) {
        LogInstallFailure(hookName, target);
        return false;
    }
    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target,
                     trampoline);
    return true;
}

size_t StreamlineInlineHookBatch::Commit() {
    std::array<StreamlineBatchPublication, kMaxEntries> publications = {};
    std::array<InlineHook::PublishedHookSpec, kMaxEntries> hooks = {};
    for (size_t i = 0; i < entryCount_; ++i) {
        Entry& entry = entries_[i];
        publications[i] = {entry.original, entry.fallback};
        hooks[i] = {entry.target, entry.detour, &entry.trampoline, PublishStreamlineBatchTrampoline,
                    &publications[i]};
    }
    InlineHook::InstallPublishedBatch(hooks.data(), entryCount_);

    size_t installedCount = 0;
    for (size_t i = 0; i < entryCount_; ++i) {
        Entry& entry = entries_[i];
        if (hooks[i].installed && entry.trampoline) {
            entry.targetSlot->store(entry.target, std::memory_order_release);
            entry.installedFlag->store(true, std::memory_order_release);
            HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", entry.hookName,
                             entry.target, entry.trampoline);
            ++installedCount;
        } else {
            LogInstallFailure(entry.hookName, entry.target);
        }
    }
    return installedCount;
}
