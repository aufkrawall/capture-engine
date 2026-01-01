#pragma once
#include "graphics_hook.h"

// DirectX 8 Hook - captures games using Direct3D 8
// Uses D3D9Ex shared surface wrapper for GPU-efficient capture
class DX8Hook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};
