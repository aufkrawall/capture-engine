#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

struct IDXGISwapChain;

// Who takes and who returns the references on a D3D12 chain's back buffers.
//
// Talos Reawakened + FSR frame generation: ResizeBuffers is refused with three
// foreign references on each of the real chain's three buffers, only with CE
// injected, and still with CE's overlay hidden, sharpening off and capture
// unbound (logs/20260926_094906, _191350, _192017). A memory scan for the
// buffer pointers (resize_reference_holders.h) found only raw pointers - it
// cannot tell a counted reference from a stored address. This trace counts the
// references themselves: after a D3D12 chain's resize succeeds, its buffers are
// registered, and every AddRef, Release, successful QueryInterface and
// GetBuffer on them is tallied by the caller's return address. On a refused
// resize the tallies name the call sites, and the module, that took references
// and never gave them back.
//
// The hooks sit on the resource class's and the swapchain class's vtables, so
// every D3D12 resource of that class passes through a pointer compare against
// at most kMaxTrackedBuffers registered buffers - nothing else.

namespace ce::backbuffer_reference_trace {

inline constexpr size_t kMaxTrackedBuffers = 8;
inline constexpr size_t kSitesPerBuffer = 48;

struct SiteTally {
    std::atomic<uintptr_t> site{0};
    std::atomic<int32_t> acquired{0};
    std::atomic<int32_t> released{0};
};

// Lock-free insert-or-find by return address. False when the table is full;
// the caller counts that as an overflow instead of losing it silently.
inline bool Record(SiteTally* table, size_t capacity, uintptr_t site, bool acquire) {
    if (!table || site == 0) {
        return false;
    }
    for (size_t i = 0; i < capacity; ++i) {
        uintptr_t current = table[i].site.load(std::memory_order_acquire);
        if (current == 0) {
            uintptr_t expected = 0;
            if (table[i].site.compare_exchange_strong(expected, site, std::memory_order_acq_rel)) {
                current = site;
            } else {
                current = expected;
            }
        }
        if (current == site) {
            (acquire ? table[i].acquired : table[i].released).fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

inline void Clear(SiteTally* table, size_t capacity) {
    for (size_t i = 0; i < capacity; ++i) {
        table[i].acquired.store(0, std::memory_order_relaxed);
        table[i].released.store(0, std::memory_order_relaxed);
        table[i].site.store(0, std::memory_order_release);
    }
}

}  // namespace ce::backbuffer_reference_trace

// Registers the chain's current buffers (replacing any earlier registration)
// and installs the counting hooks once. Call after a successful D3D12 resize.
void BackBufferReferenceTrace_Track(IDXGISwapChain* swapChain, UINT bufferCount, const char* source);
// Logs every call site that touched the registered buffers, and the balance
// per module. Call when a resize is refused.
void BackBufferReferenceTrace_Log(IDXGISwapChain* swapChain, const char* source);
// Restores the patched vtable slots (process shutdown).
void BackBufferReferenceTrace_Uninstall();
