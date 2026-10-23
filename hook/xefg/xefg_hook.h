#pragma once

// Intel XeSS frame generation (libxess_fg.dll) and XeLL (libxell.dll), observed at their exports.
//
// Every caller reaches XeFG through its exports: a game that links the SDK, and OptiScaler, which resolves them
// with GetProcAddress from its own folder (FGOutput=XeFG) through lookups CE's GetProcAddress hook does not see.
// So CE inline-detours the export entries themselves (leave-to-owner: an entry another tool patched first stays
// its), installed from the module-load notification before the module's first call. Every detour forwards its
// arguments unchanged; CE only records:
// - the swapchain-init scope (creator attribution of the native swapchain XeFG creates, fg_presenter.h), its
//   result and, for InitFromSwapChain, the application swapchain's reference count XeFG requires to be 1;
// - generation on/off (SetEnabled), the generated frames per application frame (SetNumInterpolatedFrames), the UI
//   composition state, the proxy swapchain (GetSwapChainPtr), the XeLL link and the context's destruction;
// - each application frame (SetPresentId, called on the game thread before the proxy Present): the input
//   boundary the FG cadence, the FPS limiter and the application frame rate are measured at.

#include <windows.h>

#include <cstdint>

struct IDXGISwapChain;

namespace XeFGHook {

// From NotifyHookModuleLoaded: installs the detours when `module` is libxess_fg.dll or libxell.dll.
void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath);
// Hook-thread pass: the same for modules mapped before CE (late injection).
void InstallForLoadedModules();

// ce::fg_presenter registered `swapchain` as XeFG's native swapchain (inside an init scope, or by module identity).
void NotePresenterSwapchainRegistered(IDXGISwapChain* swapchain);

struct Status {
    bool moduleHooked = false;
    bool contextAlive = false;
    bool initialized = false;
    bool enabled = false;
    uint32_t interpolatedFrames = 1;
    uint32_t presentId = 0;
    uint64_t applicationFrames = 0;
};
Status GetStatus();

}  // namespace XeFGHook
