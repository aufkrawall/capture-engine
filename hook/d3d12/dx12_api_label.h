#pragma once

// The overlay's API label for every D3D12 overlay path (main, D3D11On12 secondary, FFX, Streamline UI).

#include <windows.h>

#include <string>

#include "hook/d3d11/dx11_hook.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/dxup_api_label.h"
#include "hook/runtime/graphics_api_identity.h"

// A DXUP game's native D3D12 output carries the overlay: name the application's API with the
// translator ("DX9 (DXUP)", "DX11.1 (DXUP)"), not the output's "DX12".
inline std::string Dx12OverlayApiLabel() {
    std::string dxup = DxupOverlayApiLabel(DX11Hook_HighestApplicationMinor());
    if (!dxup.empty())
        return dxup;
    namespace identity = ce::graphics_api_identity;
    // The runtime is mapped before any swapchain presents.
    static const char* const s_label = identity::D3D12Label(
        IsDllFromProject("d3d12.dll", "vkd3d"),
        GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")));
    return s_label;
}
