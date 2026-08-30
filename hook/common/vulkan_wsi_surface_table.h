#pragma once

#include <atomic>
#include <cstdint>
#include <windows.h>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

namespace ce::vulkan_wsi_surfaces {

// Bounded number of simultaneously tracked Vulkan Win32 surface HWNDs. The
// value must comfortably exceed the largest number of windows a title keeps
// live Vulkan surfaces on at once (main window plus optional auxiliary
// surfaces). Registration is fail-closed: when the table is full, a new
// surface's HWND is not tracked and swapchains targeting it are never
// authorized for the FIFO rewrite.
inline constexpr size_t kMaxLiveSurfaceHwnds = 16;

// Refcounted, fixed-capacity set of HWNDs that currently back at least one live
// Vulkan Win32 surface.
//
// Concurrency contract:
// - `IsLive` is lock-free and safe on any thread at any time (acquire loads of
//   atomic slots; no allocation, no mutex, no waits). It is the only operation
//   allowed on a hot path.
// - `Register`/`Unregister` serialize internally on a spin flag. They run only
//   on the Vulkan surface create/destroy paths, which are cold, app-driven
//   events - never on the present path.
// - Multiple surfaces may share one HWND; the table refcounts per HWND, so the
//   HWND stays live until its last surface is destroyed.
// - HWND values can be recycled by the OS (ABA). The table only ever answers
//   "is this HWND a Vulkan surface target *right now*", so a recycled HWND is
//   re-authorized only while some live surface actually uses it again.
class LiveSurfaceHwndTable {
public:
    // Publishes one live surface backed by `window`. Returns false when the
    // table is full (or the window is null); the caller treats that as
    // fail-closed and the surface simply stays untracked.
    bool Register(HWND window) {
        if (!window)
            return false;
        ScopedSpin lock(spin_);
        for (size_t i = 0; i < kMaxLiveSurfaceHwnds; ++i) {
            if (slots_[i].window.load(std::memory_order_relaxed) == window) {
                slots_[i].refs.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        for (size_t i = 0; i < kMaxLiveSurfaceHwnds; ++i) {
            HWND expected = nullptr;
            if (slots_[i].window.compare_exchange_strong(expected, window, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                slots_[i].refs.store(1, std::memory_order_relaxed);
                liveWindowCount_.fetch_add(1, std::memory_order_acq_rel);
                return true;
            }
        }
        return false;
    }

    // Retires one surface backed by `window`. The HWND stops being live when
    // its last surface is retired. Returns false when the window is unknown.
    bool Unregister(HWND window) {
        if (!window)
            return false;
        ScopedSpin lock(spin_);
        for (size_t i = 0; i < kMaxLiveSurfaceHwnds; ++i) {
            if (slots_[i].window.load(std::memory_order_relaxed) != window)
                continue;
            if (slots_[i].refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                slots_[i].window.store(nullptr, std::memory_order_release);
                liveWindowCount_.fetch_sub(1, std::memory_order_acq_rel);
            }
            return true;
        }
        return false;
    }

    // Lock-free membership query. `nullptr` is never live.
    bool IsLive(HWND window) const {
        if (!window)
            return false;
        for (size_t i = 0; i < kMaxLiveSurfaceHwnds; ++i) {
            if (slots_[i].window.load(std::memory_order_acquire) == window)
                return true;
        }
        return false;
    }

    // Number of distinct HWNDs currently tracked (diagnostics/tests).
    uint32_t LiveWindowCount() const {
        return liveWindowCount_.load(std::memory_order_acquire);
    }

private:
    struct Slot {
        std::atomic<HWND> window{nullptr};
        std::atomic<uint32_t> refs{0};
    };

    // Tiny critical section for the cold create/destroy paths. Never taken on
    // the present path: readers use IsLive, which does not acquire it.
    struct ScopedSpin {
        explicit ScopedSpin(std::atomic_flag& flag) : flag_(flag) {
            while (flag_.test_and_set(std::memory_order_acquire)) {
            }
        }
        ~ScopedSpin() {
            flag_.clear(std::memory_order_release);
        }
        ScopedSpin(const ScopedSpin&) = delete;
        ScopedSpin& operator=(const ScopedSpin&) = delete;

    private:
        std::atomic_flag& flag_;
    };

    // C++20: default construction leaves the flag clear.
    std::atomic_flag spin_{};
    Slot slots_[kMaxLiveSurfaceHwnds] = {};
    std::atomic<uint32_t> liveWindowCount_{0};
};

// Pure sweep policy for vkDestroyInstance: every surface an instance still owns
// is destroyed implicitly with it, so its window must stop being live even when
// the application never calls vkDestroySurfaceKHR. Selects the windows to
// retire from `surfaces`, a map keyed by surface handle whose values expose
// `.window` and `.instance` (see VulkanLayerState::SurfaceRecord), erases the
// matching entries, and appends one window per surface to `out`. One selection
// per surface is what keeps LiveSurfaceHwndTable's per-HWND refcount balanced
// when several surfaces of the same instance share a window. The caller retires
// the returned windows after its own locks are released, because the bridge's
// spin section must never nest under a layer lock.
template <typename SurfaceMap, typename InstanceT, typename OutputIterator>
size_t SelectWindowsToRetireOnInstanceDestroy(SurfaceMap& surfaces, const InstanceT& destroyedInstance,
                                              OutputIterator out) {
    size_t selected = 0;
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        if (it->second.instance == destroyedInstance) {
            *out++ = it->second.window;
            ++selected;
            it = surfaces.erase(it);
        } else {
            ++it;
        }
    }
    return selected;
}

}  // namespace ce::vulkan_wsi_surfaces
