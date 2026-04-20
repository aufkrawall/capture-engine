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

// True once the current DLSS FG comeback was actually activated by an
// OFF->ON slDLSSGSetOptions edge, not merely by a later steady-state enable
// request after a provisional GetState-only activation already surfaced.
bool HasExplicitSetOptionsActivationForCurrentComeback();

// Called by the DX12 FFX handoff path when authoritative FFX runtime traffic
// takes ownership of the swapchain. Clears cached Streamline viewport state so
// stale slDLSSGGetState polling cannot immediately resurrect DLSS FG.
void OnAuthoritativeFFXTakeover();

// Called by the DX12 Streamline handoff path when a fresh authoritative
// runtime-owned Streamline swapchain takeover is observed before DLSS FG has
// actually activated. Arms the same short GetState-only startup suppression
// window that prevents provisional OFF->ON GetState activation from racing
// ahead of the later explicit SetOptions enable.
void OnAuthoritativeStreamlineStartupHandoff();

// Forward or discard any suppressed slDLSSGSetOptions(OFF) call that was buffered
// during startup-window churn once the window is no longer the active blocker.
// Called from periodic check points (DetourPresent, DetourPresent1, GetState,
// etc.) so stale deferred OFF does not linger indefinitely.
//
// When PostSL activation is still pending (startup-handoff Present bypassed the
// synthetic Present path, or the callback is deferred by the startup transition
// window guard), this function also triggers the PostSL callback directly before
// forwarding a genuine OFF edge. If a newer explicit post-FSR comeback is already
// half-armed and the buffered OFF is now just stale startup churn, the stale OFF
// is discarded instead of being replayed into the recovered comeback.
void FlushSuppressedSetOptionsOffIfNeeded();

}  // namespace StreamlineHook
