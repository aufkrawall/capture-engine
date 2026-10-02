#pragma once

#include <windows.h>
#include <d3d12.h>

// Native D3D12 device creation for the Streamline generation bridge.
//
// Split out of streamline_bridge.cpp: that unit owns the import-slot thunks, this one owns
// how a bridged D3D12CreateDevice actually reaches Microsoft's d3d12.dll - adapter
// normalization, reuse of a proven device, the device-lost-class retry, and the logs that
// make a reset attributable.
namespace ce::streamline_bridge {

// Formats an interface id for the log, braces omitted.
void DescribeIid(REFIID iid, char (&text)[40]);

// Creates (or reuses) the game's device with Microsoft's entry point rather than the 2.x
// interposer's. A null `ppDevice` is a capability probe and is answered from a prior proof
// when one exists.
HRESULT CallNativeD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid,
                                    void** ppDevice);

}  // namespace ce::streamline_bridge
