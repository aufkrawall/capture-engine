#pragma once

#include <unknwn.h>

namespace ce::vulkan_dxgi_fifo {

// Registers only the system-DXGI factory exports with the already-existing
// GetProcAddress router. No IAT scan, synthetic factory, or swapchain wrapper is
// involved.
void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded);

// Called after a real DXGI factory export succeeds. Returns true when the
// narrow Vulkan FIFO path owns this factory and it must remain unwrapped.
bool MaybeInstallFactoryHooks(IUnknown* factory, const char* source);

}  // namespace ce::vulkan_dxgi_fifo
