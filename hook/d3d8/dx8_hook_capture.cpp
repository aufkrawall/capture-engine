#include "dx8_hook_internal.h"

void DX8Hook::Init() {
    HookLog("DX8Hook::Init()");

    // Check if d3d8.dll is loaded
    HMODULE d3d8Module = GetModuleHandleA("d3d8.dll");
    if (!d3d8Module) {
        return;
    }

    TryInstallDirect3DCreate8Hook(d3d8Module);
}

void DX8Hook::Shutdown() {
    HookLog("DX8Hook::Shutdown()");
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D8);

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    dx8_hook_g_DX8Capture.CleanupDX8(true);
}

void DX8Hook::OnHostDisconnect() {
    HookLog("DX8Hook::OnHostDisconnect()");
    dx8_hook_g_DX8Capture.CleanupDX8(true);
}
