#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "hook/hooking/inline_hook.h"

class StreamlineInlineHookBatch {
public:
    using ForeignEntryPolicy = InlineHook::ForeignEntryPolicy;

    // kLeaveToOwner: an export whose entry another component already patched is recorded in
    // `targetSlot` but not hooked (`installedFlag` stays false, `original` keeps the export it was
    // seeded with). See ShouldLeaveForeignOwnedStreamlineEntry.
    template <typename T>
    bool Queue(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
               std::atomic<void*>& targetSlot, const char* hookName,
               ForeignEntryPolicy foreignEntry = ForeignEntryPolicy::kPrepend) {
        return QueueRaw(target, detour, reinterpret_cast<void* volatile*>(&original), installedFlag,
                        targetSlot, hookName, foreignEntry);
    }

    size_t Commit();

private:
    static constexpr size_t kMaxEntries = 9;
    struct Entry {
        void* target = nullptr;
        void* detour = nullptr;
        void* volatile* original = nullptr;
        void* fallback = nullptr;
        std::atomic<bool>* installedFlag = nullptr;
        std::atomic<void*>* targetSlot = nullptr;
        const char* hookName = nullptr;
        void* trampoline = nullptr;
        ForeignEntryPolicy foreignEntry = ForeignEntryPolicy::kPrepend;
    };

    bool QueueRaw(void* target, void* detour, void* volatile* original,
                  std::atomic<bool>& installedFlag, std::atomic<void*>& targetSlot,
                  const char* hookName, ForeignEntryPolicy foreignEntry);
    bool InstallOverflowEntry(const Entry& queued);
    // Publishes one entry's outcome to its slot; true when CE's hook is installed.
    static bool FinishEntry(const Entry& entry, const InlineHook::PublishedHookSpec& hook);

    std::array<Entry, kMaxEntries> entries_ = {};
    size_t entryCount_ = 0;
};
