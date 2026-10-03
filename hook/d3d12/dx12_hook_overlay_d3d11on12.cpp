#include "dx12_hook_internal.h"


void CleanupD3D11On12() {
    // Warm-backend: the x64 descriptor-free adapter is DEVICE-scoped (PSOs,
    // font buffer, vb/ib pool; backbuffer fetched per frame) and survives
    // swapchain teardown so the first present of the next swapchain can draw
    // without a backend rebuild. It is rebuilt only on device/format change

    // (EnsureDescFreeBackendForDeviceAndFormat), x86 Texture2D selection, and
    // DX12Hook::Shutdown. The x86 Texture2D adapter (no DescFree backend
    // tracked) keeps its original swapchain-scoped teardown.
    if (!dx12_hook_g_DescFreeBackend) {
        ShutdownDescFreeBackend("CleanupD3D11On12", true);
    }
    // Clean up SL FG D3D11On12 adapter
    if (dx12_hook_g_SLFGAdapter.IsInitialized()) {
        dx12_hook_g_SLFGAdapter.SetShutdownMode(true);
        dx12_hook_g_SLFGAdapter.Shutdown();
    }
    // Device-level D3D11On12 cleanup happens in g_State.Cleanup()
}
