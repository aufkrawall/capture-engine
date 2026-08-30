// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include "layer_wsi_surface_bridge.h"

#include <atomic>

#include "../common/vulkan_wsi_surface_table.h"
#include "layer_main.h"

namespace {

// The single table instance lives in the resident layer DLL: the layer is the
// only component that observes Win32 surface lifetimes, and the hook DLL reads
// it through the exported query below. No IPC, file mapping, or allocation is
// involved - both modules share this process's address space.
ce::vulkan_wsi_surfaces::LiveSurfaceHwndTable g_liveSurfaceHwnds;

}  // namespace

namespace ce::vulkan_wsi {

void PublishLiveSurfaceHwnd(HWND window) {
    if (!g_liveSurfaceHwnds.Register(window)) {
        // Full table or null window: the surface stays untracked, which keeps
        // authorization fail-closed (swapchains on it are never rewritten).
        static std::atomic<uint32_t> s_publishFailureCount{0};
        const uint32_t count = s_publishFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 10 || (count % 500) == 0) {
            LayerLog("Vulkan WSI bridge: live-HWND table full (window=%p, occurrence #%u); "
                     "its swapchains stay unauthorized",
                     (void*)window, count);
        }
    }
}

void RetireLiveSurfaceHwnd(HWND window) {
    g_liveSurfaceHwnds.Unregister(window);
}

bool IsLiveSurfaceHwnd(HWND window) {
    return g_liveSurfaceHwnds.IsLive(window);
}

}  // namespace ce::vulkan_wsi

extern "C" BOOL CEVulkanLayerIsLiveVulkanSurfaceHwnd(HWND window) {
    return g_liveSurfaceHwnds.IsLive(window) ? TRUE : FALSE;
}
