#pragma once

#include <windows.h>

namespace ce::system_latency {
struct NativeReport;
}

namespace StreamlineHook {

void Init();
void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath);

// Called from the loader DLL-unload notification (runs UNDER the loader lock
// — atomics/interlocked writes and lightweight logging only). Invalidates
// every hook slot whose patched target or saved original belongs to the
// departing module image, and clears the per-module install/IAT masks so the
// next load of the same name re-hooks the fresh instance. Without this,
// games that unload and reload the Streamline stack when toggling DLSS FG
// leave CE forwarding through trampolines into a dead (and possibly
// re-mapped) module generation (crash 20260612_003407).
void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName);
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

// Guard used while CE explicitly services a third-party overlay Present hook
// from inside a Streamline-originated Present path.  Some overlays query
// Streamline while rendering their own overlay; forwarding those queries back
// into Streamline during Streamline's own Present processing is not re-entrant.
class ExternalOverlayPresentGuard {
public:
    ExternalOverlayPresentGuard();
    ~ExternalOverlayPresentGuard();

    ExternalOverlayPresentGuard(const ExternalOverlayPresentGuard&) = delete;
    ExternalOverlayPresentGuard& operator=(const ExternalOverlayPresentGuard&) = delete;
};

bool IsExternalOverlayPresentGuardActive();
bool IsExternalOverlayPluginLookupGuardReady();

// True when device is the D3D12 device identity most recently accepted by slSetD3DDevice. FFX integrations
// layered through Streamline can expose a command-queue wrapper whose GetDevice returns this identity while
// the registered FFX resources belong to the underlying real D3D12 device.
bool IsAcceptedD3D12Device(IUnknown* device);

// Copies game-owned Streamline PCL simulation/present markers captured by CE.
// The caller correlates them with the independent display-change timeline.
bool QueryPCLLatencyReport(ce::system_latency::NativeReport& report);

}  // namespace StreamlineHook
