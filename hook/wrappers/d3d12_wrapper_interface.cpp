/**
 * D3D12 Wrapper Interface Implementation
 *
 * Compiled with MSVC to avoid MinGW ABI issues with D3D12 interfaces.
 * Provides C-compatible exports that MinGW code can call.
 */

#include "d3d12_wrapper_interface.h"
#include "d3d12_commandqueue_wrap.h"
#include "d3d12_device_wrap.h"

#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <windows.h>

// Helper to check config (basic implementation since we don't link full hook_common)
static bool IsDebugLoggingEnabled()
{
    static bool s_Checked = false;
    static bool s_Enabled = false;
    if (!s_Checked) {
        // Try to read config.ini from CWD
        s_Enabled = GetPrivateProfileIntA("Capture", "DebugLogging", 0, ".\\config.ini") != 0;
        s_Checked = true;
    }
    return s_Enabled;
}

void WrapperLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);

    // Append to file for debugging (only if enabled)
    if (IsDebugLoggingEnabled()) {
        struct stat st = {0};
        // Create logs dir if needed (simple check)
        CreateDirectoryA("logs", NULL);

        FILE* f = fopen("logs\\msvc_debug.log", "a");
        if (f) {
            fprintf(f, "%s\n", buf);
            fclose(f);
        }
    }
}

extern "C" {

// Wrapper GUID for identification - use same GUID as wrapper_base.h for consistency
// {C3D4E5F6-7890-ABCD-EF12-345678901234}
#include "wrapper_base.h"

// #define D3D12_EXPORT __declspec(dllexport)

D3D12_EXPORT HRESULT WINAPI D3D12Wrapper_CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
                                                      REFIID riid, void** ppDevice)
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
    pRealDevice->Release();  // Wrapper took ownership

    // Query for requested interface
    hr = pWrapper->QueryInterface(riid, ppDevice);
    pWrapper->Release();  // QueryInterface AddRef'd if successful

    return hr;
}

D3D12_EXPORT ID3D12Device* WINAPI D3D12Wrapper_WrapDevice(ID3D12Device* pRealDevice)
{
    if (!pRealDevice) return nullptr;

    // Check if already wrapped
    void* test = nullptr;
    if (SUCCEEDED(pRealDevice->QueryInterface(IID_CWrapD3D12Device, &test))) {
        // Already wrapped, return as-is (QI added a ref)
        pRealDevice->AddRef();
        ((IUnknown*)test)->Release();
        return pRealDevice;
    }

    // Create wrapper
    CWrapD3D12Device* pWrapper = new CWrapD3D12Device(pRealDevice);
    return pWrapper;
}

D3D12_EXPORT ID3D12Device* WINAPI D3D12Wrapper_UnwrapDevice(ID3D12Device* pDevice)
{
    if (!pDevice) return nullptr;

    // Try to query for our wrapper interface using the public GUID
    void* pWrapper = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_CWrapD3D12Device, &pWrapper))) {
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

D3D12_EXPORT BOOL WINAPI D3D12Wrapper_IsDeviceWrapped(ID3D12Device* pDevice)
{
    if (!pDevice) return FALSE;

    void* test = nullptr;
    if (SUCCEEDED(pDevice->QueryInterface(IID_CWrapD3D12Device, &test))) {
        ((IUnknown*)test)->Release();
        return TRUE;
    }
    return FALSE;
}

D3D12_EXPORT ID3D12CommandQueue* WINAPI D3D12Wrapper_WrapCommandQueue(ID3D12CommandQueue* pRealQueue,
                                                                      ID3D12Device* pDevice)
{
    if (!pRealQueue) return nullptr;

    // Note: We pass nullptr for device since the C interface doesn't have access to CWrapD3D12Device
    // The command queue wrapper will work but won't have a back-reference to the wrapper device
    CWrapD3D12CommandQueue* pWrapper = new CWrapD3D12CommandQueue(pRealQueue, nullptr);
    return pWrapper;
}

D3D12_EXPORT ID3D12CommandQueue* WINAPI D3D12Wrapper_UnwrapCommandQueue(ID3D12CommandQueue* pQueue)
{
    if (!pQueue) return nullptr;

    // Try to get real queue - we'd need a GUID for command queue too
    // For now just return as-is
    pQueue->AddRef();
    return pQueue;
}

// ============================================================================
// Sampler Override Callback Implementation
// ============================================================================

static PFN_ApplySamplerOverrides g_SamplerOverrideCallback = nullptr;

D3D12_EXPORT void WINAPI D3D12Wrapper_SetSamplerOverrideCallback(PFN_ApplySamplerOverrides callback)
{
    g_SamplerOverrideCallback = callback;
}

void ApplySamplerOverridesInternal(D3D12_SAMPLER_DESC* pDesc)
{
    if (g_SamplerOverrideCallback && pDesc) {
        g_SamplerOverrideCallback(pDesc);
    }
}

}  // extern "C"
