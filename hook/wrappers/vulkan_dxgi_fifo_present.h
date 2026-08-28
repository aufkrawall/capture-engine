#pragma once

#include <unknwn.h>

namespace ce::vulkan_dxgi_fifo {

// Registers only the system-DXGI factory exports with the already-existing
// GetProcAddress router. The returned real factory supplies stable system method
// addresses; CE hooks those bodies without modifying any COM vtable.
void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded);

// Called after a real DXGI factory export succeeds. Returns true when the
// narrow Vulkan FIFO path observes this factory and it must remain unwrapped.
bool MaybeInstallFactoryHooks(IUnknown* factory, const char* source);

}  // namespace ce::vulkan_dxgi_fifo
