#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Who a DXGI swapchain vtable slot really belongs to, for the refused-resize
// diagnostics (dxgi_shared_resize.cpp).
//
// Slot-hooking overlays (Steam) track a swapchain only through the hooks they
// wrote into these slots, and release their back-buffer references only in
// their own ResizeBuffers hook. Whether that hook is in the slot at the moment
// a resize is refused is therefore the first question a refused resize asks,
// and a slot can hand the call on in two ways: the pointer itself names another
// module, or it names DXGI's function whose first instruction jumps elsewhere
// (CE's own ResizeBuffers body hook, for one).
namespace ce::vtable_slot_owner {

struct SwapchainSlot {
    size_t index;
    const char* method;
};

// IDXGISwapChain/1/3 methods an overlay hooks to draw and to follow resizes.
inline constexpr SwapchainSlot kOverlayHookedSwapchainSlots[] = {
    {8, "Present"}, {9, "GetBuffer"}, {13, "ResizeBuffers"}, {22, "Present1"}, {39, "ResizeBuffers1"},
};

// Bytes EntryJumpTarget reads at `entry` (the FF 25 form also reads the pointer
// it names, which the caller has to check separately via IndirectJumpSlot).
inline constexpr size_t kEntryJumpBytes = 6;

// Address of the pointer an `FF 25 disp32` entry jumps through, or nullptr.
inline const void* IndirectJumpSlot(const void* entry) {
    const uint8_t* bytes = static_cast<const uint8_t*>(entry);
    if (!bytes || bytes[0] != 0xFF || bytes[1] != 0x25) {
        return nullptr;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 2, sizeof(displacement));
    return bytes + 6 + displacement;
}

// Where a function whose entry is a relative (E9) or RIP-indirect (FF 25) jump
// sends the call, or nullptr when the entry is not one of those shapes.
inline const void* EntryJumpTarget(const void* entry) {
    const uint8_t* bytes = static_cast<const uint8_t*>(entry);
    if (!bytes) {
        return nullptr;
    }
    if (bytes[0] == 0xE9) {
        int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        return bytes + 5 + displacement;
    }
    if (const void* slot = IndirectJumpSlot(entry)) {
        const void* target = nullptr;
        std::memcpy(static_cast<void*>(&target), slot, sizeof(target));
        return target;
    }
    return nullptr;
}

}  // namespace ce::vtable_slot_owner
