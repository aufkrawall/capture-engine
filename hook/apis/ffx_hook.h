#pragma once
#include <windows.h>

// FFX Hook - Hooks AMD FidelityFX API to detect FSR Frame Generation activation
// This provides usage-based detection (vs DLL-based) by detecting when FG
// context is created

namespace FFXHook {

// Initialize FFX hooks when FidelityFX DLLs are detected
// Should be called from LoadLibrary hook when detecting:
//   - amd_fidelityfx_fg.dll
//   - ffx_frameinterpolation_x64.dll
void Init();

// Check if hooks are already installed
bool IsInitialized();

// Cleanup hooks (called during shutdown)
void Shutdown();

// FFX present-callback bridge storage uses a stable context key per configure call.
void* GetPresentCallbackBridgeKey(void* context);

}  // namespace FFXHook
