#pragma once

// ID3D10Device vtable slots CE patches, counted from d3d10.h's ID3D10DeviceVtbl
// (IUnknown occupies 0-2). The DX10 sampler hook used to write slot 9, which is
// ID3D10Device::Draw, so a D3D10 swapchain would have routed every Draw through
// DetourCreateSamplerState10. DX11HookDiscoveryProbeTest calls through this slot
// on a real device to prove it is CreateSamplerState.
namespace ce::d3d10_vtable_slots {

inline constexpr unsigned kDeviceCreateSamplerState = 86;

}  // namespace ce::d3d10_vtable_slots
