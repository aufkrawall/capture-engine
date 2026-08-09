#pragma once

// Evidence-based Vulkan renderer decision for the DXGI/D3D hook paths.
//
// A process that merely loads vulkan-1.dll is not necessarily a Vulkan app:
// UE5 titles load it as a transitive dependency even when rendering through
// D3D12 (RoboCop: Rogue City session installed/captureengine/logs/20260809_134642
// reproduced exactly this: the DXGI present path latched "Vulkan active" from
// module presence and bypassed the whole DX12 overlay pipeline).  Hook
// installation and the present/resize paths must therefore treat Vulkan as
// active only when there is real Vulkan ownership (VK_LAYER_CE_overlay active)
// or when vulkan-1.dll is loaded without any D3D usage evidence.

namespace ce::vulkan_renderer_policy {

// D3D usage evidence: a real D3D12 device, a D3D11/10 device, legacy D3D
// modules (d3d9/d3d8/ddraw), or presence of the D3D12/D3D11 runtime DLLs.
// DXVK's d3d11.dll is a Vulkan front-end, so under DXVK only a real D3D12
// device counts as D3D evidence.
inline bool HasD3DUsageEvidence(bool dxvkD3D11WrapperLoaded, bool d3d12DeviceCreated,
                                bool d3d11Or10DeviceCreated, bool legacyD3DLoaded,
                                bool d3d12DllPresent, bool d3d11DllPresent) {
    if (dxvkD3D11WrapperLoaded) {
        return d3d12DeviceCreated;
    }
    return d3d12DeviceCreated || d3d11Or10DeviceCreated || legacyD3DLoaded ||
           d3d12DllPresent || d3d11DllPresent;
}

// Whether Vulkan should be treated as the active renderer for DXGI/D3D hook
// and present-path decisions.
inline bool ShouldTreatVulkanAsActiveRenderer(bool vulkanModuleLoaded, bool vulkanLayerOwned,
                                              bool d3dUsageEvidence) {
    if (vulkanLayerOwned) {
        return true;
    }
    if (!vulkanModuleLoaded) {
        return false;
    }
    return !d3dUsageEvidence;
}

} // namespace ce::vulkan_renderer_policy
