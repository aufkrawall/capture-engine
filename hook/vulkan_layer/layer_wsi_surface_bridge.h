#pragma once

#include <windows.h>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

// Resident bridge from the Vulkan layer to the capture hook DLL: two facts the
// layer is the only component to observe, published in this process's address
// space and read back through exports.
//
// The layer publishes every live Vulkan Win32 surface HWND here from its
// vkCreateWin32SurfaceKHR/vkDestroySurfaceKHR hooks and exports one query
// (CEVulkanLayerIsLiveVulkanSurfaceHwnd, see layer.def). The hook DLL resolves
// that export on the swapchain-creation path to authorize only swapchains
// whose target window is a live Vulkan surface.
namespace ce::vulkan_wsi {

// Called by VulkanLayerState::RegisterSurface (per live surface). Refcounted
// per HWND; fail-closed when the bounded table is full.
void PublishLiveSurfaceHwnd(HWND window);

// Called by VulkanLayerState::UnregisterSurface (per destroyed surface).
void RetireLiveSurfaceHwnd(HWND window);

// Lock-free in-layer query (diagnostics/tests).
bool IsLiveSurfaceHwnd(HWND window);

// Called from vkCreateDevice once the application's own extension list has been
// read. A device that enabled VK_NV_present_metering may run a metered frame
// generator, and forcing a vertical-blank-paced present mode onto such a
// swapchain removes the generator's flip scheduling rather than adding a wait
// (see vulkan_present_metering_policy.h). The hook DLL's upstream Streamline
// swapchain override needs the same answer, and it runs above the layer where
// DeviceDispatch is not reachable.
void PublishDevicePresentMetering();

// Lock-free in-layer query (diagnostics/tests).
bool AnyDeviceEnabledPresentMetering();

}  // namespace ce::vulkan_wsi

// Exported query consumed by the capture hook DLL via GetProcAddress. Returns
// TRUE only while `window` backs at least one live Vulkan Win32 surface.
extern "C" BOOL CEVulkanLayerIsLiveVulkanSurfaceHwnd(HWND window);

// Exported query consumed by the capture hook DLL via GetProcAddress. Returns
// TRUE once any Vulkan device in this process was created with
// VK_NV_present_metering in the application's own extension list. Latching is
// deliberate: the capability is a device-lifetime property, and a present mode
// can only be chosen while a swapchain is being created.
extern "C" BOOL CEVulkanLayerDeviceEnabledPresentMetering(void);
