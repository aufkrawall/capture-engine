#pragma once
#include "graphics_hook.h"

// DX9 Hook - captures games using Direct3D 9/9Ex
// Uses D3D11 interop for shared textures since D3D9 shared surfaces
// aren't compatible with modern encoding pipelines
class DX9Hook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};
