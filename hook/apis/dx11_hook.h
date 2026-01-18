#pragma once
#include "graphics_hook.h"
#include <d3d11.h>
#include <dxgi.h>

class DX11Hook : public GraphicsHook {
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect() override;  // Called when captureengine disconnects
};

// D3D11 function typedefs for IAT patching
typedef HRESULT(WINAPI* PFN_D3D11CreateDeviceAndSwapChain)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
    ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

// Detour function - called when game calls D3D11CreateDeviceAndSwapChain
HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

// Manual Hook Activation (for DXGI/DX12 interop fallbacks)
void DX11Hook_OnSwapChainCreated(IDXGISwapChain* pSwapChain);

// Update metrics for wrapper calls
#include <cstdint>
void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

// Original function pointer (set by IAT patching)
extern PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain;
