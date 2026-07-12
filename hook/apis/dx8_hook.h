#pragma once
#include "graphics_hook.h"

// DirectX 8 Hook - Legacy game support for Direct3D 8 titles.
// Hooks IDirect3DDevice8::Present/Reset by wrapping the D3D8 device.
// Internally creates a D3D9Ex device to provide GPU-efficient shared-surface
// capture, since D3D8 has no native shared handle support.
// Known limitations:
//   - Requires d3d8.dll to be loaded by the game (auto-detected).
//   - Some very old titles may not initialize D3D9Ex properly.
//   - Overlay rendering uses the D3D9 path (not a separate D3D8 overlay).
class DX8Hook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};

void DX8Hook_OnModuleLoaded();
