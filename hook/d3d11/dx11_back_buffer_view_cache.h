#pragma once

// The overlay's retained render-target views: one per back buffer of the swapchain CE draws on.
// A FLIP chain whose GetCurrentBackBufferIndex rotates hands out a different texture per index
// (DXUP's D3D12-backed D3D11 does). A single retained view keyed by the index thrashed there:
// Mirror's Edge on DXUP created a view, plus two log lines, on every one of 16438 Presents
// (session 20261018_143747). Views of one swapchain stay until it changes, resizes or tears down.

#include <windows.h>
#include <dxgi.h>

#include <cstddef>

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
