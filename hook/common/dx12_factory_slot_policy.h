#pragma once

#include <cstddef>
#include <cstdint>

// Ownership rules for the raw IDXGIFactory2::CreateSwapChainForHwnd slot call
// the temp-swapchain installer performs.
//
// `dx12_hook_oCreateSwapChainForHwnd` is the pre-patch value of one specific
// factory vtable slot, saved together with the vtable it was taken from. That
// slot function interprets its first argument as an object of that vtable's
// class, so passing any other object — e.g. a ReShade-style factory proxy
// returned by a hooked CreateDXGIFactory1 — reads garbage C++ fields and
// crashes inside dxgi (sessions 20260813_004853 / 20260813_004923: the proxy's
// +0xE8 is not the CDXGIFactory adapter table). The call is legal exactly when
// the factory object's vtable pointer equals the vtable the saved slot was
// captured from.
namespace ce::dx12_factory_slot {

inline bool ShouldInvokeSavedCreateSwapChainForHwndSlot(const void* savedSlotVtable,
                                                        const void* factoryObject) {
    if (savedSlotVtable == nullptr || factoryObject == nullptr) {
        return false;
    }
    const void* const* objectVtable = static_cast<const void* const*>(factoryObject);
    return *objectVtable == savedSlotVtable;
}

// True when `entry` begins with the two foreign hook shapes CE's bypass
// trampolines understand: a relative E9 jump or the x64 indirect FF 25 entry
// used by Microsoft Detours and common custom hooks.
inline bool HasForeignEntryJump(const void* entry) {
    if (entry == nullptr) {
        return false;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(entry);
    return bytes[0] == 0xE9 || (bytes[0] == 0xFF && bytes[1] == 0x25);
}

// Who owns the CreateSwapChainForHwnd ENTRY when another overlay patched it first.
//
// CE used to prepend itself over a foreign entry patch and keep the foreign jump as its
// "original". Session 20261001_042335 (Witcher 3, Steam overlay loaded before CE, CE installing
// 0.5 s after process start): CE overwrote Steam's patch while Steam was still installing its
// hooks. Steam abandoned that one hook - its saved original for exactly that relay stub is null
// while its neighbours are set - and the first real swapchain create ran CE -> Steam's handler ->
// 0x0. Overwriting a foreign patch races the foreign hooker's own transaction, so CE leaves the
// entry alone and intercepts below the chain with the deep body hook. The prepend remains only
// the fallback when no below-chain hook could be placed.
inline bool ShouldPrependCreateSwapChainForHwndEntry(bool foreignEntryJumpAtInstall, bool belowChainHookInstalled) {
    return !foreignEntryJumpAtInstall || !belowChainHookInstalled;
}

// A loaded overlay owns the CreateSwapChainForHwnd entry even before it has patched it - the same
// rule as for Present (ShouldLeavePresentEntryToForeignOverlayChain). Steam patches this entry from
// inside its CreateDXGIFactory1 handler and skips any entry that already jumps into another module.
// Session 20261001_044939 (Witcher 3, Steam loaded before CE): CE's factory discovery no longer
// runs Steam's handler, so CE sampled a pristine entry, prepended, and Steam - hooking it a second
// later from the game's own factory creation - found CE's jump, skipped the function, never saw
// the game's swapchain and drew nothing all session. The next launch (20261001_045157) Steam won
// the race and its overlay worked. Sampling decided only who came first.
inline bool IsCreateSwapChainForHwndEntryForeignOwned(bool foreignEntryJumpVisible, size_t loadedOverlayModuleCount) {
    return foreignEntryJumpVisible || loadedOverlayModuleCount >= 1;
}

// Entry span the below-chain body hook must clear. A visible patch measures itself (0 = let the
// deep hook read it); an owner that has not patched yet gets the widest form CE recognizes, since
// a body hook deeper than the eventual foreign patch is always safe and one inside it never is.
inline constexpr int kWidestForeignEntryPatchSize = 14;
inline int CreateSwapChainForHwndBelowChainPatchSpan(bool foreignEntryJumpVisible, size_t loadedOverlayModuleCount) {
    return (!foreignEntryJumpVisible && loadedOverlayModuleCount >= 1) ? kWidestForeignEntryPatchSize : 0;
}

// With no CE entry patch, a create CE forwarded from its factory vtable detour reaches CE again
// only at the below-chain hook. That call must get the entry detour's full handling (post-FSR
// Streamline handoff, descriptor overrides, side-effect ownership), exactly as it did when CE
// owned the entry. A nested access-denied retry stays a plain pass-through, and a call that did
// not come through CE's vtable keeps the below-chain hook's own handling.
inline bool ShouldBelowChainHookRunEntrySemantics(bool forwardedFromVtableDetour, bool accessDeniedRetryInFlight,
                                                  bool entryPrependInstalled) {
    return forwardedFromVtableDetour && !accessDeniedRetryInFlight && !entryPrependInstalled;
}

}  // namespace ce::dx12_factory_slot
