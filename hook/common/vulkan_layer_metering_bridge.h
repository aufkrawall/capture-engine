#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <windows.h>

#include <atomic>

#include "hook_common.h"

// One fact the resident CE Vulkan layer owns and the hook DLL needs above it:
// whether this process's Vulkan device was created with VK_NV_present_metering
// in the application's own extension list.
//
// Forcing a vertical-blank-paced present mode onto a swapchain a metered frame
// generator drives does not add a vertical-blank wait - it takes the generator's
// flip scheduling away. The driver says so itself in `sensors.log`'s
// `[DisplayTiming] nvFlipSchedule(... avgDelayUs=N ...)`, the lead with which
// NVIDIA announces the screen time it scheduled an image for: Portal RTX
// 20260913_184745 measured 6842 us with `vsync_mode=default` against 141 us with
// CE's FIFO override armed, same game and same 3x multi-frame generation minutes
// apart. See hook/vulkan_layer/vulkan_present_metering_policy.h.
//
// The layer publishes at vkCreateDevice and exports the query; this resolves it
// lazily the way every other route reaches the resident layer - GetModuleHandle
// plus GetProcAddress over both arch names, no load, no IPC, no file mapping.
namespace ce::vulkan_layer_bridge {

using DeviceEnabledPresentMeteringFn = BOOL (*)(void);

inline DeviceEnabledPresentMeteringFn ResolvePresentMeteringQuery() {
    HMODULE layer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
    if (!layer)
        layer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
    if (!layer)
        return nullptr;
    return reinterpret_cast<DeviceEnabledPresentMeteringFn>(
        GetProcAddress(layer, "CEVulkanLayerDeviceEnabledPresentMetering"));
}

// Fails *open* - a caller keeps its configured override when no resident layer
// answers - because a process without CE's Vulkan layer has no metered
// generator for that override to disturb.
inline bool DeviceEnabledPresentMetering() {
    static std::atomic<DeviceEnabledPresentMeteringFn> query{ResolvePresentMeteringQuery()};
    DeviceEnabledPresentMeteringFn resolved = query.load(std::memory_order_acquire);
    if (!resolved) {
        resolved = ResolvePresentMeteringQuery();
        query.store(resolved, std::memory_order_release);
    }
    return resolved && resolved() == TRUE;
}

// Whether an upstream present-mode override must stand down, with the reason
// reported once per process. `incomingPresentMode`/`desiredPresentMode` are
// VkPresentModeKHR values, taken as ints so this header needs no Vulkan header.
inline bool MeteredGeneratorOwnsPresentPlacement(int incomingPresentMode, int desiredPresentMode) {
    if (!DeviceEnabledPresentMetering())
        return false;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        HookLogImportant(
            "Vulkan layer bridge: NOT overriding present mode %d -> %d - this process's Vulkan device "
            "enabled VK_NV_present_metering, so the frame generator owns the display placement of its "
            "images and forcing a vertical-blank-paced mode would remove its flip scheduling",
            incomingPresentMode, desiredPresentMode);
    }
    return true;
}

}  // namespace ce::vulkan_layer_bridge
