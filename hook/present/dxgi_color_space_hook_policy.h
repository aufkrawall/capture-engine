#pragma once

#include <cstddef>

// Steam follows the function in swapchain slot 38 when installing its SetColorSpace1 hook.
// CE's entry jump led it to CE's endbr64 prolog, which Steam could not decode (Witcher 3,
// 2026-10-01). Like the factory-create hooks, leave the entry to a loaded overlay even before
// it patches it, and intercept the body instead. The entry stays the coverage fallback only
// when the body hook is refused.
namespace ce::dxgi_color_space_hook {

inline bool IsSetColorSpace1EntryForeignOwned(bool foreignEntryJumpVisible, size_t loadedOverlayModuleCount) {
    return foreignEntryJumpVisible || loadedOverlayModuleCount >= 1;
}

// A visible jump measures itself; an unpatched owner needs room for FF 25 + its 8-byte target.
inline constexpr int kWidestForeignEntryPatchSize = 14;

inline int SetColorSpace1BelowChainPatchSpan(bool foreignEntryJumpVisible, size_t loadedOverlayModuleCount) {
    return (!foreignEntryJumpVisible && loadedOverlayModuleCount >= 1) ? kWidestForeignEntryPatchSize : 0;
}

inline bool ShouldPrependSetColorSpace1Entry(bool entryForeignOwned, bool belowChainHookInstalled) {
    return !entryForeignOwned || !belowChainHookInstalled;
}

}  // namespace ce::dxgi_color_space_hook
