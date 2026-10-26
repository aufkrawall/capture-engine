#pragma once

// The overlay's API label for every D3D12 overlay path (main, native backend adapter, FFX, Streamline UI).

#include <windows.h>

#include <string>

#include "hook/d3d11/dx11_hook.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/dxup_api_label.h"
#include "hook/runtime/graphics_api_identity.h"
#include "hook/runtime/module_replacement.h"

// A DXUP game's native D3D12 output carries the overlay: name the application's API with the
// translator ("DX9 (DXUP)", "DX11.1 (DXUP)"), not the output's "DX12".
inline std::string Dx12OverlayApiLabel() {
    std::string dxup = DxupOverlayApiLabel(DX11Hook_HighestApplicationMinor());
    if (!dxup.empty())
        return dxup;
    namespace identity = ce::graphics_api_identity;
    // The runtime is mapped (and a custom_dll_paths replacement adopted) before any swapchain
    // presents. The application's d3d12.dll, not the first image of that name: a replacement
    // leaves System32's copy mapped beside vkd3d-proton's.
    static const char* const s_label = [] {
        const HMODULE d3d12 = ce::module_replacement::ApplicationModuleByName("d3d12.dll");
        return identity::D3D12Label(
            d3d12 && !IsModuleInSystem32(d3d12) && ModuleVersionStringContains(d3d12, "vkd3d"),
            GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")));
    }();
    return s_label;
}
