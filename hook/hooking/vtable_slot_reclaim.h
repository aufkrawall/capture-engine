#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "vtable_hook_policy.h"

// Putting CE's hooks back into a dispatch table its owner rewrote.
//
// Windows' d3d9 keeps each device's dispatch table in the device's own memory
// and copies a fresh one over it in BeginStateBlock and again in EndStateBlock,
// erasing every hook any tool wrote into it (tests/test_d3d9_dispatch_table_rewrite.cpp).
// A slot that now holds exactly the function CE saved as its original was put
// back by that rewrite: writing CE's detour into it restores the chain as it
// was, since the detour forwards to that very function. A slot that holds
// anything else is left alone - a mode-specific entry (a device created without
// D3DCREATE_MULTITHREADED records SetTexture and SetSamplerState through other
// functions while a block is recorded) or another tool's hook.

namespace ce::vtable_slot_reclaim {

struct OwnedSlot {
    void** slot = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
};

struct Counts {
    uint32_t restored = 0;
    // Rewritten to something other than CE's original.
    uint32_t leftAlone = 0;
};

class SlotWriter {
public:
    // Swaps `expected` for `value`; a table in a read-only image is unprotected
    // for the write. One query per memory region.
    bool Write(void** slot, void* expected, void* value) {
        const auto address = reinterpret_cast<uintptr_t>(slot);
        if (address < regionBegin_ || address + sizeof(void*) > regionEnd_) {
            MEMORY_BASIC_INFORMATION memory = {};
            if (VirtualQuery(slot, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_COMMIT ||
                (memory.Protect & PAGE_GUARD) != 0)
                return false;
            const DWORD access = memory.Protect & 0xFFu;
            regionBegin_ = reinterpret_cast<uintptr_t>(memory.BaseAddress);
            regionEnd_ = regionBegin_ + memory.RegionSize;
            writable_ = access == PAGE_READWRITE || access == PAGE_EXECUTE_READWRITE;
        }
        DWORD previous = 0;
        if (!writable_ && !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous))
            return false;
        const void* replaced =
            InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot), value, expected);
        if (!writable_)
            VirtualProtect(slot, sizeof(void*), previous, &previous);
        return replaced == expected;
    }

private:
    uintptr_t regionBegin_ = 0;
    uintptr_t regionEnd_ = 0;
    bool writable_ = false;
};

inline Counts Reclaim(const OwnedSlot* slots, size_t count) {
    Counts counts;
    SlotWriter writer;
    for (size_t i = 0; i < count; ++i) {
        const OwnedSlot& owned = slots[i];
        void* const current = *owned.slot;
        if (current == owned.detour)
            continue;
        if (ce::vtable_hook_policy::ShouldReclaimRestoredSlot(current, owned.detour, owned.original) &&
            writer.Write(owned.slot, current, owned.detour)) {
            ++counts.restored;
        } else {
            ++counts.leftAlone;
        }
    }
    return counts;
}

}  // namespace ce::vtable_slot_reclaim
