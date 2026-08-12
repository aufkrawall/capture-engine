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

}  // namespace ce::dx12_factory_slot
