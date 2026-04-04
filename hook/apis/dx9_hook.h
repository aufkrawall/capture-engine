#pragma once
#include "graphics_hook.h"

// DX9 Hook - captures games using Direct3D 9/9Ex
// Uses D3D11 interop for shared textures since D3D9 shared surfaces
// aren't compatible with modern encoding pipelines
// Shared Overlay Logic (used by both MinHook and Wrapper)
#include <d3d9.h>

void DX9_PresentBegin(IDirect3DDevice9* device, IDirect3DSurface9*& backBuffer);
void DX9_PresentEnd(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);
void DX9_RegisterInternalHelperDevice(IDirect3DDevice9* device);
void DX9_UnregisterInternalHelperDevice(IDirect3DDevice9* device);
bool IsDXVKD3D9WrapperLoaded();

class DX9InternalBypassScope {
public:
    DX9InternalBypassScope();
    ~DX9InternalBypassScope();

    DX9InternalBypassScope(const DX9InternalBypassScope&) = delete;
    DX9InternalBypassScope& operator=(const DX9InternalBypassScope&) = delete;
};

class DX9Hook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};
