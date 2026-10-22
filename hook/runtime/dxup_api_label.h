#pragma once

// The overlay's API label for a DXUP game, from any CE overlay path that draws on DXUP's output
// (a D3D11/DXGI swapchain - DXUP's, or Windows' under DXUP's frontend alone - or the native D3D12 one
// beneath). DXUP's legacy frontends render through D3D11, so that swapchain says nothing about the
// game's API: Mirror's Edge (D3D9) on DXUP showed "DX11.1 (DXUP)" (session 20261018_143747), and
// "DX11" with only DXUP's d3d9.dll installed (20261022_162106).

#include <windows.h>

#include <string>

#include "hook/d3d11/dx11_hook.h"
#include "hook/d3d9/dx9_hook.h"
#include "hook/ddraw/ddraw_hook.h"
#include "hook/runtime/api_translator_modules.h"
#include "hook/runtime/graphics_api_identity.h"

// What DxupOverlayApiLabel reads besides the module set, packed: a label cached against the module
// set and this is current.
inline unsigned long long DxupLabelEvidence() {
    return (WasDirectDrawInterfaceObserved() ? 1ull : 0ull) | (WasGameD3D9DeviceCreated() ? 2ull : 0ull) |
           (WasGameD3D9ExDeviceCreated() ? 4ull : 0ull) |
           (static_cast<unsigned long long>(DDrawHook_ActiveDirectDrawVersion() & 0xFF) << 8) |
           (static_cast<unsigned long long>(DDrawHook_ActiveLegacyD3DVersion() & 0xFF) << 16) |
           (static_cast<unsigned long long>(DX11Hook_HighestApplicationMinor() & 0xFF) << 24);
}

// "DX9 (DXUP)", "DX7 (DXUP)", "DX11.1 (DXUP)"; empty when no DXUP module is loaded. `d3d11Minor` is
// the application's D3D11.x for a D3D11 game.
inline std::string DxupOverlayApiLabel(unsigned d3d11Minor) {
    using ce::api_translator::IsDxupModuleLoaded;
    namespace identity = ce::graphics_api_identity;
    const bool ddraw = IsDxupModuleLoaded("ddraw.dll");
    const bool d3d8 = IsDxupModuleLoaded("d3d8.dll");
    const bool d3d9 = IsDxupModuleLoaded("d3d9.dll");
    const bool d3d11 = IsDxupModuleLoaded("d3d11.dll") || IsDxupModuleLoaded("dxgi.dll");
    if (!ddraw && !d3d8 && !d3d9 && !d3d11)
        return std::string();
    const auto frontend = identity::SelectDxupFrontend(ddraw, d3d8, d3d9, d3d11,
                                                       ddraw && WasDirectDrawInterfaceObserved(),
                                                       d3d9 && WasGameD3D9DeviceCreated());
    return identity::DxupFrontendLabel(
        frontend, static_cast<identity::DirectDrawVersion>(DDrawHook_ActiveDirectDrawVersion()),
        DDrawHook_ActiveLegacyD3DVersion(), frontend == identity::DxupFrontend::kD3D9 && WasGameD3D9ExDeviceCreated(),
        d3d11Minor);
}
