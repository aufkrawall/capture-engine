#include "xefg_hook_internal.h"

#include <algorithm>

#include "hook/hooking/inline_hook.h"

namespace XeFGHook {

bool HookedRuntimeImage::IsHooked(HMODULE module) const {
    return module && module_.load(std::memory_order_acquire) == module && !unloaded_.load(std::memory_order_acquire);
}

bool HookedRuntimeImage::IsOwnedByAnotherImage(HMODULE module) const {
    const HMODULE owner = module_.load(std::memory_order_acquire);
    return owner && owner != module && !unloaded_.load(std::memory_order_acquire);
}

size_t HookedRuntimeImage::ForgetUnloaded() {
    if (!unloaded_.load(std::memory_order_acquire)) {
        return 0;
    }
    const size_t forgotten = targetCount_;
    for (size_t i = 0; i < targetCount_; ++i) {
        InlineHook::ForgetUnloadedHook(targets_[i]);
        targets_[i] = nullptr;
    }
    targetCount_ = 0;
    module_.store(nullptr, std::memory_order_release);
    unloaded_.store(false, std::memory_order_release);
    return forgotten;
}

void HookedRuntimeImage::Begin(HMODULE module) {
    // A previous image's records go only through ForgetUnloaded: an image that is still mapped keeps them.
    targetCount_ = 0;
    unloaded_.store(false, std::memory_order_release);
    module_.store(module, std::memory_order_release);
}

void HookedRuntimeImage::NoteTargets(void* const* targets, size_t count) {
    targetCount_ = std::min(count, kMaxTargets);
    std::copy(targets, targets + targetCount_, targets_);
}

bool HookedRuntimeImage::NoteUnloaded(const void* moduleBase) {
    if (!moduleBase || module_.load(std::memory_order_acquire) != moduleBase) {
        return false;
    }
    unloaded_.store(true, std::memory_order_release);
    return true;
}

}  // namespace XeFGHook
