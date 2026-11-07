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
// translator ("DX9 -> DX11 -> DX12 (DXUP)", "DX11.1 -> DX12 (DXUP)"), not the output's "DX12".
inline std::string Dx12OverlayApiLabel() {
    thread_local std::string t_label;
    thread_local unsigned long long t_modules = 0;
    thread_local unsigned t_minor = ~0u;
    thread_local unsigned long long t_evidence = ~0ull;
    const unsigned long long modules = LoadedModuleSetGeneration();
    const unsigned long long evidence = DxupLabelEvidence();
    const unsigned minor = DX11Hook_HighestApplicationMinor();
    if (modules != 0 && modules == t_modules && minor == t_minor && evidence == t_evidence)
        return t_label;

    std::string dxup = DxupOverlayApiLabel(minor, true);
    if (!dxup.empty()) {
        t_label = std::move(dxup);
    } else {
        namespace identity = ce::graphics_api_identity;
        static const char* const s_label = [] {
            const HMODULE d3d12 = ce::module_replacement::ApplicationModuleByName("d3d12.dll");
            return identity::D3D12Label(
                d3d12 && !IsModuleInSystem32(d3d12) && ModuleVersionStringContains(d3d12, "vkd3d"),
                GetModuleHandleA("d3d12core.dll") && (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll")));
        }();
        t_label = s_label;
    }
    t_modules = modules;
    t_minor = minor;
    t_evidence = evidence;
    return t_label;
}
