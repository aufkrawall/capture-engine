#pragma once

#include <windows.h>
#include <d3d12.h>

// Reuses a proven native D3D12 device when the same adapter/feature-level request recurs.
namespace ce::streamline_bridge {

bool HasRememberedDeviceSupport(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel);
void RememberCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, void* device);
bool TryReuseCreatedDevice(IUnknown* adapter, D3D_FEATURE_LEVEL featureLevel, REFIID riid,
                           void** ppDevice);
// Whether the cache holds a reference that keeps a device alive past the game's own release.
bool HasRetainedDevice();

}  // namespace ce::streamline_bridge
