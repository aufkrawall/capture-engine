#pragma once
#include "graphics_hook.h"
#define VK_USE_PLATFORM_WIN32_KHR
#include "../../external/volk/volk.h"
#include "../common/capture_base.h"
#include <mutex>
#include <vector>

class VulkanHook : public GraphicsHook {
public:
  void Init() override;
  void Shutdown() override;
  void OnHostDisconnect() override;  // Called when captureengine disconnects
};
