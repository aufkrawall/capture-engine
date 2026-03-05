#pragma once
// AMD Anti-Lag 2 minimal type definitions and inline implementation.
// Based on AMD Anti-Lag 2.0 SDK (MIT License, AMD GPUOpen).
//
// Anti-Lag 2 uses amdxc64.dll (AMD DX12 driver extension) — loaded
// automatically on AMD GPU systems when a DX12 device is created.
// Initialize() fails gracefully on non-AMD systems (E_HANDLE).
//
// Key API (DX12 only):
//   Initialize(&ctx, device)  - obtain driver COM interface
//   Update(&ctx, true, fps)   - limit FPS + insert latency-reduction delay (blocks)
//   DeInitialize(&ctx)        - release resources

// clang-format off
#include <windows.h>
#include <d3d12.h>
// clang-format on
#include <unknwn.h>

namespace AMD {
namespace AntiLag2DX12 {

// COM interface to the AMD Anti-Lag 2 driver extension.
// IID: {44085fbe-e839-40c5-bf38-0ebc5ab4d0a6}
static const GUID IID_IAmdExtAntiLagApi = {
    0x44085fbe, 0xe839, 0x40c5, {0xbf, 0x38, 0x0e, 0xbc, 0x5a, 0xb4, 0xd0, 0xa6}
};

struct IAmdExtAntiLagApi : public IUnknown {
    virtual HRESULT UpdateAntiLagState(void* pData) = 0;
};

// Persistent context — declare once, zero-initialize, pass address to all functions.
struct Context {
    IAmdExtAntiLagApi* pAntiLagAPI = nullptr;
    bool               enabled     = false;
    unsigned int       maxFPS      = 0;
};

// v1 state struct: sets enable/disable and FPS cap
struct APIData_v1 {
    unsigned int uiSize;
    unsigned int uiVersion;
    unsigned int eMode;           // 1 = enabled, 2 = disabled
    const char*  sControlStr;
    unsigned int uiControlStrLength;
    unsigned int maxFPS;
};
// struct size differs by pointer width (8 on x64, 4 on x86)
#ifdef _WIN64
static_assert(sizeof(APIData_v1) == 32, "APIData_v1 layout mismatch (64-bit)");
#else
static_assert(sizeof(APIData_v1) == 24, "APIData_v1 layout mismatch (32-bit)");
#endif

// Forward declaration so Initialize() can call DeInitialize() on failure.
inline ULONG DeInitialize(Context* context);

// Initialize Anti-Lag 2 for the given DX12 device.
// Returns S_OK if Anti-Lag 2 is available on this system.
inline HRESULT Initialize(Context* context, ID3D12Device* device) {
    if (!context || !device || context->pAntiLagAPI)
        return E_INVALIDARG;

    HMODULE hModule = GetModuleHandleA("amdxc64.dll");
    if (!hModule)
        return E_HANDLE;  // Not an AMD GPU or driver not loaded

    typedef HRESULT(__cdecl* PFNAmdExtD3DCreateInterface)(IUnknown*, REFIID, void**);
    auto createFn = reinterpret_cast<PFNAmdExtD3DCreateInterface>(
        GetProcAddress(hModule, "AmdExtD3DCreateInterface"));
    if (!createFn)
        return E_NOINTERFACE;

    HRESULT hr = createFn(device, IID_IAmdExtAntiLagApi, (void**)&context->pAntiLagAPI);
    if (hr == S_OK && context->pAntiLagAPI) {
        // Initialize with disabled state
        APIData_v1 data = {};
        data.uiSize    = sizeof(data);
        data.uiVersion = 1;
        data.eMode     = 2;  // disabled
        hr             = context->pAntiLagAPI->UpdateAntiLagState(&data);
    }
    if (hr != S_OK)
        DeInitialize(context);
    return hr;
}

// Release Anti-Lag 2 resources. Call before destroying the device.
inline ULONG DeInitialize(Context* context) {
    ULONG refCount = 0;
    if (context) {
        if (context->pAntiLagAPI) {
            refCount               = context->pAntiLagAPI->Release();
            context->pAntiLagAPI   = nullptr;
        }
        context->enabled = false;
    }
    return refCount;
}

// Call once per frame before rendering. Blocks to enforce FPS cap.
// enable = true to activate Anti-Lag 2; maxFPS = 0 disables FPS cap.
inline HRESULT Update(Context* context, bool enable, unsigned int maxFPS) {
    if (!context || !context->pAntiLagAPI)
        return E_NOINTERFACE;

    // Only update driver state when settings change
    if (context->enabled != enable || context->maxFPS != maxFPS) {
        context->enabled = enable;
        context->maxFPS  = maxFPS;

        APIData_v1 data = {};
        data.uiSize    = sizeof(data);
        data.uiVersion = 1;
        data.eMode     = enable ? 1 : 2;
        data.maxFPS    = maxFPS;
        context->pAntiLagAPI->UpdateAntiLagState(&data);
    }

    // This nullptr call inserts the latency-reducing delay (and enforces FPS cap)
    HRESULT hr = context->pAntiLagAPI->UpdateAntiLagState(nullptr);
    return (hr == S_OK || hr == S_FALSE) ? S_OK : hr;
}

}  // namespace AntiLag2DX12
}  // namespace AMD
