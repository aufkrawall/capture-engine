#pragma once
#include "graphics_hook.h"

// OpenGL Hook - captures games using OpenGL
// Uses D3D11 interop via WGL_NV_DX_interop for NVIDIA GPUs
// Falls back to PBO readback for AMD/Intel GPUs
class OpenGLHook : public GraphicsHook {
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect() override;
};
