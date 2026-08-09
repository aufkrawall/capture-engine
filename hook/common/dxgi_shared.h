#pragma once

// Shared DXGI present/swapchain state and the Post-SL overlay callback contract.
//
// This file is the umbrella; the topic headers below hold the declarations.

#include "dxgi_shared_detail/types_and_state.h"
#include "dxgi_shared_detail/streamline_present_routing.h"

namespace DXGIShared {
// Evidence-based Vulkan renderer state, published by CheckAndInstallHooks
// (hook/main_install.cpp) and consulted by the DXGI present/resize paths.
// True only when Vulkan actually owns rendering (VK_LAYER_CE_overlay active,
// or vulkan-1.dll loaded without any D3D usage evidence).  A DX12 UE5 process
// that merely loads vulkan-1.dll as a transitive dependency must NOT disable
// the DXGI present/overlay path.
void SetVulkanActiveForDXGIPresentPath(bool active);
bool IsVulkanActive();
}
