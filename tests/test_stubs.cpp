// Test stubs for hook symbols needed by tests
// These provide minimal implementations to satisfy linker requirements

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>

// Stubs for dxgi_shared.cpp dependencies
bool IsInWrapperPresent() {
    return false;
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain*, bool) {}
void HandleDX11ProcessFrame(IDXGISwapChain*, bool) {}
void HandleDX12ResizeBegin() {}
void HandleDX11ResizeBegin() {}
void HandleDX12ResizeEnd() {}
}  // namespace DXGIShared

// Stubs for InlineHook - need to match actual class definition
// The real InlineHook is a class with static methods, so we provide definitions here
#include "inline_hook.h"

bool InlineHook::Install(void*, void*, void**) {
    return false;
}
void InlineHook::RemoveAll() {}
void* InlineHook::CreateBypassTrampoline(void*) {
    return nullptr;
}

// Stubs for DX12 - C++ linkage (matching header declarations in dx12_hook.h)
// Note: These are regular C++ functions, not extern "C"
void DX12_InvalidateSwapchain() {}
void DX12_ProcessFrameExternal(IDXGISwapChain*) {}
void DX12_OnSwapchainResizeBegin() {}
void DX12_OnSwapchainResizeEnd() {}
void DX12_SignalFSR4SwapchainRecreated() {}

// DX12_SetCommandQueue is extern "C" in the header
extern "C" void DX12_SetCommandQueue(ID3D12CommandQueue*) {}

// Stubs for DX11
extern "C" void DX11_ProcessFrameExternal(IDXGISwapChain*) {}

// Stubs for custom_overlay_dx12.cpp
HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                    ID3DBlob**) {
    return E_NOTIMPL;
}

// Stubs for freeze_watchdog.cpp
extern "C" BOOL MiniDumpWriteDump(HANDLE, DWORD, HANDLE, int, void*, void*, void*) {
    return FALSE;
}

// Stubs for hook_common.cpp
#include "config.h"
static AppConfig g_LocalConfigInstance{};
AppConfig* g_pLocalConfig = &g_LocalConfigInstance;
bool IsProcessTerminating() {
    return false;
}

bool HookIsPostSLOverlayActiveButUnconfirmed() {
    return false;
}

bool HookIsPostSLOverlayConfirmedRendering() {
    return false;
}

bool HookIsPostSLOverlayConfirmedButStartupSettling() {
    return false;
}

// Stubs for streamline_hook.cpp (StreamlineHook namespace)
namespace StreamlineHook {
void FlushSuppressedSetOptionsOffIfNeeded() {}
}  // namespace StreamlineHook
