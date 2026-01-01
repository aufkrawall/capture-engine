#pragma once
#include "graphics_hook.h"

// DirectDraw/DX6/DX7 Hook - captures games using DirectDraw surfaces
// Handles hybrid DDraw+D3D7 games where both render to same surface
// Uses D3D9Ex wrapper for GPU-efficient capture when possible
// Falls back to staging buffer for pure 2D DDraw games
class DDrawHook : public GraphicsHook {
public:
    void Init() override;
    void Shutdown() override;
    void OnHostDisconnect() override;
};
