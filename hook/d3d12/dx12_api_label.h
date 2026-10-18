#pragma once

// The overlay's API label for every D3D12 overlay path (main, D3D11On12 secondary, FFX, Streamline UI).

#include <windows.h>

#include <string>

#include "hook/d3d11/dx11_hook.h"
#include "hook/d3d9/dx9_hook.h"
#include "hook/ddraw/ddraw_hook.h"
#include "hook/runtime/api_translator_modules.h"
#include "hook/runtime/dll_utils.h"
#include "hook/runtime/graphics_api_identity.h"

// A DXUP game's native D3D12 output carries the overlay: name the application's API with the
// translator ("DX9 (DXUP)", "DX11.1 (DXUP)"), not the output's "DX12".
inline std::string Dx12OverlayApiLabel() {
    using ce::api_translator::IsDxupModuleLoaded;
    namespace identity = ce::graphics_api_identity;
    const bool ddraw = IsDxupModuleLoaded("ddraw.dll");
    const bool d3d8 = IsDxupModuleLoaded("d3d8.dll");
    const bool d3d9 = IsDxupModuleLoaded("d3d9.dll");
    const bool d3d11 = IsDxupModuleLoaded("d3d11.dll") || IsDxupModuleLoaded("dxgi.dll");
    if (ddraw || d3d8 || d3d9 || d3d11) {
        const auto frontend = identity::SelectDxupFrontend(ddraw, d3d8, d3d9, d3d11,
                                                           ddraw && WasDirectDrawInterfaceObserved(),
                                                           d3d9 && WasGameD3D9DeviceCreated());
        return identity::DxupFrontendLabel(
            frontend, static_cast<identity::DirectDrawVersion>(DDrawHook_ActiveDirectDrawVersion()),
            DDrawHook_ActiveLegacyD3DVersion(),
            frontend == identity::DxupFrontend::kD3D9 && WasGameD3D9ExDeviceCreated(),
            DX11Hook_HighestApplicationMinor());
    }
    // The runtime is mapped before any swapchain presents.
    static const char* const s_label = identity::D3D12Label(
        IsDllFromProject("d3d12.dll", "vkd3d"),
        GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")));
    return s_label;
}
