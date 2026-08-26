#pragma once

#include <cstdint>

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

// The resident CE Vulkan layer is stronger startup evidence than vulkan-1.dll
// alone.  It has already negotiated into this process and owns the renderer
// before the injected hook's IPC connection and periodic renderer decision are
// available.  Installing speculative D3D/DXGI hooks in that window can mutate
// the Vulkan driver's private WSI-to-DXGI objects.
inline bool ShouldInstallEarlyD3DDXGIHooks(bool vulkanLayerModuleLoaded) {
    return !vulkanLayerModuleLoaded;
}

// Runtime DLL presence is not proof of D3D12 use. In particular, a legacy
// client or translation front-end can load d3d12.dll through an overlay while
// its non-system D3D9/D3D11 runtime owns rendering. Do not create a speculative
// DX12 device/swapchain in that process; confirmed D3D12 device evidence wins.
inline bool ShouldSuppressSpeculativeDX12Bootstrap(bool d3d12DeviceCreated,
                                                   bool nonSystemD3D11Runtime,
                                                   bool nonSystemD3D9Runtime) {
    return !d3d12DeviceCreated && (nonSystemD3D11Runtime || nonSystemD3D9Runtime);
}

// DX9Hook::Init discovers vtable addresses by creating a synthetic D3D9
// factory and device.  That is safe only for the native runtime.  A Vulkan-
// owned or non-system D3D9 translation runtime may initialize a second global
// renderer (RTX Remix is one such split-runtime topology), so those paths must
// use the real-object IAT/Vulkan interception instead of active probing.
inline bool ShouldBootstrapD3D9Hooks(bool vulkanActive, bool nonSystemD3D9Runtime,
                                     bool hookAlreadyInstalled, bool d3d12ActuallyUsed,
                                     bool d3d11DllLoaded, bool d3d9DllLoaded) {
    return !vulkanActive && !nonSystemD3D9Runtime && !hookAlreadyInstalled &&
           !d3d12ActuallyUsed && !d3d11DllLoaded && d3d9DllLoaded;
}

// Split-renderer applications can keep their explicitly profiled client in
// one process while a direct child owns the final Vulkan swapchain. The child
// inherits layer eligibility only from the host's current source PID or the
// exact profile target published before injection, and that parent executable
// must itself still match the published whitelist. Requiring both PID and name
// proof prevents unrelated Vulkan helpers, launchers, and stale PID reuse from
// broadening an executable-name profile.
inline bool ShouldEnableVulkanLayerForProfile(bool currentProcessWhitelisted,
                                              uint32_t currentParentPid,
                                              uint32_t activeSourcePid,
                                              uint32_t profileTargetPid,
                                              bool parentProcessWhitelisted) {
    if (currentProcessWhitelisted) {
        return true;
    }
    const bool parentIsPublishedTarget =
        currentParentPid != 0 &&
        (currentParentPid == activeSourcePid || currentParentPid == profileTargetPid);
    return parentIsPublishedTarget && parentProcessWhitelisted;
}

// The layer publishes only a child PID whose parent identity passed the proof
// above. The ordinary hook DLL may then initialize process-local graphics
// runtime overrides in that exact renderer without adding its executable name
// to the user's whitelist.
inline bool IsPublishedInheritedRenderer(uint32_t currentProcessPid,
                                         uint32_t publishedRendererPid) {
    return currentProcessPid != 0 && currentProcessPid == publishedRendererPid;
}

// The profiled client remains the capture/config identity in a split process.
// A renderer-side helper hook must never steal sourcePid from that parent.
inline bool ShouldPublishHookAsSource(bool inheritedRenderer) {
    return !inheritedRenderer;
}

// Once a child renderer is known, process-local NGX/Streamline work belongs
// there alone. Before that evidence exists, retain the ordinary single-process
// behavior.
inline bool ShouldApplyProcessLocalRuntimeOverrides(uint32_t currentProcessPid,
                                                    uint32_t publishedRendererPid) {
    return publishedRendererPid == 0 || currentProcessPid == publishedRendererPid;
}

} // namespace ce::vulkan_renderer_policy
