#pragma once

#include <windows.h>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

// Resident bridge between the Vulkan layer's Win32 WSI surfaces and the
// capture hook DLL's scoped Vulkan FIFO backstop.
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

}  // namespace ce::vulkan_wsi

// Exported query consumed by the capture hook DLL via GetProcAddress. Returns
// TRUE only while `window` backs at least one live Vulkan Win32 surface.
extern "C" BOOL CEVulkanLayerIsLiveVulkanSurfaceHwnd(HWND window);
