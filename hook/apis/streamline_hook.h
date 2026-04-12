#pragma once

#include <windows.h>

namespace StreamlineHook {

void Init();
bool IsInitialized();
void Shutdown();

// Returns true when Streamline currently signals DLSS FG runtime activity.
// This is more timely than heuristic-only detection and can be refreshed by
// slDLSSGGetState fallback reconciliation.
bool IsDLSSFGRequestedViaStreamline();

// Called by the DX12 FFX handoff path when authoritative FFX runtime traffic
// takes ownership of the swapchain. Clears cached Streamline viewport state so
// stale slDLSSGGetState polling cannot immediately resurrect DLSS FG.
void OnAuthoritativeFFXTakeover();

// Forward any suppressed slDLSSGSetOptions(OFF) call that was buffered during
// the startup transition window, now that the window has expired.  Called from
// periodic check points (DetourPresent, GetState, etc.) to ensure deferred OFF
// signals eventually reach Streamline.
void FlushSuppressedSetOptionsOffIfNeeded();

}  // namespace StreamlineHook
