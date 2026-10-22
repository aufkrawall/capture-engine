#pragma once

// The overlay's retained render-target views: one per back buffer of the swapchain CE draws on.
// Views of one swapchain stay until it changes, resizes or tears down.

#include <windows.h>
#include <dxgi.h>

#include <cstddef>

// The back buffer CE's D3D10/D3D11 paths (overlay, sharpen, capture, screenshots) draw on and read:
// buffer 0, the one a D3D10/D3D11 application renders into on every swap effect. Windows keeps the
// current back buffer at index 0 (FLIP_DISCARD refuses the others, FLIP_SEQUENTIAL hands them out
// read-only; FlowD3D11FlipBufferIndex), and so does DXVK. A translator below D3D11 can report its own
// chain's index through IDXGISwapChain3::GetCurrentBackBufferIndex: DXUP's D3D12-backed chain says
// 0/1 while refusing GetBuffer(1). CE asked for that index until 2026-10-22 and lost the overlay on
// every second Present, recreating a view and writing two log lines each time (Mirror's Edge with
// DXUP, 20261022_162106: 3388 of 6788 Presents; with DXUP 0.8.1 the odd index was a real buffer its
// Present then overwrote, 20261018_143747).
inline constexpr UINT kDX11ApplicationBackBuffer = 0;

template <typename View>
class BackBufferViewCache {
public:
    static constexpr UINT kMaxBuffers = DXGI_MAX_SWAP_CHAIN_BUFFERS;

    // The view retained for this swapchain's buffer, or null when it has to be created.
    View* Find(const void* swapChain, UINT bufferIndex) const {
        if (!swapChain || swapChain != swapChain_ || bufferIndex >= kMaxBuffers)
            return nullptr;
        return views_[bufferIndex];
    }

    // Takes over the caller's reference. A view of another swapchain releases every retained
    // view first. An index past the cache is refused and the caller keeps its reference.
    template <typename Release>
    bool Store(const void* swapChain, UINT bufferIndex, View* view, Release&& release) {
        if (!swapChain || !view || bufferIndex >= kMaxBuffers)
            return false;
        if (swapChain != swapChain_) {
            Clear(release);
            swapChain_ = swapChain;
        }
        if (views_[bufferIndex])
            release(views_[bufferIndex]);
        views_[bufferIndex] = view;
        return true;
    }

    // Hands every retained view to `release` (a no-op when the device is already gone).
    template <typename Release>
    void Clear(Release&& release) {
        for (View*& view : views_) {
            if (view) {
                release(view);
                view = nullptr;
            }
        }
        swapChain_ = nullptr;
    }

    size_t Count() const {
        size_t count = 0;
        for (View* view : views_)
            count += view ? 1u : 0u;
        return count;
    }

    const void* SwapChain() const { return swapChain_; }

private:
    const void* swapChain_ = nullptr;
    View* views_[kMaxBuffers] = {};
};
