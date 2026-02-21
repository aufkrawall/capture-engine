// Test stubs for hook symbols needed by tests
// These provide minimal implementations to satisfy linker requirements

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

// Stubs for dxgi_shared.cpp dependencies
bool IsInWrapperPresent() { return false; }

namespace DXGIShared {
    void HandleDX12ProcessFrame(IDXGISwapChain*, bool) {}
    void HandleDX11ProcessFrame(IDXGISwapChain*, bool) {}
    void HandleDX12ResizeBegin() {}
    void HandleDX11ResizeBegin() {}
    void HandleDX12ResizeEnd() {}
}

// Stubs for InlineHook - need to match actual class definition
// The real InlineHook is a class with static methods, so we provide definitions here
#include "inline_hook.h"

bool InlineHook::Install(void*, void*, void**) { return false; }
void InlineHook::RemoveAll() {}

// Stubs for DX12
extern "C" __declspec(dllexport) void DX12_InvalidateSwapchain() {}
extern "C" __declspec(dllexport) void DX12_ProcessFrameExternal(IDXGISwapChain*) {}
extern "C" __declspec(dllexport) void DX12_OnSwapchainResizeBegin() {}
extern "C" __declspec(dllexport) void DX12_OnSwapchainResizeEnd() {}
extern "C" __declspec(dllexport) void DX12_SetCommandQueue(IUnknown*) {}
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue*) {}

// Stubs for DX11  
extern "C" __declspec(dllexport) void DX11_ProcessFrameExternal(IDXGISwapChain*) {}

// Stubs for custom_overlay_dx12.cpp
HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**) {
    return E_NOTIMPL;
}

// Stubs for freeze_watchdog.cpp
extern "C" BOOL MiniDumpWriteDump(HANDLE, DWORD, HANDLE, int, void*, void*, void*) {
    return FALSE;
}

// Stubs for hook_common.cpp
struct LocalConfig {
    bool debugLogging = false;
};
static LocalConfig g_LocalConfigInstance;
LocalConfig* g_pLocalConfig = &g_LocalConfigInstance;
