// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include "layer_wsi_surface_bridge.h"

#include <atomic>

#include "hook/present/vulkan_wsi_surface_table.h"
#include "layer_main.h"
#include "vulkan_flip_metering_bridge.h"

namespace {

// The single table instance lives in the resident layer DLL: the layer is the
// only component that observes Win32 surface lifetimes, and the hook DLL reads
// it through the exported query below. No IPC, file mapping, or allocation is
// involved - both modules share this process's address space.
ce::vulkan_wsi_surfaces::LiveSurfaceHwndTable g_liveSurfaceHwnds;

// Latched for the process, never cleared: a device teardown does not make the
// application's capability request retroactively untrue, and a swapchain the
// generator is about to drive can be created while the device that will run it
// is the only one left.
std::atomic<bool> g_deviceEnabledPresentMetering{false};

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

void PublishDevicePresentMetering() {
    if (!g_deviceEnabledPresentMetering.exchange(true, std::memory_order_release)) {
        LayerLog("Vulkan WSI bridge: published device present metering to the hook DLL - CE's upstream "
                 "Streamline present-mode override stands down for this process");
    }
}

bool AnyDeviceEnabledPresentMetering() {
    return g_deviceEnabledPresentMetering.load(std::memory_order_acquire);
}

}  // namespace ce::vulkan_wsi

extern "C" __declspec(dllexport) BOOL CEVulkanLayerIsLiveVulkanSurfaceHwnd(HWND window) {
    return g_liveSurfaceHwnds.IsLive(window) ? TRUE : FALSE;
}

// Also true while CE's D3D12 flip-metering bridge meters a batch: the generator then owns the display
// placement exactly as an application-enabled one does, and the hook's final-present vertical-blank
// contract has to take over from the per-present FIFO selection the layer stands down from. NVIDIA's
// ICD issues the final DXGI flip from its own thread, so at the edge where frame generation turns on or
// off the flip may still see the previous present's state; within a metered run the two agree. A
// device-wide answer instead would also restate the interval on unmetered flips, which replaces
// variable refresh with a fixed grid (Portal RTX, llm-wiki/vulkan-forced-fifo.md).
extern "C" __declspec(dllexport) BOOL CEVulkanLayerDeviceEnabledPresentMetering(void) {
    return (g_deviceEnabledPresentMetering.load(std::memory_order_acquire) ||
            ce::vulkan_flip_metering_bridge::IsMeteringEngaged())
               ? TRUE
               : FALSE;
}
