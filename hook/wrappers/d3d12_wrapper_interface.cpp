/**
 * D3D12 Wrapper Interface Implementation
 * 
 * Compiled with MSVC to avoid MinGW ABI issues with D3D12 interfaces.
 * Provides C-compatible exports that MinGW code can call.
 */

#include "d3d12_wrapper_interface.h"
#include "d3d12_device_wrap.h"
#include "d3d12_commandqueue_wrap.h"

// Wrapper GUID for identification
static const GUID IID_CWrapD3D12Device_Check = 
{ 0x12345678, 0xabcd, 0xef12, { 0x34, 0x56, 0x78, 0x90, 0x12, 0x34, 0x56, 0x78 } };

extern "C" {

#define D3D12_EXPORT __declspec(dllexport)

D3D12_EXPORT HRESULT D3D12Wrapper_CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice)
{
    if (!ppDevice) return E_POINTER;
    
    // Create real device first
    ID3D12Device* pRealDevice = nullptr;
    HRESULT hr = D3D12CreateDevice(pAdapter, MinimumFeatureLevel, IID_PPV_ARGS(&pRealDevice));
    
    if (FAILED(hr) || !pRealDevice) {
        return hr;
    }
    
    // Wrap it
    CWrapD3D12Device* pWrapper = new CWrapD3D12Device(pRealDevice);
    pRealDevice->Release(); // Wrapper took ownership
    
    // Query for requested interface
    hr = pWrapper->QueryInterface(riid, ppDevice);
    pWrapper->Release(); // QueryInterface AddRef'd if successful
    
    return hr;
}

D3D12_EXPORT ID3D12Device* D3D12Wrapper_WrapDevice(ID3D12Device* pRealDevice)
{
    if (!pRealDevice) return nullptr;
    
    // Check if already wrapped
    void* test = nullptr;
    if (SUCCEEDED(pRealDevice->QueryInterface(IID_CWrapD3D12Device_Check, &test))) {
        // Already wrapped, return as-is (QI added a ref)
        pRealDevice->AddRef();
        ((IUnknown*)test)->Release();
        return pRealDevice;
    }
    
    // Create wrapper
    CWrapD3D12Device* pWrapper = new CWrapD3D12Device(pRealDevice);
    return pWrapper;
}

D3D12_EXPORT ID3D12Device* D3D12Wrapper_UnwrapDevice(ID3D12Device* pDevice)
{
    if (!pDevice) return nullptr;
    
    // Try to query for our wrapper interface
    void* pWrapper = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_CWrapD3D12Device_Check, &pWrapper))) {
        CWrapD3D12Device* pReal = static_cast<CWrapD3D12Device*>((IUnknown*)pWrapper);
        ID3D12Device* result = pReal->GetReal();
        result->AddRef();
        ((IUnknown*)pWrapper)->Release();
        return result;
    }
    
    // Not wrapped, return as-is
    pDevice->AddRef();
    return pDevice;
}

D3D12_EXPORT BOOL D3D12Wrapper_IsDeviceWrapped(ID3D12Device* pDevice)
{
    if (!pDevice) return FALSE;
    
    void* test = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_CWrapD3D12Device_Check, &test))) {
        ((IUnknown*)test)->Release();
        return TRUE;
    }
    return FALSE;
}

D3D12_EXPORT ID3D12CommandQueue* D3D12Wrapper_WrapCommandQueue(ID3D12CommandQueue* pRealQueue, ID3D12Device* pDevice)
{
    if (!pRealQueue) return nullptr;
    
    // Note: We pass nullptr for device since the C interface doesn't have access to CWrapD3D12Device
    // The command queue wrapper will work but won't have a back-reference to the wrapper device
    CWrapD3D12CommandQueue* pWrapper = new CWrapD3D12CommandQueue(pRealQueue, nullptr);
    return pWrapper;
}

D3D12_EXPORT ID3D12CommandQueue* D3D12Wrapper_UnwrapCommandQueue(ID3D12CommandQueue* pQueue)
{
    if (!pQueue) return nullptr;
    
    // Try to get real queue - we'd need a GUID for command queue too
    // For now just return as-is
    pQueue->AddRef();
    return pQueue;
}

} // extern "C"
