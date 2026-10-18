#pragma once

// The overlay's API label for every D3D12 overlay path (main, D3D11On12 secondary, FFX, Streamline UI).

#include <windows.h>

#include "hook/runtime/dll_utils.h"
#include "hook/runtime/graphics_api_identity.h"

inline const char* Dx12OverlayApiLabel() {
    // The runtime is mapped before any swapchain presents.
    static const char* const s_label = ce::graphics_api_identity::D3D12Label(
        IsDllFromProject("d3d12.dll", "vkd3d"),
        GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")));
    return s_label;
}
