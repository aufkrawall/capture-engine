#pragma once

#include <array>
#include <atomic>
#include <cstddef>

class StreamlineInlineHookBatch {
public:
    template <typename T>
    bool Queue(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
               std::atomic<void*>& targetSlot, const char* hookName) {
        return QueueRaw(target, detour, reinterpret_cast<void* volatile*>(&original), installedFlag,
                        targetSlot, hookName);
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
    };

    bool QueueRaw(void* target, void* detour, void* volatile* original,
                  std::atomic<bool>& installedFlag, std::atomic<void*>& targetSlot,
                  const char* hookName);
    bool InstallOverflowEntry(void* target, void* detour, void* volatile* original,
                              std::atomic<bool>& installedFlag, std::atomic<void*>& targetSlot,
                              const char* hookName);

    std::array<Entry, kMaxEntries> entries_ = {};
    size_t entryCount_ = 0;
};
