#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

namespace ce::vulkan_dxgi_fifo_registry {

// Capacity of the open-addressed pointer table. A bounded table keeps the
// present-gate lookup a fixed handful of atomic loads; the value must be a
// power of two for the probe mask below.
inline constexpr size_t kCapacity = 64;
static_assert((kCapacity & (kCapacity - 1)) == 0, "open addressing needs a power-of-two capacity");

inline constexpr size_t kInvalidSlot = ~static_cast<size_t>(0);

// Lock-free registry of DXGI swapchain instance pointers observed at
// successful targeted creation (CreateSwapChain / CreateSwapChainForHwnd /
// CreateSwapChainForCoreWindow / CreateSwapChainForComposition). Membership
// is what authorizes rewriting the final system present: a swapchain these
// detours never saw - another overlay's, a capture utility's, anything not
// born from a watched system factory method - always passes through with its
// parameters untouched, even while the FIFO backstop is armed.
//
// No COM reference is taken: entries are raw interface pointers observed on
// the caller's already-live object, never dereferenced after the fact, and
// never released. A destroyed swapchain leaves its address registered; that
// is the intended recreation contract, because a fresh swapchain allocated at
// the same address is re-registered (refreshed) by its own creation call, and
// the rewrite itself is additionally gated by the armed/lifecycle policy. The
// table never erases, so probe chains only grow and Find can stop at the
// first empty slot.
//
// Fail-closed capacity: when the table is full, Register returns kInvalidSlot
// and the caller leaves that swapchain's presents untouched rather than
// falling back to an unscoped rewrite.
class ObservedSwapchainRegistry {
public:
    // Registers or refreshes an observed instance. Returns the slot index, or
    // kInvalidSlot when the table is full.
    size_t Register(const void* swapchain) {
        size_t probe = Hash(swapchain);
        for (size_t attempt = 0; attempt < kCapacity; ++attempt) {
            std::atomic<const void*>& slot = slots_[probe];
            const void* observed = slot.load(std::memory_order_acquire);
            if (observed == swapchain)
                return probe;
            if (observed == nullptr) {
                const void* expected = nullptr;
                if (slot.compare_exchange_strong(expected, swapchain, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                    return probe;
                }
                if (expected == swapchain)
                    return probe;  // concurrent registration of the same instance
                // Another pointer claimed the slot first; keep probing.
            }
            probe = (probe + 1) & kSlotMask;
        }
        return kInvalidSlot;
    }

    // Returns the slot index of a registered instance, or kInvalidSlot.
    size_t Find(const void* swapchain) const {
        size_t probe = Hash(swapchain);
        for (size_t attempt = 0; attempt < kCapacity; ++attempt) {
            const void* observed = slots_[probe].load(std::memory_order_acquire);
            if (observed == swapchain)
                return probe;
            if (observed == nullptr)
                return kInvalidSlot;  // an empty slot ends the probe chain
            probe = (probe + 1) & kSlotMask;
        }
        return kInvalidSlot;
    }

    const void* PointerAt(size_t slot) const {
        return slots_[slot].load(std::memory_order_acquire);
    }

    // 1-based per-identity cadence counter for rate-limited diagnostics.
    uint32_t NextPresentationOccurrence(size_t slot) {
        return presentationCounts_[slot].fetch_add(1, std::memory_order_relaxed) + 1;
    }

private:
    static size_t Hash(const void* pointer) {
        uintptr_t bits = reinterpret_cast<uintptr_t>(pointer);
        bits ^= bits >> 15;  // heap addresses share high bits; spread them down
        bits *= UINT64_C(0x9E3779B97F4A7C15);  // wide multiply, truncated on 32-bit
        bits ^= bits >> 13;
        return static_cast<size_t>(bits) & kSlotMask;
    }

    static constexpr size_t kSlotMask = kCapacity - 1;
    std::atomic<const void*> slots_[kCapacity] = {};
    std::atomic<uint32_t> presentationCounts_[kCapacity] = {};
};

}  // namespace ce::vulkan_dxgi_fifo_registry
