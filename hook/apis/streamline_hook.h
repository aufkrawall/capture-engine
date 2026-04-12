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
// periodic check points (DetourPresent, DetourPresent1, GetState, etc.) to
// ensure deferred OFF signals eventually reach Streamline.
//
// When the startup-handoff Present never triggered PostSL activation (because
// it was promoted to top-level and bypassed the synthetic Present path), this
// function also calls the PostSL callback to complete activation before
// forwarding the suppressed OFF.  This ensures Streamline receives ON before
// OFF even when no synthetic Presents arrive to drive PostSL.
void FlushSuppressedSetOptionsOffIfNeeded();

}  // namespace StreamlineHook
