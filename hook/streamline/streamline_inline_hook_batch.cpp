#include "streamline_inline_hook_batch.h"

#include "streamline_foreign_entry_policy.h"
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
                                         const char* hookName, ForeignEntryPolicy foreignEntry) {
    if (!target || !original) {
        return false;
    }
    if (target == detour) {
        InterlockedExchangePointer(original, nullptr);
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* slotTarget = targetSlot.load(std::memory_order_acquire);
    const auto request = ce::streamline_runtime_policy::ResolveStreamlineHookSlotRequest(
        slotTarget, target,
        slotTarget != nullptr &&
            DoesAddressBelongToLoadedModule(const_cast<void*>(slotTarget), nullptr, nullptr, 0, nullptr));
    if (request == ce::streamline_runtime_policy::StreamlineHookSlotRequest::kAlreadyHandled) {
        return false;
    }
    if (request == ce::streamline_runtime_policy::StreamlineHookSlotRequest::kRefuseSecondInstance) {
        static std::atomic<uint32_t> s_refusedRetargetCount{0};
        const uint32_t refusedCount = s_refusedRetargetCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(refusedCount, 10, 300)) {
            HookLogImportant(
                "Streamline Hook: Refusing to retarget %s from %p to %p — the recorded target is still mapped, "
                "so a second live instance would take over CE's single forward pointer (installed=%d count=%u)",
                hookName, slotTarget, target, installedFlag.load(std::memory_order_acquire) ? 1 : 0,
                refusedCount);
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

    Entry entry;
    entry.target = target;
    entry.detour = detour;
    entry.original = original;
    entry.fallback = InterlockedCompareExchangePointer(original, nullptr, nullptr);
    entry.installedFlag = &installedFlag;
    entry.targetSlot = &targetSlot;
    entry.hookName = hookName;
    entry.foreignEntry = foreignEntry;
    if (entryCount_ == entries_.size()) {
        return InstallOverflowEntry(entry);
    }
    entries_[entryCount_++] = entry;
    return false;
}

bool StreamlineInlineHookBatch::FinishEntry(const Entry& entry, const InlineHook::PublishedHookSpec& hook) {
    if (hook.installed && entry.trampoline) {
        entry.targetSlot->store(entry.target, std::memory_order_release);
        entry.installedFlag->store(true, std::memory_order_release);
        HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", entry.hookName,
                         entry.target, entry.trampoline);
        return true;
    }
    if (hook.leftToForeignOwner) {
        // Recorded, not hooked: the slot keeps this export's module attribution and its single
        // forward pointer, and `original` keeps the export it was seeded with.
        entry.targetSlot->store(entry.target, std::memory_order_release);
        HookLogImportant(
            "Streamline Hook: Left %s at %p to the component that patched its entry first; CE observes its "
            "callers through the GetProcAddress and import hooks and forwards to that entry (original=%p)",
            entry.hookName, entry.target, InterlockedCompareExchangePointer(entry.original, nullptr, nullptr));
        return false;
    }
    LogInstallFailure(entry.hookName, entry.target);
    return false;
}

bool StreamlineInlineHookBatch::InstallOverflowEntry(const Entry& queued) {
    Entry entry = queued;
    StreamlineBatchPublication publication{entry.original, entry.fallback};
    InlineHook::PublishedHookSpec hook = {entry.target, entry.detour, &entry.trampoline,
                                          PublishStreamlineBatchTrampoline, &publication};
    hook.foreignEntry = entry.foreignEntry;
    InlineHook::InstallPublishedBatch(&hook, 1);
    return FinishEntry(entry, hook);
}

size_t StreamlineInlineHookBatch::Commit() {
    std::array<StreamlineBatchPublication, kMaxEntries> publications = {};
    std::array<InlineHook::PublishedHookSpec, kMaxEntries> hooks = {};
    for (size_t i = 0; i < entryCount_; ++i) {
        Entry& entry = entries_[i];
        publications[i] = {entry.original, entry.fallback};
        hooks[i] = {entry.target, entry.detour, &entry.trampoline, PublishStreamlineBatchTrampoline,
                    &publications[i]};
        hooks[i].foreignEntry = entry.foreignEntry;
    }
    InlineHook::InstallPublishedBatch(hooks.data(), entryCount_);

    size_t installedCount = 0;
    for (size_t i = 0; i < entryCount_; ++i) {
        if (FinishEntry(entries_[i], hooks[i])) {
            ++installedCount;
        }
    }
    return installedCount;
}
