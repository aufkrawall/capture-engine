/**
 * D3DKMT (Display Driver Kernel Mode) Hook Header
 *
 * Hooks the kernel-mode driver interface for universal VRAM reporting control.
 * This is a universal VRAM-reporting override technique.
 */

#ifndef D3DKMT_HOOK_H
#define D3DKMT_HOOK_H

#include <windows.h>

namespace D3DKMTHooks {

// Install D3DKMT hooks
bool Install();

// Set VRAM override values (in bytes)
void SetVramOverride(UINT64 dedicatedBytes, UINT64 sharedBytes);

// Disable VRAM override
void DisableVramOverride();

}  // namespace D3DKMTHooks

#endif  // D3DKMT_HOOK_H
