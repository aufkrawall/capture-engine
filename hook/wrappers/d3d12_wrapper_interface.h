/**
 * D3D12 Wrapper Interface
 * 
 * C-compatible interface for D3D12 wrappers that can be called from MinGW code.
 * The actual wrapper implementation is compiled with MSVC to avoid ABI issues.
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef D3D12_EXPORT
#ifdef _MSC_VER
#define D3D12_EXPORT __declspec(dllexport)
#else
#define D3D12_EXPORT
#endif
#endif

// Wrapper creation - returns wrapped device
// Returns S_OK on success, the wrapped device in *ppWrappedDevice
D3D12_EXPORT HRESULT D3D12Wrapper_CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice);

// Wrap an existing device (used when device is created via other means)
// Returns the wrapped device, or the original if wrapping fails
D3D12_EXPORT ID3D12Device* D3D12Wrapper_WrapDevice(ID3D12Device* pRealDevice);

// Unwrap a potentially wrapped device back to real device
D3D12_EXPORT ID3D12Device* D3D12Wrapper_UnwrapDevice(ID3D12Device* pDevice);

// Check if a device is wrapped
D3D12_EXPORT BOOL D3D12Wrapper_IsDeviceWrapped(ID3D12Device* pDevice);

// Wrap a command queue (called internally when device creates queue)
D3D12_EXPORT ID3D12CommandQueue* D3D12Wrapper_WrapCommandQueue(ID3D12CommandQueue* pRealQueue, ID3D12Device* pDevice);

// Unwrap command queue
D3D12_EXPORT ID3D12CommandQueue* D3D12Wrapper_UnwrapCommandQueue(ID3D12CommandQueue* pQueue);

#ifdef __cplusplus
}
#endif
