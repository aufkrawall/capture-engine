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

// True from the game's accepted slDLSSGSetOptions(OFF) until an explicit enable (or sustained generation
// evidence): DLSS FG activity seen anywhere else must not switch CE's FG state back on meanwhile.
bool HoldsExplicitDLSSGOff();

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

// Release a slDLSSGSetOptions(OFF) held during startup-window churn once its protection ends. A held
// OFF is the title's latest request (any SetOptions(ON) clears it) and is never discarded. Called
// from DetourPresent/DetourPresent1: if the title has marked a frame since the hold, the OFF is left
// to ServiceHeldSetOptionsOffOnTitleFrame; otherwise it is forwarded from here.
//
// When PostSL activation is still pending (startup-handoff Present bypassed the
// synthetic Present path, or the callback is deferred by the startup transition
// window guard), this function also triggers the PostSL callback directly.
void FlushSuppressedSetOptionsOffIfNeeded();

// Called on the title's thread right after its PCL present-start marker reached Streamline: replays a
// held OFF whose protection ended through CE's own slDLSSGSetOptions handling.
void ServiceHeldSetOptionsOffOnTitleFrame();

// Marks PCL markers CE issues itself (the 2.x bridge re-marks a present from inside Streamline's own
// Present hook). They are not the title's frame boundary: no proof is counted and no held OFF is
// replayed there.
class CeIssuedFrameMarkerScope {
public:
    CeIssuedFrameMarkerScope() { ++Depth(); }
    ~CeIssuedFrameMarkerScope() { --Depth(); }

    CeIssuedFrameMarkerScope(const CeIssuedFrameMarkerScope&) = delete;
    CeIssuedFrameMarkerScope& operator=(const CeIssuedFrameMarkerScope&) = delete;

    static bool Active() { return Depth() > 0; }

private:
    static int& Depth() {
        static thread_local int depth = 0;
        return depth;
    }
};

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
