#pragma once

// Shared between the XeFG and XeLL detour units (xefg_hook.cpp, xell_hook.cpp).

#include <windows.h>

#include <cstdint>

#include "xefg_api.h"

namespace XeFGHook {

// One application frame (xefgSwapChainSetPresentId on the game thread, before the proxy Present): paced first, then
// counted.
void PaceApplicationFrame(ce::xefg_api::Handle handle);
void NoteApplicationFrame(ce::xefg_api::Handle handle, uint32_t presentId);

// Detours the XeLL exports of `module` (xell_hook.cpp).
void InstallXeLLExports(HMODULE module, const char* modulePath);

}  // namespace XeFGHook
