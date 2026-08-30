#pragma once

#include <dxgi.h>
#include <unknwn.h>

namespace ce::vulkan_dxgi_fifo {

// Which authoritative DXGIShared present detour is consulting the policy. Only
// the diagnostic label differs; the decision is identical for both.
enum class FinalPresentVariant {
    kPresent,
    kPresent1,
};

// Registers only the system-DXGI factory exports with the already-existing
// GetProcAddress router. The returned real factory supplies stable system method
// addresses; CE hooks those bodies without modifying any COM vtable.
void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded);

// Called after a real DXGI factory export succeeds. Returns true when the
// narrow Vulkan FIFO path observes this factory and it must remain unwrapped.
bool MaybeInstallFactoryHooks(IUnknown* factory, const char* source);

// The single per-present parameter decision, answered at the top of the one
// authoritative DXGIShared Present/Present1 detour. Gates on the armed/lifecycle
// policy (armed config + DXGIShared::IsVulkanActive + !HookIsShuttingDown) and
// on the registered live-WSI swapchain set, then applies the pure FIFO policy
// in place. Disarmed calls, DXGI_PRESENT_TEST queries, and swapchains that were
// never authorized pass through byte-identical. Lock-free; never blocks, never
// touches a vtable, and never calls a driver API.
void ApplyFinalPresentPolicy(IDXGISwapChain* swapchain, UINT& syncInterval, UINT& flags,
                             FinalPresentVariant variant);

}  // namespace ce::vulkan_dxgi_fifo
