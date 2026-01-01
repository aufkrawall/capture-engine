#pragma once
#include "graphics_hook.h"
#include <d3d11.h>
#include <dxgi.h>

class DX11Hook : public GraphicsHook {
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect();  // Called when captureengine disconnects
};
