#pragma once

// Which Direct3D API a DXGI swapchain belongs to, from the device interfaces it answers. Shared by
// DXGIShared::DetectAPIType (dxgi_shared.cpp) and DetectSwapChainAPITypeForDX11Hook
// (dx11_hook_helpers.cpp); the decision itself is DXGIShared::SelectPrimarySwapChainAPIType.

#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include "common/logging/log_meter.h"
#include "hook/present/dxgi_shared_detail/types_and_state.h"

void HookLog(const char* fmt, ...);

namespace DXGIShared {

inline APIType ClassifySwapChainAPI(IDXGISwapChain* swapChain) {
    if (!swapChain) {
        return APIType::Unknown;
    }
    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;
    unsigned d3d11FeatureLevel = 0;

    ID3D12Device* d3d12Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12Device)) && d3d12Device) {
        d3d12Device->Release();
        hasD3D12Device = true;
    }
    // Always try all three: a D3D10 device answers ID3D11Device too (D3D10-on-D3D11), so stopping
    // at D3D11 misclassifies it; SelectPrimarySwapChainAPIType decides.
    ID3D11Device* d3d11Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11Device)) && d3d11Device) {
        hasD3D11Device = true;
    }
    ID3D10Device* d3d10Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10Device)) && d3d10Device) {
        d3d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d3d10Device1 = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d3d10Device1)) && d3d10Device1) {
            d3d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    // Feature level and module lookups only where both interfaces answer (native D3D10, DXVK D3D10
    // and D3D11): a native D3D11 present pays nothing more.
    const bool dual = !hasD3D12Device && hasD3D11Device && hasD3D10Device;
    if (dual) {
        d3d11FeatureLevel = static_cast<unsigned>(d3d11Device->GetFeatureLevel());
    }
    if (hasD3D11Device) {
        d3d11Device->Release();
    }
    const bool d3d10Runtime = dual && IsD3D10RuntimeModuleLoaded();
    const APIType api = SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device, d3d10Runtime,
                                                      d3d11FeatureLevel);
    if (dual) {
        static ce::log_meter::ChangeGate s_dualLog;
        if (const auto verdict = s_dualLog.Observe(
                ce::log_meter::FieldKey(swapChain, d3d10Runtime, d3d11FeatureLevel, static_cast<int>(api)))) {
            HookLog("[GraphicsAPI] swapchain=%p answers ID3D10Device and ID3D11Device: d3d10Runtime=%d "
                    "featureLevel=0x%X -> %s%s",
                    swapChain, d3d10Runtime ? 1 : 0, d3d11FeatureLevel, api == APIType::D3D10 ? "DX10" : "DX11",
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return api;
}

}  // namespace DXGIShared
