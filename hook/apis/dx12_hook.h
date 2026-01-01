#pragma once
#include "graphics_hook.h"
#include <d3d12.h>
#include <dxgi1_4.h>

class DX12Hook : public GraphicsHook {
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect() override;

  // Provide access to detected addresses if needed, or keep internal.
};
